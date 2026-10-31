#!/usr/bin/env python3
"""Regenerate with the source compiler library (RETURN_COMPILER_BIN).

Checked-in DSLB keeps native host gates independent of the compiler rollout.
No bytecode is hand patched: source locations and schema hashes come from the
real compiler. Regeneration is explicit; program construction never launches a host.
"""
import hashlib, json, os, subprocess, tempfile
from pathlib import Path
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
compiler=os.environ['RETURN_COMPILER_BIN']
with tempfile.TemporaryDirectory(dir=ROOT/'build') as scratch:
    wrapper=Path(scratch)/'compile.lms'
    for source in sorted(HERE.glob('*.dsl')):
        wrapper.write_text('p = _lmcode_ schema='+json.dumps(str(ROOT/'schema/lmok.dsl.json'))+';\n'+source.read_text()+'end _lmcode_\n',encoding='utf8',newline='\n')
        subprocess.run(["node",str(ROOT/"scripts/compile-fixture.mjs"),str(wrapper),str(source.with_suffix('.dslb'))],check=True,env={**os.environ,"LMSCRIPT_BIN":compiler})
paths=sorted([*HERE.glob('*.dsl'),*HERE.glob('*.dslb')])
(HERE/'fixtures.lock').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.name+'\n' for p in paths),encoding='utf8',newline='\n')
