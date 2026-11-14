"""Actual cross-page text exceeds twice the enforced container memory budget."""
import argparse,json,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'protocol_check'))
from _transport import Session
p=argparse.ArgumentParser();p.add_argument('--image',required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
a.output.parent.mkdir(parents=True,exist_ok=True);config=json.loads((ROOT/'protocol_check/config.json').read_text(encoding='utf-8'));config['memory']='128m'
session=Session(config,a.output.with_suffix('.stderr.log'),image=a.image);limit=128*1024*1024;stages=[];report=dict(passed=False,memory_limit_bytes=limit,rows=1100,effective_text_bytes=1100*65535*4,stages=stages)
def snapshot(stage,seconds=None):
 raw=session.docker('exec',session.name,'cat','/sys/fs/cgroup/memory.max','/sys/fs/cgroup/memory.peak','/sys/fs/cgroup/memory.events').stdout.decode().splitlines()
 events=dict((k,int(v)) for k,v in (line.split() for line in raw[2:]));assert int(raw[0])==limit and events['oom']==events['oom_kill']==0,(stage,raw)
 value=dict(stage=stage,memory_peak_bytes=int(raw[1]),memory_events=events)
 if seconds is not None:value['seconds']=seconds
 stages.append(value)
def sql(text,**expected):
 before=time.perf_counter();response=session.request(text);elapsed=time.perf_counter()-before;assert response['ok'],(text[:100],response)
 for key,value in expected.items():assert response[key]==value,(text[:100],key)
 return elapsed
try:
 assert report['effective_text_bytes']>2*limit
 session.start();sql('CREATE TABLE t(s VARCHAR(65535),id BIGINT PRIMARY KEY,v VECTOR(2))');text='x'+'😀'*65534
 sql('BEGIN')
 for i in range(1,1101):sql("INSERT INTO t VALUES ('%s',%d,[%d,0])"%(text,i,i),affected_rows=1)
 assert session.docker('exec',session.name,'test','-f','/data/txn.tmp',check=False).returncode==0
 snapshot('loaded uncommitted')
 snapshot('cross-page commit',sql('COMMIT'))
 report['logical_table_bytes']=int(session.docker('exec',session.name,'stat','-c','%s','/data/t_1.dat').stdout);assert report['logical_table_bytes']>2*limit
 session.crash();session.start()
 for i in [1,550,1100]:sql('SELECT s,v FROM t WHERE id=%d'%i,rows=[[text,[i,0]]])
 sql('SELECT id FROM t ORDER BY id',rows=[[i] for i in range(1,1101)])
 sql('CREATE VECTOR INDEX vi ON t(v)')
 snapshot('cross-page update',sql("UPDATE t SET s='short',v=[0,0]",affected_rows=1100))
 session.crash();session.start();sql('SELECT id FROM t WHERE s=\'short\' ORDER BY id',rows=[[i] for i in range(1,1101)])
 sql("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 3",rows=[[1],[2],[3]])
 snapshot('cross-page delete',sql('DELETE FROM t WHERE id>=550',affected_rows=551))
 session.crash();session.start();sql('SELECT id FROM t ORDER BY id',rows=[[i] for i in range(1,550)])
 sql('SELECT s,v FROM t WHERE id=1',rows=[['short',[0,0]]]);snapshot('recovered final state')
 report['passed']=True
except Exception as error:
 report['error_type']=type(error).__name__;report['error']=str(error);raise
finally:
 try:session.close()
 finally:a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report))
