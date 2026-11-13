"""Bare numeric vectors cannot silently reinterpret SQL strings or identifiers."""
import argparse,json,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from protocol_client import create_fixture_client
parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path);parser.add_argument('--image');args=parser.parse_args()
report=dict(passed=False,requests=0,invalid_component_checks=0);last={}
try:
    with tempfile.TemporaryDirectory(prefix='mvd-vector-types-') as directory:
        c=create_fixture_client(directory,args.image,args.output)
        try:
            def sql(text,error=False,**fields):
                global last
                value=c.request(text);report['requests']+=1
                last=dict(sql=text,response=value,expected_error=error)
                assert value['ok'] is (not error),last
                if error:
                    assert value.get('error',{}).get('code'),last
                    report['invalid_component_checks']+=1
                for k,v in fields.items():assert value.get(k)==v,(last,k,v)
                return value
            sql('CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2))')
            sql("INSERT INTO t VALUES(1,['1,2'])",error=True)
            sql("INSERT INTO t VALUES(1,'[3,4]')",affected_rows=1)
            sql('CREATE VECTOR INDEX vi ON t(v)')
            invalid=["['1',2]","[1,'2']","['1,2']",'["1",2]',
                     '[`1`,2]','[1,`2`]','[`1,2`]','[foo,2]',
                     "['-1',+2]","['1e2',2]","['1','2']"]
            for transaction in [False,True]:
                expected=[[1,[3,4]]]
                if transaction:
                    sql('BEGIN');sql("INSERT INTO t VALUES(2,'[0,0]')",affected_rows=1)
                    expected.append([2,[0,0]])
                for literal in invalid:
                    for text in ['INSERT INTO t VALUES(99,'+literal+')',
                                 'UPDATE t SET v='+literal+' WHERE id=1',
                                 "INSERT INTO t VALUES(98,'[1,2]'),(99,"+literal+')',
                                 'SELECT id FROM t ORDER BY L2_DISTANCE(v,'+literal+') LIMIT 1',
                                 'SELECT L2_DISTANCE(v,'+literal+') FROM t WHERE id=999',
                                 'UPDATE t SET v='+literal+' WHERE id=999']:
                        sql(text,error=True)
                        sql('SELECT id,v FROM t ORDER BY id',rows=expected)
                if transaction:sql('ROLLBACK')
            for n,literal in enumerate(['[-1,+2]','[1e-2,-3E+1]','[ /* closed */ 3,4]'],10):
                sql('INSERT INTO t VALUES(%d,%s)'%(n,literal),affected_rows=1)
            sql('DELETE FROM t WHERE id>=10',affected_rows=3)
            c.kill();c.start();sql('SELECT id,v FROM t ORDER BY id',rows=[[1,[3,4]]])
            report.update(passed=True,statement_atomicity=True,prior_transaction_work_preserved=True,restart_verified=True)
        finally:c.close()
except Exception:
    report['failure']=last;raise
finally:
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False),flush=True)
