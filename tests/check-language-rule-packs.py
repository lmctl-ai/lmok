#!/usr/bin/env python3
"""Per-rule fixture, mutation, autofix, and .lmok config controls for S6 packs."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / 'build' / 'lmok'
FIXTURES = ROOT / 'tests/fixtures/language-packs'
PACKS = {'lmscript': ROOT / 'rules/lmscript/modernization.json',
         'lmcode': ROOT / 'rules/lmcode/modernization.json'}
RULE_LANG = {f'LMS{i:03d}': 'lmscript' for i in range(1, 17)}
RULE_LANG['LMC001'] = 'lmcode'
FIXES = {
    'LMS012': ('system(["lmctl", "--dsl"]);\n', 'system(["lmctl", "--lmcode"]);\n'),
    'LMS013': ('system(["lmctl", "--dsl-schema", "--json"]);\n', 'system(["lmctl", "--lmcode-schema", "--json"]);\n'),
    'LMS014': ('system(["lmctl", "--dsl-signatures"]);\n', 'system(["lmctl", "--lmcode-signatures"]);\n'),
}


def finding_rows(output: bytes) -> list[dict]:
    result = []
    for line in output.splitlines():
        try:
            row = json.loads(line)
        except (json.JSONDecodeError, UnicodeDecodeError):
            continue
        if isinstance(row, dict) and 'rule' in row:
            result.append(row)
    return result


def lint(pack: Path, source: Path, *extra: str) -> subprocess.CompletedProcess:
    return subprocess.run([str(HOST), 'lint', str(pack), str(source), *extra],
                          cwd=ROOT, capture_output=True)


for pack in PACKS.values():
    if not pack.is_file():
        raise SystemExit('missing configured pack: ' + str(pack))

with tempfile.TemporaryDirectory(prefix='language-packs-', dir=ROOT / 'build') as temp:
    scratch = Path(temp)
    for rule, lang in RULE_LANG.items():
        pack = PACKS[lang]
        ext = '.lmcode' if lang == 'lmcode' else '.lms'
        positive = FIXTURES / lang / f'{rule}-positive{ext}'
        negative = FIXTURES / lang / f'{rule}-negative{ext}'
        for path in (positive, negative):
            if not path.is_file(): raise SystemExit('missing fixture: ' + str(path))
        good = lint(pack, positive)
        if good.returncode:
            raise SystemExit(f'{rule} positive lint error: {good.stderr.decode(errors="replace")}')
        if rule not in [row['rule'] for row in finding_rows(good.stdout)]:
            raise SystemExit(f'{rule} positive fixture did not fire: {good.stdout.decode(errors="replace")}')
        clean = lint(pack, negative)
        if clean.returncode:
            raise SystemExit(f'{rule} negative lint error: {clean.stderr.decode(errors="replace")}')
        if rule in [row['rule'] for row in finding_rows(clean.stdout)]:
            raise SystemExit(f'{rule} fired on its negative fixture: {clean.stdout.decode(errors="replace")}')

        data = json.loads(pack.read_text(encoding='utf-8'))
        mutated = dict(data)
        mutated['rules'] = [item for item in data['rules'] if item['id'] != rule]
        if len(mutated['rules']) == len(data['rules']):
            raise SystemExit(f'{rule} not present in pack')
        mutant = scratch / f'{rule}-disabled.json'
        mutant.write_text(json.dumps(mutated), encoding='utf-8')
        control = lint(mutant, positive)
        if control.returncode or rule in [row['rule'] for row in finding_rows(control.stdout)]:
            raise SystemExit(f'{rule} disabled-rule control survived: {control.stdout.decode(errors="replace")}{control.stderr.decode(errors="replace")}')
        print(f'PASS {rule}: positive, negative and disabled-rule control')

    for rule, (before, after) in FIXES.items():
        source = scratch / f'{rule}-autofix.lms'
        source.write_text(before, encoding='utf-8')
        fixed = lint(PACKS['lmscript'], source, '--fix')
        if fixed.returncode or source.read_text(encoding='utf-8') != after:
            raise SystemExit(f'{rule} autofix bytes mismatch: {source.read_text(encoding="utf-8")!r}; stderr={fixed.stderr.decode(errors="replace")}')
        original = scratch / f'{rule}-autofix-control.lms'
        original.write_text(before, encoding='utf-8')
        data = json.loads(PACKS['lmscript'].read_text(encoding='utf-8'))
        mutant_data = dict(data)
        mutant_data['rules'] = [item for item in data['rules'] if item['id'] != rule]
        mutant = scratch / f'{rule}-fix-disabled.json'
        mutant.write_text(json.dumps(mutant_data), encoding='utf-8')
        control = lint(mutant, original, '--fix')
        if control.returncode or original.read_text(encoding='utf-8') != before:
            raise SystemExit(f'{rule} autofix-disabled control failed: {control.stderr.decode(errors="replace")}')
        print(f'PASS {rule}: exact autofix and disabled-fix control')

    # Exercise both new packs through the documented root-relative project config.
    project = scratch / 'project'
    (project / '.lmok').mkdir(parents=True)
    (project / 'rules/lmscript').mkdir(parents=True)
    (project / 'rules/lmcode').mkdir(parents=True)
    (project / 'src').mkdir()
    shutil.copyfile(PACKS['lmscript'], project / 'rules/lmscript/modernization.json')
    shutil.copyfile(PACKS['lmcode'], project / 'rules/lmcode/modernization.json')
    shutil.copyfile(FIXTURES / 'lmscript/LMS012-positive.lms', project / 'src/legacy.lms')
    shutil.copyfile(FIXTURES / 'lmcode/LMC001-positive.lmcode', project / 'src/fstring.lmcode')
    config = {'version': 1,
              'packs': {'lmscript': ['rules/lmscript/modernization.json'],
                        'lmcode': ['rules/lmcode/modernization.json']},
              'severity_floor': 'minor', 'fail_on': 'none'}
    (project / '.lmok/config.json').write_text(json.dumps(config), encoding='utf-8')
    configured = subprocess.run([str(HOST), 'ci', '--config', '.lmok/config.json', str(project)],
                                cwd=project, capture_output=True)
    if configured.returncode:
        raise SystemExit('configured project pack lint failed: ' + configured.stdout.decode(errors='replace') + configured.stderr.decode(errors='replace'))
    ids = [row['rule'] for row in finding_rows(configured.stdout)]
    for expected in ('LMS012', 'LMC001'):
        if expected not in ids:
            raise SystemExit(f'.lmok config did not load {expected}: {configured.stdout.decode(errors="replace")}')
    print('PASS .lmok config loads lmscript and lmcode packs')
