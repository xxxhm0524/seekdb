"""Additional scored-reference boundary differential checks, through real JSONL.

Deploy beside native_test_support.py in an existing native verifier workspace.
The fixture is generated independently; neither process supplies expectations.
Reference-success differences are failures. Reference rejection of an extension
is recorded separately and never used to invent a reference success.
"""
import argparse
import hashlib
import json
import math
import random
import struct
import sys
from decimal import Decimal, localcontext
from pathlib import Path

from native_test_support import NativeSession, ROOT, save


def same(a, b):
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, float) or isinstance(b, float):
        return (type(a) in (int, float) and type(b) in (int, float)
                and math.isfinite(a) and math.isfinite(b)
                and math.isclose(a, b, rel_tol=1e-12, abs_tol=1e-12))
    return type(a) is type(b) and a == b


def run(out, candidate, reference, phase, reference_commit=None):
    out.mkdir(exist_ok=False)
    ours = NativeSession(out, 'ours', candidate)
    ref = NativeSession(out, 'reference', reference)
    failures, differences, observations = [], [], []
    trace = (out / 'requests.jsonl').open('w', encoding='utf-8')

    def check(sql, group, truth=None):
        a, b = ours.execute(sql, ok=None), ref.execute(sql, ok=None)
        different = a['ok'] != b['ok']
        if a['ok'] and b['ok']:
            different |= a.get('columns') != b.get('columns') or not same(a.get('rows'), b.get('rows'))
            if sql.lstrip().split()[0].upper() in ('INSERT', 'UPDATE', 'DELETE'):
                different |= a.get('affected_rows') != b.get('affected_rows')
        item = dict(sql=sql, category=group, ours=a, reference=b,
                    qualifying_failure=bool(b['ok'] and different),
                    rejection_difference=bool(not b['ok'] and a['ok']))
        if truth is not None:
            item['independent_truth_failure'] = not (a['ok'] and b['ok'] and same(a.get('rows'), truth) and same(b.get('rows'), truth))
        observations.append(item)
        trace.write(json.dumps(item, ensure_ascii=False, allow_nan=False) + '\n'); trace.flush()
        if item['qualifying_failure'] or item.get('independent_truth_failure'):
            failures.append(item)
        if item['rejection_difference']:
            differences.append(item)
        return a, b

    def rollback_probe(sql, group):
        check('BEGIN;', 'begin')
        check(sql, group)
        check('ROLLBACK;', 'rollback')

    try:
        if phase in ('names', 'all'):
            words = 'create table insert into values select from where order by limit asc desc drop update set delete begin commit rollback bigint varchar vector null primary key not and or is index on using cosine l2 as'.split()
            for word in words:
                for location in ('table', 'column', 'alias'):
                    check('BEGIN;', 'begin')
                    table = word if location == 'table' else 'probe'
                    column = word if location == 'column' else 'n'
                    a, b = check(f'CREATE TABLE {table}(id BIGINT PRIMARY KEY,{column} BIGINT);', f'name_{location}')
                    if a['ok'] and b['ok']:
                        check(f'INSERT INTO {table}(id,{column}) VALUES(1,7),(2,3);', f'name_{location}')
                        alias = word if location == 'alias' else 'k'
                        check(f'SELECT {column} AS {alias},id FROM {table} ORDER BY {alias},id;', f'name_{location}', [[3,2],[7,1]])
                        check(f'UPDATE {table} SET {column}=9 WHERE id=1;', f'name_{location}')
                        check(f'DELETE FROM {table} WHERE {column}=3;', f'name_{location}')
                    check('ROLLBACK;', 'rollback')
            for spelling in ('`SELECT`', '`a``b`', '`汉😀`', '`space name`', '`a\x00b`'):
                check('BEGIN;', 'begin')
                a, b = check(f'CREATE TABLE {spelling}(id BIGINT PRIMARY KEY,`ORDER` BIGINT);', 'quoted_names')
                if a['ok'] and b['ok']:
                    check(f'INSERT INTO {spelling} VALUES(1,9);', 'quoted_names')
                    check(f'SELECT `ORDER` FROM {spelling};', 'quoted_names', [[9]])
                check('ROLLBACK;', 'rollback')

        if phase in ('sql', 'all'):
            check('CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT,s VARCHAR(4),v VECTOR(2));', 'setup')
            vals = [(0, 0, '', [0,0]), (-7, -7, 'a', [1,0]), (7, 7, 'a\x00b', [1,0]),
                    (-9223372036854775808, 9223372036854775807, '汉😀', [-1,0]),
                    (9223372036854775807, -9223372036854775808, 'é', [0,1]), (3, 0, '\x00', [0,-1])]
            def string(x): return "'" + x.replace("'", "''") + "'"
            literals = [f'({k},{n},{string(s)},{json.dumps(v)})' for k,n,s,v in vals]
            check('INSERT INTO t VALUES' + ','.join(literals) + ';', 'setup')
            randomizer = random.Random(120055)
            projections = ['*','id,id','n AS id,id','v,s,id','L2_DISTANCE(v,[0,0]),id',
                           'L2_DISTANCE(v,[1,0]) AS n,n,id','s AS n,id,n',
                           'id AS n,n AS id,s AS s','id AS x,n AS x',
                           'id AS d,L2_DISTANCE(v,[0,0]) AS d',
                           'L2_DISTANCE(v,[1,0]) AS d,id,L2_DISTANCE(v,[0,1]) AS e']
            orderings = ['', 'id','n DESC,s ASC','s DESC,n DESC','id DESC,n ASC',
                         'L2_DISTANCE(v,[0,0])','L2_DISTANCE(v,[0,0]) DESC,n DESC',
                         'd','d DESC,e ASC','x','s ASC,s DESC,id DESC',
                         'L2_DISTANCE(v,[0,0]),L2_DISTANCE(v,[0,0]) DESC',
                         'L2_DISTANCE(v,[0,0]),L2_DISTANCE(v,[0,1]) DESC']
            filters = ['', 'id>=-9223372036854775808 AND id<=9223372036854775807',
                       "s<'汉😀😀'", "s>='a\x00b'", 'n>=0 AND n<=7', 'id=99',
                       'n>7 AND n<0', "s=''", 'id=0 AND id=7']
            limits = ['', 'LIMIT 0','LIMIT 1','LIMIT 2','LIMIT 6','LIMIT 65537','LIMIT 9223372036854775807']
            for indexed in (False, True):
                if indexed: check('CREATE VECTOR INDEX idx ON t(v);', 'setup_index')
                for _ in range(700):
                    projection, order, where, limit = [randomizer.choice(x) for x in (projections,orderings,filters,limits)]
                    sql = f'SELECT {projection} FROM t' + (' WHERE '+where if where else '') + (' ORDER BY '+order if order else '') + (' '+limit if limit else '') + ';'
                    check(sql, 'generated_select_indexed' if indexed else 'generated_select')
                for number in ('+0000','-0000','+9223372036854775807','-9223372036854775808','01','-01','1.0','1e0','9223372036854775808','-9223372036854775809'):
                    rollback_probe(f'UPDATE t SET n={number} WHERE id=7;', 'bigint_literal')
                for assignment in ("s='12345'", "v='[0]'", "v='[1e400,0]'", "n='1'", "s=1", "n=2,n=3", "s='a',s='b'"):
                    rollback_probe(f'UPDATE t SET {assignment} WHERE id=99;', 'empty_update_validation')
                for expression in ('v', 'missing', 'L2_DISTANCE(v,[0])', 'L2_DISTANCE(s,[0,0])'):
                    check(f'SELECT id FROM t WHERE id=99 ORDER BY {expression} LIMIT 0;', 'empty_query_validation')
            for width in ('1','+1','-0','0','0002','4294967295','4294967296','9223372036854775807'):
                rollback_probe(f'CREATE TABLE extent(id BIGINT PRIMARY KEY,s VARCHAR({width}));', 'varchar_width')
            for literal in ('[3.4028235e38,-3.4028235e38]','[1.401298464324817e-45,-1e-50]',
                            '[1e-400,-1e-400]', '[1,1.490116119384765625e-8]',
                            '[1.000000059604644775390626,0]', '[3.4028235677973366e38,0]'):
                for quote in (False, True):
                    value = string(literal) if quote else literal
                    check('BEGIN;', 'begin')
                    check(f'UPDATE t SET v={value} WHERE id=7;', 'float_boundary')
                    check('SELECT v FROM t WHERE id=7;', 'float_boundary_projection')
                    check('SELECT id,L2_DISTANCE(v,[0,0]) AS d FROM t ORDER BY d,id DESC;', 'float_boundary_ranking')
                    check('ROLLBACK;', 'rollback')
            for db in (ours, ref): db.restart(crash=True)
            check('SELECT * FROM t ORDER BY id;', 'post_restart')
        if phase in ('numeric', 'all'):
            check('CREATE TABLE rounding(id BIGINT PRIMARY KEY,v VECTOR(2));', 'numeric_setup')
            check('INSERT INTO rounding VALUES(1,[0,0]),(2,[1,0]);', 'numeric_setup')
            rng = random.Random(811297)
            literals = ['1.000000059604644775390626', '-1.000000059604644775390626',
                        '3.4028235677973366e38', '-3.4028235677973366e38',
                        '3.4028235e38', '1.401298464324817e-45', '1e-400', '-1e-400']
            with localcontext() as context:
                context.prec = 180
                for _ in range(180):
                    bits = rng.randrange(1, 0x7f7fffff)
                    low = struct.unpack('<f', struct.pack('<I', bits))[0]
                    high = struct.unpack('<f', struct.pack('<I', bits+1))[0]
                    midpoint = (Decimal.from_float(low) + Decimal.from_float(high)) / 2
                    gap = Decimal.from_float(high) - Decimal.from_float(low)
                    for shift in (-1,0,1):
                        literals.append(str(midpoint + shift*gap/(Decimal(2)**80)))
            for literal in literals:
                try:
                    rounded = struct.unpack('<f', struct.pack('<f', float(literal)))[0]
                    valid = math.isfinite(rounded)
                except OverflowError:
                    valid = False
                for quoted in (False,True):
                    check('BEGIN;', 'begin')
                    value = '['+literal+',0]'; value = "'"+value+"'" if quoted else value
                    a, b = check('UPDATE rounding SET v='+value+' WHERE id=1;', 'numeric_midpoint_write')
                    if a['ok'] != valid or b['ok'] != valid:
                        item = dict(category='independent_binary64_to_binary32',literal=literal,expected_ok=valid,ours=a,reference=b,qualifying_failure=False)
                        failures.append(item);trace.write(json.dumps(item)+'\n');trace.flush()
                    if valid and a['ok'] and b['ok']:
                        a, b = check('SELECT v FROM rounding WHERE id=1;', 'numeric_midpoint_projection')
                        expected = struct.pack('<f', rounded)
                        for label, response in (('ours',a),('reference',b)):
                            value = response['rows'][0][0][0]
                            if rounded != 0 and struct.pack('<f',value) != expected:
                                item = dict(category='independent_stored_float32',literal=literal,expected_bits=expected.hex(),side=label,response=response,qualifying_failure=False)
                                failures.append(item);trace.write(json.dumps(item)+'\n');trace.flush()
                        check('SELECT id,L2_DISTANCE(v,[0,0]) AS d FROM rounding ORDER BY d,id;', 'numeric_midpoint_distance')
                    check('ROLLBACK;', 'rollback')
        if phase in ('sort', 'all'):
            check('CREATE TABLE wide(id BIGINT PRIMARY KEY,s VARCHAR(700000));', 'sort_setup')
            large = 'a'*700000
            check("INSERT INTO wide VALUES(2,'"+large+"b'),(1,'"+large[:-1]+"');", 'sort_setup_invalid_width')
            check("INSERT INTO wide VALUES(3,'"+large+"'),(1,'"+large[:-1]+"'),(2,'b');", 'sort_setup')
            for size in (1,24,25,32):
                for suffix in ('', ' LIMIT 1'):
                    ordering = ','.join(['s ASC']*size+['id DESC'])
                    check('SELECT id FROM wide ORDER BY '+ordering+suffix+';', 'repeated_wide_sort', [[1]] if suffix else [[1],[3],[2]])
            check('SELECT id,s AS a,s AS b FROM wide ORDER BY a,b DESC LIMIT 0;', 'repeated_alias_binding', [])
            check('SELECT id FROM wide ORDER BY '+','.join(['s DESC','s ASC']*16)+';', 'repeated_mixed_direction_sort', [[2],[3],[1]])
        result = dict(passed=not failures, observations=len(observations),
                      qualifying_failure_count=sum(x['qualifying_failure'] for x in failures),
                      failures=failures, reference_rejections_accepted_by_candidate=differences,
                      seed=120055, phase=phase,
                      candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
                      reference_sha256=hashlib.sha256(reference.read_bytes()).hexdigest(),
                      reference_commit=reference_commit,
                      scope='Actual hardened native processes; finite differential checks, no formal score.')
        save(out/'report.json', result)
        print(json.dumps({k:v for k,v in result.items() if k not in ('failures','reference_rejections_accepted_by_candidate')}, ensure_ascii=False), flush=True)
        if failures:
            print(json.dumps({'first_failures':failures[:6]}, ensure_ascii=False), flush=True)
        return result
    finally:
        trace.close()
        for db in (ours, ref):
            try: db.shutdown()
            finally: db.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    parser.add_argument('--candidate', default=str(ROOT/'build/minivectordb'))
    parser.add_argument('--reference', default=str(ROOT/'build/reference-a08'))
    parser.add_argument('--phase', choices=('names','sql','numeric','sort','all'), default='all')
    parser.add_argument('--reference-commit', help='Verified source identity; binary SHA-256 is always recorded')
    args = parser.parse_args()
    result = run(Path(args.output), Path(args.candidate), Path(args.reference), args.phase, args.reference_commit)
    raise SystemExit(0 if result['passed'] else 1)
