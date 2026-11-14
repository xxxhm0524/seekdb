"""Alternating same-host phase measurements, including child CPU, over JSONL."""
import argparse,ctypes,json,os,statistics,sys,tempfile,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tests'))
from protocol_client import Client
import numpy as np
p=argparse.ArgumentParser();p.add_argument('--reference');p.add_argument('--candidate');p.add_argument('--reference-image');p.add_argument('--candidate-image');p.add_argument('--rounds',type=int,default=5);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
assert bool(a.reference_image)==bool(a.candidate_image)
assert a.reference_image or (a.reference and a.candidate)
class ImageClient:
 def __init__(self,image,directory):
  sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'protocol_check'))
  from _transport import Session
  config=json.loads((Path(__file__).resolve().parents[1]/'protocol_check/config.json').read_text(encoding='utf-8'))
  self.session=Session(config,Path(directory)/'stderr.log',image=image);self.start()
 def start(self):self.session.start()
 def request(self,sql):return self.session.request(sql)
 def kill(self):self.session.crash()
 def close(self):self.session.close()
 def child_cpu(self):
  raw=self.session.docker('exec',self.session.name,'cat','/sys/fs/cgroup/cpu.stat').stdout.decode()
  return dict((k,int(v)) for k,v in (line.split() for line in raw.splitlines()))['usage_usec']/1e6
def cpu(client):
 if hasattr(client,'child_cpu'):return client.child_cpu()
 if os.name=='nt':
  from ctypes import wintypes
  values=[wintypes.FILETIME() for _ in range(4)]
  call=ctypes.WinDLL('kernel32',use_last_error=True).GetProcessTimes;call.argtypes=[wintypes.HANDLE,*([ctypes.POINTER(wintypes.FILETIME)]*4)];call.restype=wintypes.BOOL
  assert call(int(client.p._handle),*[ctypes.byref(v) for v in values])
  return sum((v.dwHighDateTime<<32)|v.dwLowDateTime for v in values[2:])*1e-7
 fields=Path('/proc/%d/stat'%client.p.pid).read_text().split();return (int(fields[13])+int(fields[14]))/os.sysconf('SC_CLK_TCK')
vectors=np.random.default_rng(20261114).random((10000,128),dtype=np.float32)
batches=['INSERT INTO items VALUES '+','.join("(%d,'c%d','[%s]')"%(i+1,i%10,','.join('%.9g'%float(x) for x in vectors[i])) for i in range(start,start+100)) for start in range(0,10000,100)]
results=[]
for trial in range(a.rounds):
 for version in (['reference','candidate'] if trial%2==0 else ['candidate','reference']):
  with tempfile.TemporaryDirectory(prefix='mvd-phase-') as directory:
   c=ImageClient(getattr(a,version+'_image'),directory) if a.reference_image else Client(binary=str(Path(getattr(a,version)).resolve()),datadir=directory)
   phases={}
   def measure(name,action):
    before=cpu(c);wall=time.perf_counter();action();phases[name]=dict(wall_seconds=time.perf_counter()-wall,cpu_seconds=cpu(c)-before)
   def run(sql,ok=True):
    value=c.request(sql);assert value['ok'] is ok,(sql[:60],value);return value
   try:
    run('CREATE TABLE items(id BIGINT PRIMARY KEY,cat VARCHAR(8),v VECTOR(128))')
    run('CREATE TABLE cold_items(id BIGINT PRIMARY KEY,cat VARCHAR(8),v VECTOR(128))')
    measure('cold_autocommit_insert',lambda:[run(sql.replace('items','cold_items',1)) for sql in batches])
    measure('parse_missing_table',lambda:[run(sql.replace('items','absent',1),False) for sql in batches])
    run('BEGIN')
    measure('stage_insert',lambda:[run(sql) for sql in batches])
    measure('commit',lambda:run('COMMIT'))
    measure('parse_duplicate',lambda:[run(sql,False) for sql in batches])
    run('CREATE TABLE auto_items(id BIGINT PRIMARY KEY,cat VARCHAR(8),v VECTOR(128))')
    measure('autocommit_insert',lambda:[run(sql.replace('items','auto_items',1)) for sql in batches])
    rows=run('SELECT id FROM items ORDER BY id')['rows'];assert rows==[[i] for i in range(1,10001)]
    c.kill();c.start();assert run('SELECT id FROM items WHERE id=10000')['rows']==[[10000]]
   finally:c.close()
   row=dict(round=trial,version=version,phases=phases);results.append(row);print(json.dumps(row),flush=True)
medians={v:{phase:{metric:statistics.median(row['phases'][phase][metric] for row in results if row['version']==v) for metric in ['wall_seconds','cpu_seconds']} for phase in results[0]['phases']} for v in ['reference','candidate']}
report=dict(passed=True,results=results,medians=medians,cpu_ratios={phase:medians['candidate'][phase]['cpu_seconds']/max(.000001,medians['reference'][phase]['cpu_seconds']) for phase in medians['reference']},scope='CPU localization, not official timing or score; container cgroup CPU includes symmetric stats exec overhead; no concurrent builds during measurements')
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(medians=medians)))
