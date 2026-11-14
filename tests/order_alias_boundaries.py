"""Repeated projection names must not manufacture explicit ORDER BY aliases."""
import argparse
import json
import math
from pathlib import Path
import sys
from client import Client


def run(directory):
    db=Client(directory);checked=0
    try:
        db.execute('CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT,v VECTOR(3));')
        db.execute("INSERT INTO t VALUES(2,0,'[0,0,0]'),(1,9,'[1,0,0]'),(-1,7,'[0,0,0]');")
        def check(sql,expected,columns):
            nonlocal checked
            result=db.execute(sql)
            assert result['columns']==columns and result['rows']==expected,(sql,result,expected)
            checked+=1
        def phase():
            check('SELECT id,id FROM t ORDER BY id;',[[-1,-1],[1,1],[2,2]],['id','id'])
            check('SELECT id,ID FROM t ORDER BY ID DESC;',[[2,2],[1,1],[-1,-1]],['id','id'])
            check('SELECT n AS id,id FROM t ORDER BY id;',[[0,2],[7,-1],[9,1]],['id','id'])
            check('SELECT id AS n,n FROM t ORDER BY n;', [[-1,7],[1,9],[2,0]],['n','n'])
            check("SELECT L2_DISTANCE(v,'[0,0,0]') AS id,id FROM t ORDER BY id LIMIT 3;",[[0.0,-1],[0.0,2],[1.0,1]],['id','id'])
            check('SELECT id,id FROM t ORDER BY n,id;',[[2,2],[-1,-1],[1,1]],['id','id'])
            check('SELECT id,id FROM t ORDER BY id LIMIT 0;',[],['id','id'])
            db.execute('SELECT id AS x,n AS x FROM t ORDER BY x;',ok=False)
        phase();db.execute('CREATE VECTOR INDEX idx ON t(v);');phase()
        db.execute('BEGIN;');db.execute("UPDATE t SET v='[2,0,0]' WHERE id=2;")
        check("SELECT L2_DISTANCE(v,'[0,0,0]') AS id,id FROM t ORDER BY id;",[[0.0,-1],[1.0,1],[2.0,2]],['id','id'])
        db.execute('ROLLBACK;');db.restart(crash=True);phase()
        return dict(passed=True,queries_checked=checked,duplicate_ordinary_projection=True,
                    explicit_alias_precedence=True,explicit_duplicate_alias_rejected=True,index_rollback_and_restart=True)
    finally:db.kill()


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    p=argparse.ArgumentParser();p.add_argument('--output',required=True);args=p.parse_args()
    out=Path(args.output);out.mkdir(parents=True,exist_ok=False)
    try:result=run(out/'data')
    except Exception as error:
        import traceback
        result=dict(passed=False,error=str(error),traceback=traceback.format_exc())
    (out/'report.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result),flush=True)
    raise SystemExit(0 if result['passed'] else 1)
