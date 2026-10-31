#!/usr/bin/env python3
"""Regression checks for SARIF Unicode columns and URI escaping."""
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build" / ("lmok.exe" if os.name == "nt" else "lmok")

with tempfile.TemporaryDirectory(dir=ROOT / "build") as td:
    work = Path(td)
    pack = work / "rules.json"
    pack.write_text(json.dumps({"lang": "c", "rules": [{
        "id": "N", "on": "number_literal", "message": "number", "severity": "major"
    }]}), encoding="utf-8")
    source = work / "a #雪.c"
    source.write_text('const char *s="雪"; int a=1;\n', encoding="utf-8")
    sarif = work / "out.sarif"
    result = subprocess.run([str(HOST), "lint", str(pack), str(source), "--sarif", str(sarif)], capture_output=True)
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    doc = json.loads(sarif.read_text(encoding="utf-8"))
    assert doc["runs"][0]["columnKind"] == "unicodeCodePoints", doc["runs"][0]
    finding = doc["runs"][0]["results"][0]
    physical = finding["locations"][0]["physicalLocation"]
    assert physical["artifactLocation"]["uri"].endswith("a%20%23%E9%9B%AA.c"), physical
    region = physical["region"]
    # The literal begins at Unicode code-point column 26 (tree-sitter gives
    # byte column 28 because 雪 occupies three UTF-8 bytes).
    assert (region["startColumn"], region["endColumn"]) == (26, 27), region
    # Astral characters and CRLF exercise code points rather than UTF-16
    # units, byte columns, or whole-file character offsets.
    text = 'const char *s="雪😀"; int a=1;\r\nint b=2;\r\n'
    source.write_bytes(text.encode('utf-8'))
    expected = [(1, text.splitlines()[0].index('1') + 1), (2, 7)]
    def regions(path):
        run = json.loads(path.read_bytes())['runs'][0]
        assert run['columnKind'] == 'unicodeCodePoints', run
        return [(r['locations'][0]['physicalLocation']['region']['startLine'],
                 r['locations'][0]['physicalLocation']['region']['startColumn']) for r in run['results']]
    result = subprocess.run([str(HOST), 'lint', str(pack), str(source), '--sarif', str(sarif)], capture_output=True)
    assert result.returncode == 0, result.stderr
    assert regions(sarif) == expected, regions(sarif)
    if os.name == 'nt':
        uri = json.loads(sarif.read_bytes())['runs'][0]['results'][0]['locations'][0]['physicalLocation']['artifactLocation']['uri']
        assert uri.startswith('file:///'), uri

    # The check and CI producers must pass the same source boundary as lint.
    compiler = os.environ.get('LMSCRIPT_BIN', 'lmscript')
    if os.name == 'nt' and not compiler.lower().endswith(('.exe', '.cmd', '.bat')):
        compiler = shutil.which(compiler + '.cmd') or shutil.which('lmscript.cmd') or compiler
    rule, binary = work / 'rule.lms', work / 'rule.dslb'
    rule.write_text('p = _lmcode_ schema=' + json.dumps(str(ROOT / 'schema/lmok.dsl.json')) + ';\n'
                    '@number_literal fn numbers(n: Node) -> void implements Rule { ts.report(n,"number","N","major"); }\n'
                    'end _lmcode_\n', encoding='utf-8')
    compiled = subprocess.run(["node",str(ROOT/"scripts/compile-fixture.mjs"), str(rule), str(binary)], capture_output=True)
    assert compiled.returncode == 0, compiled.stderr
    result = subprocess.run([str(HOST), 'check', '--lang', 'c', str(binary), str(source), '--sarif', str(sarif)], capture_output=True)
    assert result.returncode == 0, result.stderr
    assert regions(sarif) == expected, regions(sarif)
    (work / '.lmok').mkdir()
    (work / '.lmok/config.json').write_text(json.dumps({'version': 1, 'packs': {'c': ['rules.json']},
        'severity_floor': 'major', 'fail_on': 'findings'}), encoding='utf-8')
    result = subprocess.run([str(HOST), 'ci', '--sarif', str(sarif)], cwd=work, capture_output=True)
    assert result.returncode == 1, result.stdout + result.stderr
    assert regions(sarif) == expected, regions(sarif)

    # Colons are valid filename data on POSIX, prohibited by Windows.
    if os.name != 'nt':
        colon = work / "a:b.c"
        colon.write_text("int a=1;\n", encoding="utf-8")
        colon_sarif = work / "colon.sarif"
        result = subprocess.run([str(HOST), "lint", str(pack), str(colon), "--sarif", str(colon_sarif)], capture_output=True)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        colon_uri = json.loads(colon_sarif.read_text(encoding="utf-8"))["runs"][0]["results"][0]["locations"][0]["physicalLocation"]["artifactLocation"]["uri"]
        assert "%3A" in colon_uri, colon_uri
print("review SARIF lint/check/CI Unicode/URI checks passed")
