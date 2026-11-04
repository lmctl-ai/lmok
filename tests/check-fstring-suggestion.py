#!/usr/bin/env python3
"""Prove the f-string rule fires, then prove the oracle catches a disabled predicate."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / 'build' / 'lmok'
COMPILER = os.environ.get('LMSCRIPT_BIN')
if not COMPILER:
    raise SystemExit('LMSCRIPT_BIN must select the private source compiler')
RULE = ROOT / 'rules/lmcode/prefer-fstring.lms'
FIXTURE = ROOT / 'tests/fixtures/fstring-concat.lms'
EXPECTED = '"rule":"LBC120"'
NEEDLE = 'if (prefix.kind() == "string" && is_str_call(conversion)) {'


def compile_rule(source: Path, output: Path) -> None:
    result = subprocess.run(['node', str(ROOT / 'scripts/compile-fixture.mjs'), str(source), str(output)],
                            cwd=ROOT, capture_output=True, env=dict(os.environ, LMSCRIPT_BIN=COMPILER))
    if result.returncode:
        raise SystemExit('rule compilation failed: ' + result.stderr.decode(errors='replace'))


def lint(bytecode: Path, fixture: Path = FIXTURE) -> str:
    result = subprocess.run([str(HOST), 'check', '--lang', 'lmcode', str(bytecode), str(fixture)],
                            cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        raise SystemExit('rule check failed: ' + result.stderr + result.stdout)
    return result.stdout


with tempfile.TemporaryDirectory(prefix='fstring-suggestion-', dir=ROOT / 'build') as temp:
    scratch = Path(temp)
    normal = scratch / 'normal.dslb'
    compile_rule(RULE, normal)
    output = lint(normal)
    if EXPECTED not in output or '1 findings in ' not in output:
        raise SystemExit('f-string suggestion missing from control fixture: ' + output)

    standalone=lint(normal,ROOT/'tests/fixtures/fstring-concat.lmcode')
    if EXPECTED in standalone or '0 findings in ' not in standalone: raise SystemExit('LBC120 leaked into standalone lmcode: '+standalone)

    source = RULE.read_text(encoding='utf-8')
    if source.count(NEEDLE) != 1:
        raise SystemExit('mutation anchor changed')
    mutant = scratch / 'mutant.lms'
    mutant.write_text(source.replace(NEEDLE, 'if (false) {', 1), encoding='utf-8')
    mutant_binary = scratch / 'mutant.dslb'
    compile_rule(mutant, mutant_binary)
    changed = lint(mutant_binary)
    if EXPECTED in changed or '0 findings in ' not in changed:
        raise SystemExit('disabled-rule control was not caught: ' + changed)
    scope = scratch / 'scope-mutant.lms'
    scope.write_text(source.replace('in_lmscript_body(n) && ', '', 1), encoding='utf-8')
    scope_binary = scratch / 'scope.dslb'
    compile_rule(scope, scope_binary)
    leaked = lint(scope_binary, ROOT / 'tests/fixtures/fstring-concat.lmcode')
    if EXPECTED not in leaked: raise SystemExit('removed-scope control did not reproduce standalone advice')
    print('PASS body-only f-string suggestion; CAUGHT disabled-predicate and removed-scope controls')
