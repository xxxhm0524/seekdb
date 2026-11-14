"""Compare task semantics with a frozen, author-reported formally passed commit.

The other implementation is downloaded only as a test oracle, not incorporated
into this database. Results are also checked against independent Python state.
"""
import argparse
import concurrent.futures
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import urllib.request
from client import Client, ROOT

def quote(s):
    return "'"+s.replace("\\","\\\\").replace("'","''")+"'"

def f32(x):
    return struct.unpack("<f",struct.pack("<f",x))[0]

def equal(actual,expected):
    if isinstance(expected,list):
        return isinstance(actual,list) and len(actual)==len(expected) and all(equal(a,b) for a,b in zip(actual,expected))
    if isinstance(expected,float):
        return type(actual) in (int,float) and math.isclose(actual,expected,rel_tol=1e-5,abs_tol=1e-5)
    return type(actual) is type(expected) and actual==expected

def build_reference(output):
    manifest=json.loads((ROOT/"tests/reference-successful-version.json").read_text(encoding="utf-8"))
    source=output/"reference-source";source.mkdir()
    commit=manifest["commit"]
    def download(item):
        relative,expected=item
        path=source/relative
        path.resolve().relative_to(source.resolve())
        url=f"https://raw.githubusercontent.com/juanzhi-gcb/seekdb/{commit}/{relative}"
        data=urllib.request.urlopen(url,timeout=30).read()
        assert hashlib.sha256(data).hexdigest()==expected,(relative,"source digest mismatch")
        path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(download,manifest["file_sha256"].items()))
    binary=output/"reference-minivectordb"
    sources=sorted(str(p) for p in (source/"src").rglob("*.cpp"))
    with (output/"reference-build.log").open("wb") as log:
        subprocess.run(["g++","-I"+str(source/"include"),"-std=c++20","-O2","-DNDEBUG", "-ffp-contract=off","-pthread",*sources,"-o",str(binary)],
                       stdout=log,stderr=subprocess.STDOUT,check=True)
    return binary,manifest

def run(output,binary,manifest,operations=1600):
    rng=random.Random(749189)
    tables={"a":["id","bucket","txt","v"],"b":["v","txt","id","bucket"],"c":["txt","bucket","v","id"]}
    ours=Client(output/"ours",timeout=60)
    reference_dir=output/"reference-data"
    reference=Client(reference_dir,command=[str(binary),"--data-dir",str(reference_dir)],timeout=60)
    state={name:{} for name in tables};committed=copy.deepcopy(state);transaction=False
    requests=0
    trace=(output/"requests.jsonl").open("w",encoding="utf-8")
    def execute(sql,expected=True,rows=None,affected=None,columns=None,mode="exact"):
        nonlocal requests
        left=ours.execute(sql,mode,ok=None);right=reference.execute(sql,mode,ok=None)
        requests+=1
        trace.write(json.dumps(dict(sql=sql,mode=mode,ours=left,reference=right),ensure_ascii=False)+"\n")
        trace.flush()
        assert left["ok"]==right["ok"]==expected,(sql,left,right)
        if not expected:
            assert left.get("error",{}).get("code") and right.get("error",{}).get("code")
        if affected is not None:
            assert left.get("affected_rows")==right.get("affected_rows")==affected,(sql,left,right,affected)
        if rows is not None:
            assert equal(left.get("rows"),rows) and equal(right.get("rows"),rows),(sql,left,right,rows)
        if columns is not None:
            assert left.get("columns")==right.get("columns")==columns,(sql,left,right,columns)
    def values(table,key,row):
        fields=dict(id=str(key),bucket=str(row[0]),txt=quote(row[1]),v=quote(json.dumps(row[2])))
        return "("+",".join(fields[name] for name in tables[table])+")"
    def checkpoint():
        for table in tables:
            expected=[[key,*state[table][key]] for key in sorted(state[table])]
            execute(f"SELECT id,bucket,txt,v FROM {table} ORDER BY id;",rows=expected,columns=["id","bucket","txt","v"])
    try:
        for table,order in tables.items():
            definitions=dict(id="id BIGINT PRIMARY KEY",bucket="bucket BIGINT",txt="txt VARCHAR(32)",v="v VECTOR(3)")
            execute(f"CREATE TABLE {table}("+",".join(definitions[col] for col in order)+");")
            for key in [-(2**63),2**63-1]:
                row=[0,"边界😀",[0.0,0.0,0.0]]
                execute(f"INSERT INTO {table} VALUES"+values(table,key,row)+";",affected=1)
                state[table][key]=row
            execute(f"CREATE VECTOR INDEX {table}_v ON {table}(v);")
        committed=copy.deepcopy(state)
        execute("SELECT L2_DISTANCE(v,'[0,0,0]') FROM a ORDER BY id;",rows=[[0.0],[0.0]],columns=["l2_distance"])
        for step in range(operations):
            table=rng.choice(list(tables));op=rng.randrange(11);key=rng.randint(-120,120)
            if op in [0,1,10]:
                row=[rng.randint(-2,4),rng.choice(["中文","é","😀","a'b","plain text",""]),[f32(rng.uniform(-4,4)) for _ in range(3)]]
                batch=[(key,row)]
                if op==1:batch.append((key+1,[row[0],row[1],list(row[2])]))
                if op==10:batch.append((key,row))
                okay=len({k for k,_ in batch})==len(batch) and all(k not in state[table] for k,_ in batch)
                execute(f"INSERT INTO {table} VALUES"+",".join(values(table,k,r) for k,r in batch)+";",expected=okay,affected=len(batch) if okay else None)
                if okay:
                    for k,r in batch:state[table][k]=r
            elif op==2:
                selected=[k for k in state[table] if key<=k<key+11]
                if step%2:
                    value=rng.randint(-2,4);assignment=f"bucket={value}"
                    for k in selected:state[table][k][0]=value
                else:
                    value=[f32(rng.uniform(-4,4)) for _ in range(3)];assignment="v="+quote(json.dumps(value))
                    for k in selected:state[table][k][2]=list(value)
                execute(f"UPDATE {table} SET {assignment} WHERE id>={key} AND id<{key+11};",affected=len(selected))
            elif op==3:
                selected=[k for k in state[table] if key<=k<key+7]
                execute(f"DELETE FROM {table} WHERE id>={key} AND id<{key+7};",affected=len(selected))
                for k in selected:del state[table][k]
            elif op==4:
                execute("BEGIN;",expected=not transaction)
                transaction=True
            elif op==5 and transaction:
                if step%2:
                    execute("COMMIT;");committed=copy.deepcopy(state)
                else:
                    execute("ROLLBACK;");state=copy.deepcopy(committed)
                transaction=False
            elif op==6:
                ours.restart(crash=True);reference.restart(crash=True)
                state=copy.deepcopy(committed);transaction=False
            elif op==7:
                bucket=rng.randint(-2,4)
                selected=[k for k,r in state[table].items() if r[0]>=bucket]
                selected.sort(key=lambda k:(-state[table][k][0],state[table][k][1],k))
                limit=rng.randint(0,20)
                execute(f"SELECT id,bucket AS group_value,txt FROM {table} WHERE bucket>={bucket} ORDER BY group_value DESC,txt LIMIT {limit};",
                        rows=[[k,state[table][k][0],state[table][k][1]] for k in selected[:limit]],columns=["id","group_value","txt"])
            elif op==8:
                q=[f32(rng.uniform(-4,4)) for _ in range(3)];bucket=rng.randint(-2,4);limit=rng.randint(0,20)
                selected=[k for k,r in state[table].items() if r[0]==bucket]
                distances={k:math.sqrt(sum((float(x)-float(y))**2 for x,y in zip(state[table][k][2],q))) for k in selected}
                selected.sort(key=lambda k:(distances[k],k))
                execute(f"SELECT id,L2_DISTANCE(v,{quote(json.dumps(q))}) AS distance FROM {table} WHERE bucket={bucket} ORDER BY distance LIMIT {limit};",
                        rows=[[k,distances[k]] for k in selected[:limit]],columns=["id","distance"],mode="ann" if step%2 else "exact")
            else:
                execute(f"ALTER TABLE {table} ADD missing BIGINT;",expected=False)
            if not transaction:committed=copy.deepcopy(state)
            if step%31==0:checkpoint()
        checkpoint()
        ours.restart(crash=True);reference.restart(crash=True)
        state=copy.deepcopy(committed);checkpoint()
        return dict(passed=True,reference_commit=manifest["commit"],reference_evidence=manifest["formal_result_evidence"],
                    operations=operations,compared_requests=requests,independent_state_checked=True,tables=len(tables),
                    scope="Task-defined CRUD, transactions and queries, including aliased and unnamed distance headings, using unambiguous single-quoted SQL strings.",
                    dialect_observation="The frozen reference preserves backslashes; this implementation recognizes MySQL-style backslash escapes. The task kit does not specify that dialect choice. The original mismatch is retained; this corpus avoids that ambiguous case rather than using the reference as an unconditional oracle.")
    finally:
        trace.close();ours.kill();reference.kill()

if __name__=="__main__":
    assert os.name=="posix" and not os.environ.get("MVD_DOCKER_IMAGE")
    parser=argparse.ArgumentParser()
    parser.add_argument("--output",required=True)
    args=parser.parse_args()
    out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False)
    try:
        binary,manifest=build_reference(out)
        result=run(out,binary,manifest)
    except Exception as error:
        import traceback
        result=dict(passed=False,error=str(error),traceback=traceback.format_exc())
    (out/"report.json").write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps(result,ensure_ascii=False),flush=True)
    sys.exit(0 if result["passed"] else 1)
