"""Reject incomplete block comments before any SQL or transaction takes effect."""
import argparse
import json
from pathlib import Path
import sys
import tempfile

sys.path.insert(0,str(Path(__file__).resolve().parent))
from protocol_client import Client
parser=argparse.ArgumentParser()
parser.add_argument('--output',type=Path)
parser.add_argument('--image')
args=parser.parse_args()
report=dict(passed=False,requests=0,rejected_comments=0)
last={}
try:
    with tempfile.TemporaryDirectory(prefix='mvd-comments-') as directory:
        if args.image:
            sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'protocol_check'))
            from _transport import Session
            config=json.loads((Path(__file__).resolve().parent.parent/'protocol_check/config.json').read_text())
            class ImageClient:
                def __init__(self):
                    log=args.output.with_suffix('.stderr.log') if args.output else Path(directory)/'stderr.log'
                    log.parent.mkdir(parents=True,exist_ok=True)
                    self.session=Session(config,log,image=args.image)
                    self.active=False
                    try:self.start()
                    except Exception:self.session.close();raise
                def start(self):self.session.start();self.active=True
                def request(self,text):return self.session.request(text,'exact')
                def kill(self):self.session.crash();self.active=False
                def close(self):
                    try:
                        if self.active:self.session.shutdown();self.active=False
                    finally:self.session.close()
            c=ImageClient()
        else:c=Client(datadir=directory)
        try:
            def sql(text,error=False,**fields):
                global last
                value=c.request(text);report['requests']+=1
                last=dict(sql=text,response=value,expected_error=error)
                assert value['ok'] is (not error),last
                if error:
                    assert value.get('error',{}).get('code')=='syntax_error',last
                    report['rejected_comments']+=1
                for key,wanted in fields.items():assert value.get(key)==wanted,(last,key,wanted)
                return value
            sql('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(40),v VECTOR(2))')
            # Minimal RED on 1696c7f7: it returns success and leaves id=99.
            sql("INSERT INTO t VALUES(99,'bad','[1,2]') /*",error=True)
            sql('SELECT id FROM t ORDER BY id',rows=[])
            sql("INSERT INTO t VALUES(1,'base','[3,4]')",affected_rows=1)
            for transaction in [False,True]:
                expected=[[1,'base',[3,4]]]
                if transaction:
                    sql('BEGIN')
                    sql("INSERT INTO t VALUES(2,'prior','[0,0]')",affected_rows=1)
                    expected.append([2,'prior',[0,0]])
                statements=["INSERT INTO t VALUES(99,'bad','[1,2]')", "UPDATE t SET s='bad'",
                            'DELETE FROM t', 'SELECT id FROM t', 'COMMIT','ROLLBACK',
                            'DROP TABLE t','CREATE TABLE forbidden(id BIGINT PRIMARY KEY)']
                for base in statements:
                    for tail in ['/*',' /* unfinished',' /*/',' /**',' /* Unicode 中文',
                                 ' /**/ /* next',' /*\n * missing terminator']:
                        sql(base+tail,error=True)
                        sql('SELECT id,s,v FROM t ORDER BY id',rows=expected)
                if transaction:sql('COMMIT')
            # Comments inside a string remain data. Closed comments and line comments work.
            sql("INSERT /*closed*/ INTO t VALUES(3,'/*','[1,0]') /**/",affected_rows=1)
            sql("UPDATE t SET s='*/' WHERE id=3 /*closed*/",affected_rows=1)
            sql('SELECT id FROM t WHERE id=3 -- no closing delimiter needed',rows=[[3]])
            c.kill();c.start()
            sql('SELECT id,s FROM t ORDER BY id',rows=[[1,'base'],[2,'prior'],[3,'*/']])
            sql('BEGIN')
            sql("UPDATE t SET s='temporary' WHERE id=1",affected_rows=1)
            sql('ROLLBACK /* not closed',error=True)
            sql("SELECT s FROM t WHERE id=1",rows=[['temporary']])
            sql('ROLLBACK')
            sql("SELECT s FROM t WHERE id=1",rows=[['base']])
            report.update(passed=True,prior_transaction_work_preserved=True,
                          closed_and_quoted_comments_preserved=True,restart_verified=True)
        finally:c.close()
except Exception:
    report['failure']=last
    raise
finally:
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False),flush=True)
