"""Read genuine cross-page WAL and table files in both directions with c7e340bc."""
import argparse,json,sys
from pathlib import Path
from wal_compatibility import execute,read_wal,describe_wal
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'protocol_check'))
from _transport import Session
p=argparse.ArgumentParser()
for version in ['reference','candidate']:
 group=p.add_mutually_exclusive_group(required=True);group.add_argument('--'+version+'-image');group.add_argument('--'+version+'-binary',type=Path)
p.add_argument('--output',type=Path,required=True);a=p.parse_args()
assert bool(a.reference_image)==bool(a.candidate_image)
targets={v:dict(image=getattr(a,v+'_image')) if getattr(a,v+'_image') else dict(command=[str(getattr(a,v+'_binary').resolve()),'--data-dir','{data_dir}']) for v in ['reference','candidate']}
a.output.parent.mkdir(parents=True,exist_ok=True);config=json.loads((ROOT/'protocol_check/config.json').read_text(encoding='utf-8'));trials=[]
for writer,reader in [('reference','candidate'),('candidate','reference')]:
 session=Session(config,a.output.with_suffix('.'+writer+'.stderr.log'),**targets[writer])
 try:
  session.start();execute(session,'CREATE TABLE t(s VARCHAR(65535),id BIGINT PRIMARY KEY,v VECTOR(2))');execute(session,'CREATE TABLE v(id BIGINT PRIMARY KEY,v VECTOR(16384))')
  text='😀'*65535;zero='['+','.join(['0']*16384)+']'
  execute(session,'BEGIN')
  for i in range(1,13):execute(session,"INSERT INTO t VALUES ('%s',%d,[%d,0])"%(text,i,i),affected_rows=1)
  execute(session,'INSERT INTO v VALUES (1,'+zero+')');execute(session,'COMMIT')
  raw,_=read_wal(session);records=describe_wal(raw);assert records['commits']==1 and records['page_records']>12
  session.crash();session.image=targets[reader].get('image');session.command=targets[reader].get('command');session.start()
  execute(session,'SELECT id FROM t ORDER BY id',rows=[[i] for i in range(1,13)])
  execute(session,'SELECT s FROM t WHERE id=7',rows=[[text]])
  execute(session,'SELECT id,L2_DISTANCE(v,'+zero+') AS d FROM v',rows=[[1,0]])
  execute(session,'CREATE VECTOR INDEX vi ON t(v)');execute(session,"SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 3",rows=[[1],[2],[3]])
  execute(session,'BEGIN');execute(session,"UPDATE t SET s='short',v=[0,0] WHERE id=7");execute(session,'DELETE FROM t WHERE id=4');execute(session,'COMMIT')
  execute(session,'BEGIN');execute(session,'DELETE FROM t');session.crash()
  session.image=targets[writer].get('image');session.command=targets[writer].get('command');session.start()
  execute(session,'SELECT id FROM t ORDER BY id',rows=[[i] for i in range(1,13) if i!=4]);execute(session,'SELECT s FROM t WHERE id=7',rows=[['short']])
  execute(session,"SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 1",rows=[[7]])
  trials.append(dict(writer=writer,reader=reader,wal=records,rows=12,shortened_text_and_delete_survive=True))
 finally:session.close()
report=dict(passed=True,reference_sha='c7e340bce93b5f4b29864a2eca6da3dbc37da877',trials=trials,scope='Cross-page rows, committed WAL replay, index rebuild and a return trip after updates')
a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(report))
