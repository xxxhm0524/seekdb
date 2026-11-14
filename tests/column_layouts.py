"""Projection/predicate reads across every order of variable and fixed fields."""
import argparse
import itertools
import json
from pathlib import Path
import sys
from client import Client
from invariants import quote

def run(directory):
    db=Client(directory,env={"MVD_VECTOR_CACHE_MIB":"0"})
    layouts=0
    try:
        for layouts,order in enumerate(itertools.permutations(["id","a","n","v","b"]),1):
            table=f"t{layouts}"
            definitions=dict(id="id BIGINT PRIMARY KEY",a="a VARCHAR(8)",n="n BIGINT",v="v VECTOR(2)",b="b VARCHAR(32)")
            db.execute(f"CREATE TABLE {table}("+",".join(definitions[x] for x in order)+");")
            expected={-2:["中文",2,[3.0,4.0],"a\x00b"],0:["",-1,[0.0,0.0],"😀"],9:["é",2,[0.0,1.0],"quoted'\\text"]}
            values=[]
            for key,(a,n,v,b) in expected.items():
                fields=dict(id=str(key),a=quote(a),n=str(n),v=quote(json.dumps(v)),b=quote(b))
                values.append("("+",".join(fields[x] for x in order)+")")
            db.execute(f"INSERT INTO {table} VALUES"+",".join(values)+";")
            assert db.execute(f"SELECT b,n,a FROM {table} WHERE id=-2;")["rows"]==[["a\x00b",2,"中文"]]
            assert db.execute(f"SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM {table} WHERE n=2 ORDER BY d LIMIT 2;")["rows"]==[[9,1.0],[-2,5.0]]
            assert db.execute(f"SELECT a FROM {table} WHERE b={quote('😀')} AND n<0;")["rows"]==[[""]]
            db.execute(f"BEGIN;")
            db.execute(f"UPDATE {table} SET b='pending',v='[1,0]' WHERE id=-2;")
            assert db.execute(f"SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM {table} WHERE a='中文' ORDER BY d LIMIT 1;")["rows"]==[[-2,1.0]]
            db.execute("ROLLBACK;")
            assert db.execute(f"SELECT b FROM {table} WHERE id=-2;")["rows"]==[["a\x00b"]]
            assert db.execute(f"DELETE FROM {table} WHERE b={quote('😀')};")["affected_rows"]==1
            assert db.execute(f"SELECT id FROM {table} ORDER BY n DESC,id;")["rows"]==[[-2],[9]]
        db.restart(crash=True)
        assert db.execute("SELECT b FROM t1 WHERE id=-2;")["rows"]==[["a\x00b"]]
        assert db.execute(f"SELECT b FROM t{layouts} WHERE id=-2;")["rows"]==[["a\x00b"]]
        return dict(passed=True,column_orders=layouts,transaction_read_and_rollback=True,embedded_nul_and_unicode=True)
    finally:
        db.kill()

if __name__=="__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    p=argparse.ArgumentParser();p.add_argument("--output",required=True);args=p.parse_args()
    out=Path(args.output);out.mkdir(parents=True,exist_ok=False)
    result=run(out/"data")
    (out/"report.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
    print(json.dumps(result),flush=True)
