// Maintainer-only compiler-library fixture export, never a public language API.
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
const args=process.argv.slice(2);const check=args[0]==='--check';if(check)args.shift();
const [input,output]=args;const source=path.resolve(input);
try {
 const module=process.env.LMCODE_COMPILER_MODULE ?? path.resolve(path.dirname(fs.realpathSync(process.env.LMSCRIPT_BIN)),'../dist/dsl.js');
 const {CompiledDsl}=await import(pathToFileURL(module).href);
 const {LMScript}=await import(pathToFileURL(path.join(path.dirname(module),'lmscript.js')).href);
 const engine=new LMScript();let bytes;
 engine.registerFunction('fixture_capture',p=>{bytes=new Uint8Array(CompiledDsl.bytesOf(p));return null;},1,{isDefault:true});
 let raw=fs.readFileSync(source,'utf8');
 const lines=raw.split('\n');const start=lines.findIndex(l=>/\b\w+\s*=\s*_lmcode_/.test(l));
 const end=lines.findIndex((l,i)=>i>start&&l==='end _lmcode_');if(start<0||end<0)throw Error('fixture requires one lmcode literal');
 const variable=lines[start].match(/(\w+)\s*=\s*_lmcode_/)[1];
 lines[start]=variable+' = _lmcode_ schema='+JSON.stringify(path.resolve('schema/lmok.dsl.json'))+';';
 raw=lines.slice(0,end+1).join('\n');
 const result=await engine.runAsync(raw+'\nfixture_capture('+variable+');',{file:source,noPreflight:true,platform:{cwd:path.dirname(source),env:process.env}});
 if(!result.success){for(const d of result.diagnostics)console.error(`${source}:${d.line}:${d.column}: ${d.message}`);process.exitCode=result.exitCode??2;}
 else if(!check){if(!bytes)throw Error('fixture did not construct program');fs.writeFileSync(output,bytes);}
}catch(e){console.error(`${source}:1:1: ${e.message}`);process.exitCode=2;}
