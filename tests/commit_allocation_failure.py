"""A failed COMMIT must leave one coherent outcome, live and after restart."""
import argparse,json,os,subprocess,tempfile
from pathlib import Path
from build_support import engine_objects
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('--output',type=Path)
p.add_argument('--sanitize',choices=['address','undefined']);a=p.parse_args()
flags=['-g','-fno-omit-frame-pointer','-fsanitize='+a.sanitize] if a.sanitize else []
wrappers=['-Wl,--wrap=__imp__commit'] if os.name=='nt' else ['-Wl,--wrap=fsync']
with tempfile.TemporaryDirectory(prefix='mvd-commit-allocation-') as directory:
    binary=Path(directory)/('probe.exe' if os.name=='nt' else 'probe')
    subprocess.run(['g++','-std=c++20','-O1' if a.sanitize else '-O2','-fopenmp',*flags,
        str(ROOT/'tests/commit_allocation_failure.cpp'),*engine_objects(ROOT,['-std=c++20','-O1' if a.sanitize else '-O2','-fopenmp',*flags]),*wrappers,'-o',str(binary)],check=True,timeout=180)
    results=[]
    for mode in ['live','wide','shutdown','retry','sync-live','sync-shutdown']:
        data=Path(directory)/mode;data.mkdir()
        run=subprocess.run([str(binary),str(data),mode],capture_output=True,text=True,timeout=120)
        row=dict(mode=mode,exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr)
        if run.returncode==0:
            row['matrix']=json.loads(run.stdout.splitlines()[-1]);assert row['matrix']['violations']==0
            if mode.startswith('sync-'):assert row['matrix']['cases']==row['matrix']['actual_injections']==1
            else:assert row['matrix']['actual_injections']>15 and row['matrix']['cases']==180
        results.append(row)
    report=dict(passed=all(x['exit_code']==0 for x in results),sanitizer=a.sanitize,results=results,
        scope='Two-table COMMIT allocation failure; scan and primary-key query agree, failed recovery remains retryable, immediate shutdown preserves WAL.')
    if a.output:
        a.output.parent.mkdir(parents=True,exist_ok=True)
        a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report),flush=True)
    raise SystemExit(0 if report['passed'] else 1)
