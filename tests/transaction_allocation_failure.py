"""One-shot allocation faults in real DML, preserving earlier transaction work."""
import argparse,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser()
p.add_argument('--output',type=Path)
p.add_argument('--sanitize',choices=['address','undefined'])
p.add_argument('--operation',type=int,choices=range(3))
p.add_argument('--point',type=int)
a=p.parse_args()
assert (a.operation is None)==(a.point is None)
flags=['-g','-fno-omit-frame-pointer','-fsanitize='+a.sanitize] if a.sanitize else []
with tempfile.TemporaryDirectory(prefix='mvd-tx-allocation-') as directory:
    binary=Path(directory)/('probe.exe' if os.name=='nt' else 'probe')
    subprocess.run(['g++','-std=c++20','-O1' if a.sanitize else '-O2','-fopenmp',*flags,
        str(ROOT/'tests/transaction_allocation_failure.cpp'),str(ROOT/'src/engine.cpp'),
        str(ROOT/'src/sql.cpp'),'-o',str(binary)],check=True,timeout=180)
    data=Path(directory)/'data';data.mkdir()
    command=[str(binary),str(data)]
    if a.operation is not None:command += [str(a.operation),str(a.point)]
    run=subprocess.run(command,capture_output=True,text=True,timeout=120)
    report=dict(passed=run.returncode==0,exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr,
        scope='One allocation failure per statement, followed by query, COMMIT and restart. Earlier transaction work must survive.',
        sanitizer=a.sanitize)
    if run.returncode==0:
        report['matrix']=json.loads(run.stdout.splitlines()[-1])
        assert report['matrix']['violations']==0
        if a.operation is None:assert report['matrix']['cases']==480 and report['matrix']['actual_injections']>50
    cache_data=Path(directory)/'cache';cache_data.mkdir()
    cache=subprocess.run([str(binary),str(cache_data),'cache'],capture_output=True,text=True,timeout=45)
    report['cache']=dict(exit_code=cache.returncode,stdout=cache.stdout,stderr=cache.stderr)
    report['passed']=report['passed'] and cache.returncode==0
    if cache.returncode==0:
        report['cache']['matrix']=json.loads(cache.stdout.splitlines()[-1])
        assert report['cache']['matrix']['cases']==80 and report['cache']['matrix']['actual_injections']>=1
    if a.output:
        a.output.parent.mkdir(parents=True,exist_ok=True)
        a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report),flush=True)
    raise SystemExit(0 if report['passed'] else 1)
