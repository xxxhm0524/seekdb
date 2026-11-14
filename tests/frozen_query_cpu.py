"""Reproduce the frozen score fixture, with query-only cgroup CPU snapshots."""
import argparse,json,statistics,sys,time
from pathlib import Path
from docker_score_compare import workload,validate_queries,drop_caches
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'protocol_check'))
from _transport import Session
p=argparse.ArgumentParser();p.add_argument('--reference-image',required=True);p.add_argument('--candidate-image',required=True);p.add_argument('--seed',type=int,required=True);p.add_argument('--rounds',type=int,default=5);p.add_argument('--max-cpu-ratio',type=float,default=1.05);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
a.output.parent.mkdir(parents=True,exist_ok=True);config=json.loads((ROOT/'protocol_check/config.json').read_text(encoding='utf-8'));data=workload(a.seed,10000,1000);rounds=[]
def cpu(session):
 raw=session.docker('exec',session.name,'cat','/sys/fs/cgroup/cpu.stat').stdout.decode();return dict((k,int(v)) for k,v in (line.split() for line in raw.splitlines()))['usage_usec']/1e6
for trial in range(a.rounds):
 result={}
 for version in (['reference','candidate'] if trial%2==0 else ['candidate','reference']):
  drop_caches(True);session=Session(config,a.output.with_suffix('.%d.%s.log'%(trial,version)),image=getattr(a,version+'_image'))
  def sql(text):
   response=session.request(text,'exact');assert response['ok'],(text[:100],response);return response
  try:
   session.start();sql('CREATE TABLE items(id BIGINT PRIMARY KEY,cat VARCHAR(8),v VECTOR(128))')
   for text,count in data[2]:assert sql(text)['affected_rows']==count
   sql('CREATE VECTOR INDEX vi ON items(v)')
   for i in [0,500]:sql(data[4][i][0])
   before_cpu=cpu(session);before=time.perf_counter();answers=[sql(text)['rows'] for text,_ in data[4]];seconds=time.perf_counter()-before;cpu_seconds=cpu(session)-before_cpu
   recall=validate_queries(data,answers,'exact');result[version]=dict(seconds=seconds,cpu_seconds=cpu_seconds,recall=recall)
   session.crash();session.start();assert sql('SELECT id FROM items WHERE id=10000')['rows']==[[10000]]
  finally:session.close()
 rounds.append(result);print(json.dumps(dict(seed=a.seed,round=trial,**result)),flush=True)
medians={v:{field:statistics.median(r[v][field] for r in rounds) for field in ['seconds','cpu_seconds']} for v in ['reference','candidate']};ratios=[r['candidate']['cpu_seconds']/r['reference']['cpu_seconds'] for r in rounds];ratio=medians['candidate']['cpu_seconds']/medians['reference']['cpu_seconds']
regression=ratio>a.max_cpu_ratio and sum(r>a.max_cpu_ratio for r in ratios)>=3
report=dict(passed=True,seed=a.seed,rounds=rounds,medians=medians,cpu_ratio=ratio,cpu_threshold=a.max_cpu_ratio,cpu_regression_reproduced=regression,scope='Exact frozen 10k/128d/1000-query fixture; cold-cache control; five alternating pairs; no production source changes')
a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(seed=a.seed,cpu_ratio=ratio,cpu_regression_reproduced=regression)));raise SystemExit(1 if regression else 0)
