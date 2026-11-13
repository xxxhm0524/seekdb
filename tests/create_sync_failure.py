import argparse,json,os,subprocess,tempfile
from pathlib import Path
from build_support import engine_objects
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('--output',type=Path);a=p.parse_args()
report=dict(passed=False,scope='Actual Engine CREATE/DROP/INDEX with one sync call returning ENOSPC; no power-loss or file-byte tampering.')
with tempfile.TemporaryDirectory(prefix='mvd-create-sync-') as directory:
    binary=Path(directory)/('probe.exe' if os.name=='nt' else 'probe')
    wrappers=['-Wl,--wrap=__imp__commit'] if os.name=='nt' else ['-Wl,--wrap=fsync','-Wl,--wrap=rename']
    subprocess.run(['g++','-std=c++20','-O2','-fopenmp',str(ROOT/'tests/create_sync_failure.cpp'),
        *engine_objects(ROOT,['-std=c++20','-O2','-fopenmp']),*wrappers,'-o',str(binary)],check=True,timeout=120)
    results=[]
    for mode in ['create','drop','index','existing-index','allocation','replace']:
        data=Path(directory)/('data-'+mode);data.mkdir()
        run=subprocess.run([str(binary),str(data),mode],capture_output=True,text=True,timeout=30)
        results.append(dict(mode=mode,exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr))
    report.update(passed=all(x['exit_code']==0 for x in results),results=results)
    if a.output:
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report),flush=True)
    raise SystemExit(0 if report['passed'] else 1)
