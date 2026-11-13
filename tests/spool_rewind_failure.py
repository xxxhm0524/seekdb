"""Physical rewind succeeds, sync fails, then the same offsets are reused."""
import argparse,json,os,subprocess,tempfile
from pathlib import Path
from build_support import engine_objects
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('--output',type=Path);p.add_argument('--sanitize',choices=['address','undefined']);a=p.parse_args()
flags=['-g','-fno-omit-frame-pointer','-fsanitize='+a.sanitize] if a.sanitize else []
wrappers=['__imp__commit','__imp__write'] if os.name=='nt' else ['fsync','write']
with tempfile.TemporaryDirectory(prefix='mvd-rewind-fault-') as directory:
    binary=Path(directory)/('probe.exe' if os.name=='nt' else 'probe')
    subprocess.run(['g++','-std=c++20','-O1' if a.sanitize else '-O2','-fopenmp',*flags,str(ROOT/'tests/spool_rewind_failure.cpp'),*engine_objects(ROOT,['-std=c++20','-O1' if a.sanitize else '-O2','-fopenmp',*flags]),*['-Wl,--wrap='+n for n in wrappers],'-o',str(binary)],check=True,timeout=180)
    results=[]
    for mode in ['matrix','commit','rollback','kill','kill-commit']:
        data=Path(directory)/mode;data.mkdir()
        if mode.startswith('kill'):
            proc=subprocess.Popen([str(binary),str(data),mode],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            first=proc.stdout.readline();marker=proc.stdout.readline()
            if marker=='kill-ready\n':
                proc.kill();proc.wait(timeout=10)
                verify='verify-full' if mode=='kill-commit' else 'verify-empty'
                check=subprocess.run([str(binary),str(data),verify],capture_output=True,text=True,timeout=20)
                code=check.returncode;stderr=check.stderr
            else:
                code=1;proc.wait(timeout=10);stderr=proc.stderr.read()
            proc.stdin.close();proc.stdout.close();proc.stderr.close()
            row=dict(mode=mode,exit_code=code,stdout=first,stderr=stderr,kill_marker_observed=marker=='kill-ready\n',killed_process_exit_code=proc.returncode)
        else:
            run=subprocess.run([str(binary),str(data),mode],capture_output=True,text=True,timeout=45)
            row=dict(mode=mode,exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr)
            if mode=='matrix' and run.returncode==0:
                row['matrix']=json.loads(run.stdout.splitlines()[-1]);assert row['matrix']==dict(cases=112,violations=0)
        results.append(row)
    report=dict(passed=all(r['exit_code']==0 for r in results),sanitizer=a.sanitize,results=results,scope='Real spool and Engine failures; earlier transaction work, reused offsets, normal and forced restart. No file-byte tampering.')
    if a.output:
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report),flush=True);raise SystemExit(0 if report['passed'] else 1)
