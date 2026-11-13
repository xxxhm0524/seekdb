import argparse,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('--output',type=Path);a=p.parse_args()
names=['__imp__write','_lseeki64'] if os.name=='nt' else ['write','pwrite']
with tempfile.TemporaryDirectory(prefix='mvd-io-progress-') as directory:
    binary=Path(directory)/('probe.exe' if os.name=='nt' else 'probe')
    subprocess.run(['g++','-std=c++20','-O2',str(ROOT/'tests/file_io_progress.cpp'),*['-Wl,--wrap='+n for n in names],'-o',str(binary)],check=True,timeout=45)
    run=subprocess.run([str(binary),directory],capture_output=True,text=True,timeout=10)
    report=dict(passed=run.returncode==0,exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr)
    if run.returncode==0:report['matrix']=json.loads(run.stdout.splitlines()[-1]);assert report['matrix']['violations']==0
    if a.output:
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report));raise SystemExit(0 if report['passed'] else 1)
