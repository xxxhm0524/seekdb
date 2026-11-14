"""Semantic oracles across page seams, including a non-leading primary key."""
import argparse,json,math,tempfile
from pathlib import Path
from protocol_client import create_fixture_client
p=argparse.ArgumentParser();p.add_argument('--image');p.add_argument('--output',type=Path);a=p.parse_args()
checks=0
with tempfile.TemporaryDirectory(prefix='mvd-wide-boundary-') as directory:
 c=create_fixture_client(directory,a.image,a.output)
 def sql(text,**expected):
  global checks
  response=c.request(text);assert response['ok'],(text[:100],response)
  for key,value in expected.items():assert response[key]==value,(text[:100],key)
  checks+=1;return response
 def literal(s):return "'"+s.replace("'","''")+"'"
 try:
  for number,width in enumerate([16360,16361,16362,16368,25000,32760,65535]):
   name='t'+str(number);text=('😀界éx'*(width//4+1))[:width]
   # PK is physically after the possibly split text; internal row IDs remain independent.
   sql('CREATE TABLE '+name+'(s VARCHAR(%d),id BIGINT PRIMARY KEY,v VECTOR(2))'%width)
   sql('BEGIN');sql('INSERT INTO '+name+' VALUES '+','.join('('+literal(text)+',%d,[%d,0])'%(i,i) for i in [-5,0,9]),affected_rows=3);sql('COMMIT')
   sql('SELECT id,s FROM '+name+' ORDER BY id',rows=[[-5,text],[0,text],[9,text]])
   sql('SELECT id FROM '+name+" ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 3",rows=[[0],[-5],[9]])
   sql('BEGIN');sql('DELETE FROM '+name+' WHERE id=0',affected_rows=1)
   sql('INSERT INTO '+name+" VALUES ('gap',77,[0,0])");sql('DELETE FROM '+name+' WHERE id=77');sql('INSERT INTO '+name+" VALUES ('new',88,[0,0])");sql('COMMIT')
   sql('UPDATE '+name+" SET s='updated',v=[1,0] WHERE id=-5",affected_rows=1)
   c.kill();c.start()
   sql('SELECT id,s FROM '+name+' ORDER BY id',rows=[[-5,'updated'],[9,text],[88,'new']])
   sql('CREATE VECTOR INDEX vi ON '+name+'(v)')
   sql('SELECT id FROM '+name+" WHERE s='new' ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 3",rows=[[88]])
   sql('BEGIN');sql('DELETE FROM '+name);sql('ROLLBACK')
   sql('SELECT id FROM '+name+' ORDER BY id',rows=[[-5],[9],[88]])
  sql('CREATE TABLE vectors(id BIGINT PRIMARY KEY,v VECTOR(16384))')
  zero='['+','.join(['0']*16384)+']';one='['+','.join(['1']*16384)+']'
  sql('INSERT INTO vectors VALUES (-1,'+zero+'),(2,'+one+')',affected_rows=2)
  sql('SELECT id,L2_DISTANCE(v,'+zero+') AS d FROM vectors ORDER BY d',rows=[[-1,0],[2,128]])
  sql('CREATE VECTOR INDEX vi ON vectors(v)');c.kill();c.start()
  sql('SELECT id FROM vectors ORDER BY L2_DISTANCE(v,'+zero+') LIMIT 2',rows=[[-1],[2]])
  oversized='CREATE TABLE too_large(id BIGINT PRIMARY KEY,'+','.join('s%d VARCHAR(65535)'%i for i in range(70))+')'
  assert not c.request(oversized)['ok'];checks+=1
 finally:c.close()
report=dict(passed=True,checks=checks,widths=[16360,16361,16362,16368,25000,32760,65535],vector_dimension=16384,pk_after_text=True,utf8_across_pages=True)
if a.output:a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report))
