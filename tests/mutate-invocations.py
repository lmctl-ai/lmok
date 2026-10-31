#!/usr/bin/env python3
"""Real native adapter faults; always restore owned checkout source and binary."""
import os,subprocess,sys,tempfile,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];source=ROOT/'cpp/main.cpp';original=source.read_text();host=ROOT/'build/lmok'
def build():
 p=subprocess.run(['cmake','--build','build','-j2'],cwd=ROOT,capture_output=True)
 assert p.returncode==0,(p.stdout,p.stderr)
def check(binary):return subprocess.run([sys.executable,str(ROOT/'tests/check-invocations.py'),str(binary)],cwd=ROOT,capture_output=True)
try:
 build();p=check(host);assert p.returncode==0,(p.stdout,p.stderr);print('PASS lmok invocation baseline',flush=True)
 mutants=[('lost summary','std::cout<<"{\\"host_summary\\":{\\"part\\":"','if(false)std::cout<<"{\\"host_summary\\":{\\"part\\":"','lost summary'),('reset shared context','results.select(part.index,reg,contract);phase="run";','host.reset();results.select(part.index,reg,contract);phase="run";','shared host/add-order/one process'),('wrong slot','results.select(part.index,reg,contract)','results.select(0,reg,contract)','shared host/add-order/one process')]
 for name,anchor,replacement,diagnostic in mutants:
  assert original.count(anchor)==1,(name,'anchor changed');source.write_text(original.replace(anchor,replacement));build();p=check(host)
  assert p.returncode==1 and diagnostic.encode() in p.stderr,(name,p.stdout,p.stderr)
  print('CAUGHT '+name,flush=True)
 source.write_text(original);build()
 with tempfile.TemporaryDirectory(prefix='lmok-per-file-',dir=ROOT/'build') as tmp:
  wrapper=Path(tmp)/'host';wrapper.write_text('#!'+shutil.which('python3')+'\n'+'''import sys,subprocess,os
host='''+repr(str(host))+'''
a=sys.argv[1:]
if not a or a[0].endswith('schema') or a[0] in ['check','--lmcode-signatures','--version']:sys.exit(subprocess.call([host,*a]))
for i in range(0,len(a),2):
 p=subprocess.run([host,'--lmcode',a[i+1]],env={k:v for k,v in os.environ.items() if k!='LMCODE_INVOCATION_FILE'},capture_output=True)
 sys.stdout.buffer.write(p.stdout);sys.stderr.buffer.write(p.stderr)
 if p.returncode:sys.exit(p.returncode)
''');wrapper.chmod(0o755);p=check(wrapper);assert p.returncode==1 and b'AssertionError' in p.stderr,(p.stdout,p.stderr);print('CAUGHT real per-file process',flush=True)
finally:
 source.write_text(original);build()
