#!/usr/bin/env python3
"""Production controls: baseline green, restored hidden bounds assertion-red."""
from pathlib import Path
import subprocess, sys
ROOT=Path(__file__).resolve().parents[1]
source=ROOT/'cpp/serve.cpp'
original=source.read_text()
host=str(Path(sys.argv[1]).resolve())
# Reintroduce the OS deadline in a portable valid C++ form.
socket_timer='''\n#ifdef _WIN32
  DWORD hidden_deadline = 600000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&hidden_deadline), sizeof hidden_deadline);
#else
  timeval hidden_deadline{}; hidden_deadline.tv_sec=600;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &hidden_deadline, sizeof hidden_deadline);
#endif
'''
cases=[('idle','s.set("idle_timeout_s", Json::of(0));','s.set("idle_timeout_s", Json::of(300));'),
       ('body','s.set("max_body_bytes", Json::of(0));','s.set("max_body_bytes", Json::of(16 * 1024 * 1024));'),
       ('socket','  std::string req = method + ', socket_timer+'  std::string req = method + ')]
def build():
    subprocess.run(['cmake','--build',str(Path(host).parent),'-j2'],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)
def check():
    return subprocess.run([sys.executable,'tests/check-serve-unbounded.py',host],cwd=ROOT,capture_output=True,text=True)
assert check().returncode==0,'baseline must pass'
try:
    for name,before,after in cases:
        assert original.count(before)==1,('mutation anchor',name)
        source.write_text(original.replace(before,after))
        build()
        result=check()
        assert result.returncode!=0 and 'AssertionError' in result.stderr,('survived',name,result.stderr)
        print('CAUGHT hidden '+name+' bound',flush=True)
finally:
    source.write_text(original)
    build()
print('PASS 3 serve bound controls')
