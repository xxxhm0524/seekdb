"""Search for successful-reference/failed-candidate boundary mismatches.

Each result preserves both complete responses. Independent protocol and numeric
truth checks distinguish a shared bug from agreement with a passing program.
"""
import argparse
import json
import math
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
from client import Client


def typed(value):
    if isinstance(value, dict):
        return ('object', sorted((k, typed(v)) for k, v in value.items()))
    if isinstance(value, list):
        return ('array', [typed(v) for v in value])
    return (type(value).__name__, value)


def close(left, right):
    if isinstance(left, list) and isinstance(right, list):
        return len(left) == len(right) and all(close(a, b) for a, b in zip(left, right))
    if isinstance(left, float) or isinstance(right, float):
        return type(left) in (int, float) and type(right) in (int, float) and math.isclose(left, right, rel_tol=1e-12, abs_tol=1e-12)
    return type(left) is type(right) and left == right


def run(out, reference_binary, ours_binary=None):
    ours_dir, reference_dir = out / 'ours', out / 'reference'
    command = [str(ours_binary), '--data-dir', str(ours_dir)] if ours_binary else None
    ours = Client(ours_dir, command=command, timeout=90)
    reference = Client(reference_dir, command=[str(reference_binary), '--protocol', 'jsonl', '--data-dir', str(reference_dir)], timeout=90)
    observations, failures, differences = [], [], []
    trace = (out / 'requests.jsonl').open('w', encoding='utf-8')
    counter = 0
    def record(item):
        observations.append(item)
        trace.write(json.dumps(item, ensure_ascii=False, allow_nan=False) + '\n'); trace.flush()
        if item.get('qualifying_failure'):
            failures.append(item); print(json.dumps(item, ensure_ascii=False), flush=True)
        elif item.get('different'):
            differences.append(item)
    def request(db, value):
        db.process.stdin.write((json.dumps(value, ensure_ascii=False, allow_nan=False) + '\n').encode()); db.process.stdin.flush()
        return db.receive()
    def execute(sql, category='sql', expected=None, rows=None):
        nonlocal counter
        counter += 1; value = dict(id=counter, op='execute', sql=sql, search_mode='exact')
        left, right = request(ours, value), request(reference, value)
        mismatch = left['ok'] != right['ok']
        if left['ok'] and right['ok']:
            mismatch |= not close(left.get('rows'), right.get('rows')) or left.get('columns') != right.get('columns')
            # DDL/TX may omit affected_rows; the official protocol permits that.
            # Require this field only for INSERT/UPDATE/DELETE acknowledgements.
            if sql.lstrip().split(None,1)[0].upper() in ['INSERT','UPDATE','DELETE']:
                mismatch |= left.get('affected_rows') != right.get('affected_rows')
        qualifying = right['ok'] and (not left['ok'] or mismatch)
        truth_bad = expected is not None and (left['ok'] is not expected or right['ok'] is not expected)
        if rows is not None:
            truth_bad |= not close(left.get('rows'), rows) or not close(right.get('rows'), rows)
        record(dict(category=category, sql=sql, ours=left, reference=right, different=bool(mismatch),
                    qualifying_failure=bool(qualifying), independent_truth_failure=bool(truth_bad)))
        return left, right
    try:
        execute('CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT,txt VARCHAR(32),v VECTOR(3));', expected=True)
        execute("INSERT INTO t VALUES(-3,-7,'汉😀','[0,0,0]'),(0,0,'','[1,0,0]'),(1,7,'a','[1,0,0]'),(2,7,'é','[0,1,0]'),(3,9,'中','[0,0,1]');", expected=True)
        ids = [None, True, False, 0, -1, -(2**63), 2**63-1, 2**64-1, 2**64, 2**64+1, -(2**63)-1,
               10**100, -(10**100), 10**400, 10**3000, '汉😀\0id', ['id', 2**64+1], {'n':10**400,'list':[True,None,'é']}, .125, -0.0]
        for identifier in ids:
            value = dict(id=identifier, op='execute', sql='SELECT id FROM t WHERE id=0;')
            left, right = request(ours, value), request(reference, value)
            a, b = typed(left.get('id')) == typed(identifier), typed(right.get('id')) == typed(identifier)
            record(dict(category='protocol_id', input=value, ours=left, reference=right, different=left != right,
                        qualifying_failure=bool(right['ok'] and b and (not left['ok'] or not a)),
                        independent_truth_failure=not(a and b and left['ok'] and right['ok'])))
        for extra in [10**100,10**400,{'nested':[10**400]}]:
            value=dict(id='metadata',op='execute',sql='SELECT id FROM t WHERE id=0;',extra=extra)
            left,right=request(ours,value),request(reference,value)
            record(dict(category='unknown_metadata',input=value,ours=left,reference=right,different=left['ok']!=right['ok'],
                        qualifying_failure=right['ok'] and not left['ok']))
        numeric = ['0','-0','+0','1','+1','-1','.5','-.5','+.5','1.','-1.','+1.','01','001.25','1e+2','+1e-2','1.e2','1E-9999','3.4028235e38','-3.4028235e38']
        for indexed in [False, True]:
            if indexed: execute('CREATE VECTOR INDEX idx ON t(v);', expected=True)
            for number in numeric:
                for quoted in [False, True]:
                    literal = '['+number+',0,0]'; literal = "'"+literal+"'" if quoted else literal
                    execute(f'SELECT id,L2_DISTANCE(v,{literal}) AS d FROM t ORDER BY d LIMIT 3;', 'vector_numeric')
            for limit in ['0','-0','-000','1','+1','0002','16','17','18446744073709551615','-1','1.0','1e1']:
                execute(f"SELECT id,L2_DISTANCE(v,'[0,0,0]') AS d FROM t WHERE n>=0 ORDER BY d LIMIT {limit};",'limits')
            for sql in [
                'SELECT id AS k,n FROM t ORDER BY k DESC LIMIT 3;',
                'SELECT id AS a,n AS a FROM t ORDER BY a;',
                'SELECT id,n FROM t ORDER BY n DESC,id ASC;',
                "SELECT id,L2_DISTANCE(v,'[0,0,0]') AS d FROM t ORDER BY d DESC,n ASC LIMIT 4;",
                "SELECT id,L2_DISTANCE(v,'[0,0,0]') AS d,L2_DISTANCE(v,'[1,0,0]') AS e FROM t ORDER BY e LIMIT 4;",
                "SELECT id,L2_DISTANCE(v,'[0,0,0]') AS d FROM t WHERE n>=0 AND n<=7 ORDER BY d LIMIT 99;",
                "SELECT id,L2_DISTANCE(v,'[0,0,0]') AS d FROM t WHERE n>7 AND n<0 ORDER BY d LIMIT 10;",
                'SELECT id FROM t; -- trailing comment',
                'SELECT /* x */ id FROM t ORDER BY id LIMIT 2;',
                'SELECT id FROM t;;',
                'SELECT id k FROM t;',
                'SELECT t.id FROM t;',
                'SELECT id FROM t # comment',
            ]: execute(sql,'select_grammar')
            for literal in ["'[1,\f0,0]'", "'[1/*ordinary*/,0,0]'", "'[1,--line\n0,0]'", "'[1 2,0]'", "'[1/**/2,0]'", "'[1,0,]'", "'[\"1\",0,0]'", "'[NaN,0,0]'"]:
                execute(f'SELECT id,L2_DISTANCE(v,{literal}) AS d FROM t ORDER BY d LIMIT 3;','vector_lexical_boundaries')
        for i, width in enumerate(['+3','0003','3','0','-1','4294967296']):
            execute(f'CREATE TABLE ext{i}(id BIGINT PRIMARY KEY,v VECTOR({width}));','schema_extents')
        # Transactions include DDL and an error after an earlier successful write.
        execute('COMMIT;','transaction_no_active')
        execute('ROLLBACK;','transaction_no_active')
        execute('BEGIN;',expected=True)
        execute("INSERT INTO t VALUES(100,1,'kept','[0,0,0]');",expected=True)
        execute("INSERT INTO t VALUES(101,1,'partial','[0,0,0]'),(0,1,'duplicate','[0,0,0]');",expected=False)
        execute('SELECT id FROM t WHERE id>=100 ORDER BY id;',rows=[[100]])
        execute('BEGIN;','nested_transaction',expected=False)
        execute('COMMIT;',expected=True)
        execute('BEGIN;',expected=True)
        execute('CREATE TABLE temporary_table(id BIGINT PRIMARY KEY,v VECTOR(3));',expected=True)
        execute("INSERT INTO temporary_table VALUES(1,'[1,2,3]');",expected=True)
        execute('CREATE VECTOR INDEX temporary_idx ON temporary_table(v);',expected=True)
        execute('ROLLBACK;',expected=True)
        execute('SELECT id FROM temporary_table;',expected=False)
        execute('BEGIN;',expected=True)
        execute('CREATE TABLE committed_empty(id BIGINT PRIMARY KEY,v VECTOR(3));',expected=True)
        execute("INSERT INTO committed_empty VALUES(1,'[1,2]');",expected=False)
        execute('COMMIT;',expected=True)
        execute('SELECT id FROM committed_empty;',rows=[])
        for db in [ours,reference]: db.restart(crash=True)
        execute('SELECT id FROM t WHERE id>=100 ORDER BY id;',rows=[[100]])
        execute('SELECT id FROM committed_empty;',rows=[])
        # Cache/index thresholds and tails, with an independent ordered-f64 oracle.
        rng=random.Random(93011)
        for dimensions in [1,3,31,32,33,128,129]:
            name='v'+str(dimensions); vectors={}
            execute(f'CREATE TABLE {name}(v VECTOR({dimensions}),bucket BIGINT,id BIGINT PRIMARY KEY);',expected=True)
            values=[]
            for k in range(257):
                vector=[rng.randrange(-16,17)/8 for _ in range(dimensions)]
                if k<3: vector=[0.0]*dimensions
                vectors[k]=vector;values.append(f"('{json.dumps(vector)}',{k%5},{k-128})")
            execute(f'INSERT INTO {name} VALUES'+','.join(values)+';',expected=True)
            execute(f'CREATE VECTOR INDEX idx_{name} ON {name}(v);',expected=True)
            query=[.125]*dimensions
            for group in [None,1,8]:
                selected=[k for k in vectors if group is None or k%5==group]
                distances={k:math.sqrt(sum((float(x)-.125)**2 for x in vectors[k])) for k in selected}
                selected.sort(key=lambda k:(distances[k],k-128))
                where='' if group is None else f'WHERE bucket={group}'
                for limit in [0,1,10,16,17,31,32,33,257,300]:
                    expected=[[k-128,distances[k]] for k in selected[:limit]]
                    execute(f"SELECT id,L2_DISTANCE(v,'{json.dumps(query)}') AS d FROM {name} {where} ORDER BY d LIMIT {limit};",'vector_thresholds',rows=expected)
        # A failing SQL error also has to echo a very large identifier exactly.
        value=dict(id={'large':10**400},op='execute',sql='SELECT missing FROM t;')
        left,right=request(ours,value),request(reference,value)
        record(dict(category='error_id',input=value,ours=left,reference=right,different=left!=right,
                    qualifying_failure=typed(right.get('id'))==typed(value['id']) and typed(left.get('id'))!=typed(value['id'])))
        return dict(passed=not failures and not any(x.get('independent_truth_failure') for x in observations),
                    observations=len(observations),qualifying_failures=failures,differences=differences,
                    independent_truth_failures=[x for x in observations if x.get('independent_truth_failure')],
                    source_reference_commit='dfee55b84284f4c39d2df45951cdbcb1d1baf1a2',
                    scope='Actual frozen reference and candidate processes; not an official platform admission or score.')
    finally:
        trace.close();ours.kill();reference.kill()


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    p=argparse.ArgumentParser();p.add_argument('--output',required=True);p.add_argument('--reference-bin',required=True);p.add_argument('--ours-bin');args=p.parse_args()
    out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False)
    try:result=run(out,Path(args.reference_bin).resolve(),Path(args.ours_bin).resolve() if args.ours_bin else None)
    except Exception as error:
        import traceback
        result=dict(passed=False,error=str(error),traceback=traceback.format_exc())
    (out/'report.json').write_text(json.dumps(result,ensure_ascii=False,indent=2,allow_nan=False),encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k not in ['qualifying_failures','differences','independent_truth_failures']},ensure_ascii=False),flush=True)
    raise SystemExit(0 if result['passed'] else 1)
