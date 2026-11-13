import argparse,json,os,subprocess,tempfile
from pathlib import Path
from build_support import engine_objects
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('--output',type=Path);a=p.parse_args()
names=['__imp__open','__imp__read','_lseeki64'] if os.name=='nt' else ['open','pread','lseek']
with tempfile.TemporaryDirectory(prefix='mvd-startup-io-') as directory:
    binary=Path(directory)/('probe.exe' if os.name=='nt' else 'probe')
    subprocess.run(['g++','-std=c++20','-O2','-fopenmp',str(ROOT/'tests/startup_io_failure.cpp'),
        *engine_objects(ROOT,['-std=c++20','-O2','-fopenmp']),*['-Wl,--wrap='+name for name in names],'-o',str(binary)],check=True,timeout=120)
    data=Path(directory)/'data';data.mkdir()
    run=subprocess.run([str(binary),str(data)],capture_output=True,text=True,timeout=30)
    report=dict(passed=run.returncode==0,exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr,
        scope='Real startup with catalog open/size/read and tail-table size/read returning EIO; no file-byte tampering.')
    if run.returncode==0:report['matrix']=json.loads(run.stdout.splitlines()[-1]);assert report['matrix']['violations']==0
    if a.output:
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report),flush=True);raise SystemExit(0 if report['passed'] else 1)
