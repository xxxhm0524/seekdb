"""Controlled native pair measurements; never calculate an official E."""
import io,json,os,shutil,subprocess,sys,tarfile,time
from pathlib import Path

ROOT=Path(__file__).resolve().parent
REPO=Path(sys.argv[1]).resolve(strict=True)
BASE='3d6fcd00182cacb6fe1a70b72a9c5759850e9f24'
out=ROOT/'native-evidence/performance';out.mkdir()
env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1')
env.pop('MVD_NATIVE_CGROUP',None)
def run(name,cmd,cwd=ROOT):
    with (out/(name+'.log')).open('wb') as log:r=subprocess.run(cmd,cwd=cwd,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=1800)
    print(json.dumps({'performance_stage':name,'returncode':r.returncode}),flush=True);r.check_returncode()
for name in ('native_ab_e860.py','native_query_pairs.py','native_write_pairs.py','native_resource_pairs.py','benchmark-oracle.cpp'):
    shutil.copyfile(REPO/'bench/e5000'/name,ROOT/name)
if subprocess.run(['git','cat-file','-e',BASE+'^{commit}'],cwd=REPO,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode:
    run('fetch-baseline',['git','fetch','--no-tags','--depth=1','origin',BASE],REPO)
source=ROOT/'performance-baseline-source';source.mkdir()
raw=subprocess.check_output(['git','-c','core.autocrlf=false','archive',BASE,'src','third_party','Makefile'],cwd=REPO)
with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
    for item in archive.getmembers():
        p=(source/item.name).resolve();p.relative_to(source.resolve());assert item.isfile() or item.isdir()
        if item.isdir():p.mkdir(parents=True,exist_ok=True)
        else:p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(archive.extractfile(item).read())
run('baseline-build',['make','-B','-j2','CXX=g++','LDFLAGS=-static'],source)
shutil.copyfile(source/'minivectordb',ROOT/'build/performance-baseline');(ROOT/'build/performance-baseline').chmod(0o755)
run('oracle-build',['g++','-O2','-ffp-contract=off','benchmark-oracle.cpp','-o','build/benchmark-oracle'])
cpus=sorted(os.sched_getaffinity(0));assert len(cpus)>=2
driver,server=cpus[:2];cpu_args=['--driver-cpu',str(driver),'--server-cpu',str(server)]
run('resources',[sys.executable,'native_resource_pairs.py','--output','native-evidence/performance/resources','--rounds','3','--program','baseline=build/performance-baseline','--program','candidate=build/candidate','--ordered-keys']+cpu_args)
for shape,args in [('id',[]),('projection',['--with-body'])]:
    run('queries-'+shape,[sys.executable,'native_query_pairs.py','--output','native-evidence/performance/queries-'+shape,'--baseline','build/performance-baseline','--candidate','build/candidate','--rounds','6']+cpu_args+args)
run('writes',[sys.executable,'native_write_pairs.py','--output','native-evidence/performance/writes','--baseline','build/performance-baseline','--candidate','build/candidate','--rounds','6']+cpu_args)
result={'passed':True,'baseline_commit':BASE,'driver_cpu':driver,'server_cpu':server,'results':{},'scope':'Same native compiler, actual JSONL requests, fixed CPUs not reserved; cgroup memory includes page cache. Raw measurements only, no formal E inferred.'}
for name in ('resources','queries-id','queries-projection','writes'):
    data=json.loads((out/name/'report.json').read_text());assert data['passed'];result['results'][name]={k:v for k,v in data.items() if k not in ('records','samples')}
(out/'report.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({'performance_passed':True,'scope':result['scope']}),flush=True)
