"""Identifier quoting must survive keyword and vector-function recognition."""
import argparse,json,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from protocol_client import create_fixture_client
p=argparse.ArgumentParser();p.add_argument('--output',type=Path);p.add_argument('--image');a=p.parse_args()
report=dict(passed=False,requests=0,names_checked=0);last={}
def quote(s):return '`'+s.replace('`','``')+'`'
try:
    with tempfile.TemporaryDirectory(prefix='mvd-identifiers-') as d:
        c=create_fixture_client(d,a.image,a.output)
        try:
            def sql(text,error=False,**fields):
                global last
                r=c.request(text);report['requests']+=1;last=dict(sql=text,response=r)
                assert r['ok'] is (not error),last
                for k,v in fields.items():assert r.get(k)==v,(last,k,v)
                return r
            # Minimal RED: declaration succeeds but reading its ordinary column fails.
            sql('CREATE TABLE f(id BIGINT PRIMARY KEY,l2_distance BIGINT,vector_distance BIGINT)')
            sql('INSERT INTO f VALUES(1,10,20)',affected_rows=1)
            sql('SELECT l2_distance,vector_distance FROM f',columns=['l2_distance','vector_distance'],rows=[[10,20]])
            for alias in ['l2_distance','vector_distance']:
                sql('SELECT id AS '+alias+' FROM f ORDER BY '+alias,columns=[alias],rows=[[1]])
            for name in ['primary','from','where','order','limit','table','create','select','begin','commit','rollback',
                         'l2_distance','vector_distance','a`b','中文`列','two words']:
                table=quote('table`'+name);pk=quote(name)
                sql('CREATE TABLE '+table+'('+pk+' BIGINT PRIMARY KEY,s VARCHAR(8),v VECTOR(2))')
                sql('INSERT INTO '+table+" VALUES(2,'b','[3,4]'),(1,'a','[0,0]')",affected_rows=2)
                sql('SELECT '+pk+' FROM '+table+' ORDER BY '+pk,columns=[name],rows=[[1],[2]])
                sql('SELECT '+pk+' `from` FROM '+table+' ORDER BY `from`',columns=['from'],rows=[[1],[2]])
                sql('SELECT '+pk+",L2_DISTANCE(v,'[0,0]') AS `l2_distance` FROM "+table+
                    ' ORDER BY `l2_distance`',columns=[name,'l2_distance'],rows=[[1,0],[2,5]])
                sql('UPDATE '+table+" SET s='ok' WHERE "+pk+'=1',affected_rows=1)
                sql('BEGIN');sql('DELETE FROM '+table+' WHERE '+pk+'=2',affected_rows=1);sql('ROLLBACK')
                sql('SELECT '+pk+',s FROM '+table+' ORDER BY '+pk,rows=[[1,'ok'],[2,'b']])
                c.kill();c.start()
                sql('SELECT '+pk+' FROM '+table+' ORDER BY '+pk,rows=[[1],[2]])
                sql('DROP TABLE '+table)
                report['names_checked']+=1
            for text in ['`BEGIN`','CREATE `table`(id BIGINT PRIMARY KEY)','SELECT `missing FROM f']:
                sql(text,error=True)
            sql('SELECT id FROM f',rows=[[1]])
            report.update(passed=True,quoted_keywords_not_commands=True,escaped_backticks=True,
                          function_and_column_names_separated=True,restart_verified=True)
        finally:c.close()
except Exception:
    report['failure']=last;raise
finally:
    if a.output:
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False),flush=True)
