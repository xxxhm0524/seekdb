import argparse,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('--output',type=Path);p.add_argument('--sanitize',choices=['address','undefined']);a=p.parse_args()
flags=['-g','-fno-omit-frame-pointer','-fsanitize='+a.sanitize] if a.sanitize else []
with tempfile.TemporaryDirectory(prefix='mvd-parallel-fault-') as directory:
    binary=Path(directory)/('probe.exe' if os.name=='nt' else 'probe')
    subprocess.run(['g++','-std=c++20','-O1' if a.sanitize else '-O2','-fopenmp',*flags,str(ROOT/'tests/parallel_query_failure.cpp'),str(ROOT/'src/engine.cpp'),str(ROOT/'src/sql.cpp'),'-o',str(binary)],check=True,timeout=180)
    results=[]
    for mode in ['heap','indexed']:
        data=Path(directory)/mode;data.mkdir()
        run=subprocess.run([str(binary),str(data),mode],capture_output=True,text=True,timeout=20)
        results.append(dict(mode=mode,exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr))
    report=dict(passed=all(row['exit_code']==0 for row in results),results=results)
    if a.output:
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report),flush=True);raise SystemExit(0 if report['passed'] else 1)
