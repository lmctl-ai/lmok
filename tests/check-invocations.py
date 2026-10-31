#!/usr/bin/env python3
"""Real public single/builder/check flows against the production native adapter."""
import json,os,shlex,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def check(host):
 host=str(Path(host).resolve())
 with tempfile.TemporaryDirectory(prefix='lmok-invocation-',dir=ROOT/'build') as temp:
  work=Path(temp).resolve();target=work/'水 input.json';target.write_text('{"a":1,"b":2}')
  sarif=work/'result.sarif';schema=work/'other.json';other=json.loads((ROOT/'schema/lmok.dsl.json').read_text());other['name']='other';schema.write_text(json.dumps(other))
  def q(v):return json.dumps(str(v),ensure_ascii=False)
  source=f'''s=load_schema(cli={q(shlex.quote(host))});
p=_lmcode_ s;
let tree = ts.parse("{{}}");ts.report(tree.root(),"first","R_FIRST","warning");$return=ts.report_count();
end _lmcode_
q=_lmcode_ s;
let tree = ts.parse("{{}}");ts.report(tree.root(),"second","R_SECOND","warning");$return=ts.report_count();
end _lmcode_
r=run_lmcode(p);assert(r.ok && r.result==1,"single");
b=lmcode_build(s);b.add(p,["--last","$DSLFILE"]);b.add(q,"--first $DSLFILE");
for(i in range(2)){{r=run_lmcode(b);assert(r.ok&&r.parts==2&&r.result[0]==2&&r.result[1]==1,"shared host/add-order/one process");}}
d=lmcode_build(s);d.add(p);d.add(p);r=run_lmcode(d);assert(r.ok&&r.result[0]==1&&r.result[1]==2,"duplicate slots");
one=lmcode_build(s);one.add(p);r=run_lmcode(one);assert(r.ok&&r.parts==1&&r.result[0]==1,"one slot builder");
bad=_lmcode_ s;
$return=99;let tree=ts.parse("{{}}");tree.root().child(999);
end _lmcode_
fail=lmcode_build(s);fail.add(p);fail.add(bad);r=run_lmcode(fail);assert(!r.ok&&r.phase=="run"&&r.part==1&&!r.has("result")&&!r.has("parts"),"part failure");
r=run_lmcode(bad);assert(!r.ok&&!r.has("part")&&!r.has("result"),"single failure");
other=load_schema(schema={q(schema)});
wrong=_lmcode_ other;
$return=null;
end _lmcode_
load=lmcode_build(s);load.add(p);load.add(wrong);r=run_lmcode(load);assert(!r.ok&&r.phase=="load"&&r.part==1&&!r.has("result"),"load part");
rule=_lmcode_ s;
$return.body="kept";
@pair fn visit(n: Node) -> void implements Rule {{ $return.last=n.text();ts.report(n,"first","R_FIRST","warning");ts.report(n,"second","R_SECOND","error"); }}
end _lmcode_
r=run_lmcode(rule,{{command:[{q(host)},"check","$DSLFILE",{q(target)},"--sarif",{q(sarif)}]}});
assert(r.ok&&r.result.body=="kept"&&r.result.last=="\\\"b\\\":2","check sink");
r=run_lmcode(rule,{{command:[{q(host)},"check","$DSLFILE",{q(target)},"--sarif",{q(work)}]}});
assert(!r.ok&&r.phase=="app"&&!r.has("result")&&!r.has("part"),"global SARIF failure");
global_builder=lmcode_build([{q(host)},"check"]);global_builder.add(rule,["$DSLFILE"]);
r=run_lmcode(global_builder,{{args:[{q(target)},"--sarif",{q(work)}]}});
assert(!r.ok&&r.phase=="app"&&!r.has("part")&&!r.has("result")&&!r.has("parts"),"builder global SARIF failure");
println("PASS real lmok invocation integration");
'''
  file=work/'real.lms';file.write_text(source)
  p=subprocess.run([os.environ.get('LMSCRIPT_BIN','lmscript'),'--no-preflight',str(file)],cwd=ROOT,capture_output=True)
  assert p.returncode==0 and b'PASS real lmok invocation integration' in p.stdout,(p.returncode,p.stdout,p.stderr)
  for line in p.stdout.splitlines():
   if line.startswith(b'{"host_summary":'):assert set(json.loads(line))=={'host_summary'}
  assert p.stdout.count(b'"host_summary":')==13,('lost summary',p.stdout)
  groups=[json.loads(line) for line in p.stdout.splitlines() if line.startswith(b'[')]
  assert [[x['rule'] for x in g] for g in groups]==[['R_FIRST','R_SECOND']*2]*3,groups
  run=json.loads(sarif.read_text())['runs'][0]
  assert [r['ruleId'] for r in run['results']]==['R_FIRST','R_SECOND']*2,run
  assert [r['id'] for r in run['tool']['driver']['rules']]==['R_FIRST','R_SECOND'],run
  print('PASS real lmok single/builder/reuse/roles/part/global/check/findings/SARIF flows')
if __name__=='__main__':check(sys.argv[1])
