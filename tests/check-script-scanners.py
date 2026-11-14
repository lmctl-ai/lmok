#!/usr/bin/env python3
"""S4 build control: actual pinned parser links only with its external scanner."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
cc = shlex.split(os.environ.get('CC', 'cc'))
if not shutil.which(cc[0]):
    raise SystemExit('C compiler required for scanner build control: ' + cc[0])
(ROOT / 'build').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='scanner control ', dir=ROOT / 'build') as directory:
    work = Path(directory)
    for lang in ('lmscript', 'lmcode', 'cpp'):
        grammar = ROOT / 'third_party' / ('tree-sitter-' + lang) / 'src'
        main = work / (lang + '.c')
        main.write_text('struct TSLanguage;\nextern const struct TSLanguage *tree_sitter_' + lang + '(void);\n'
                        'int main(void) { return tree_sitter_' + lang + '() == 0; }\n', encoding='utf-8')
        output = work / (lang + ('.exe' if os.name == 'nt' else ''))
        base = [*cc, str(grammar / 'parser.c'), str(main), '-o', str(output)]
        good = subprocess.run([*base, str(grammar / 'scanner.c')], capture_output=True)
        assert good.returncode == 0, good.stderr.decode(errors='replace')
        assert subprocess.run([str(output)], capture_output=True).returncode == 0
        bad = subprocess.run(base, capture_output=True)
        assert bad.returncode != 0, 'scanner omission unexpectedly linked: ' + lang
        diagnostic = (bad.stdout + bad.stderr).decode(errors='replace')
        assert 'tree_sitter_' + lang + '_external_scanner_' in diagnostic, diagnostic
        print('CAUGHT missing ' + lang + ' scanner: language-specific unresolved link symbols')
