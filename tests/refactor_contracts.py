import argparse,json,os,subprocess,tempfile
from pathlib import Path
from build_support import engine_objects
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('--output',type=Path);p.add_argument('--sanitize',choices=['address','undefined']);a=p.parse_args()
flags=['-std=c++20','-O1' if a.sanitize else '-O2','-fopenmp']
if a.sanitize:flags+=['-g','-fno-omit-frame-pointer','-fsanitize='+a.sanitize]
objects=engine_objects(ROOT,flags);results=[]
with tempfile.TemporaryDirectory(prefix='mvd-refactor-contract-') as directory:
    for name in ['workspace_contract','lifecycle_contract','binding_contract']:
        binary=Path(directory)/(name+('.exe' if os.name=='nt' else ''))
        wrappers=['__imp__commit','__imp__write'] if os.name=='nt' else ['fsync','write']
        links=['-Wl,--wrap='+n for n in wrappers] if name=='workspace_contract' else []
        subprocess.run(['g++',*flags,str(ROOT/'tests'/(name+'.cpp')),*objects,*links,'-o',str(binary)],check=True,timeout=120)
        data=Path(directory)/(name+'-data');data.mkdir()
        run=subprocess.run([str(binary),str(data)],capture_output=True,text=True,timeout=30)
        results.append(dict(name=name,exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr))
report=dict(passed=all(row['exit_code']==0 for row in results),results=results,sanitizer=a.sanitize)
if a.output:a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report),flush=True);raise SystemExit(0 if report['passed'] else 1)
