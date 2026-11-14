"""Physical working-column clustering must preserve all public query semantics."""
import argparse
import json
import math
from pathlib import Path
import sys
from client import Client


def quote(value):
    return "'" + value.replace('\\', '\\\\').replace("'", "''") + "'"


def run(directory):
    count, checked = 321, 0
    categories = ['', 'aa', 'z', 'é', '中文', '🙂', 'a\x00b']
    db = Client(directory, timeout=60)
    try:
        for dimensions in [3, 33, 128]:
            table = 't' + str(dimensions)
            rows = {}
            db.execute(f'CREATE TABLE {table}(v VECTOR({dimensions}),category VARCHAR(20),id BIGINT PRIMARY KEY,bucket BIGINT,content VARCHAR(64));')
            keys = [-(2**63), 2**63 - 1] + list(range(-160, 159))
            for base in range(0, count, 40):
                values = []
                for i in range(base, min(base + 40, count)):
                    k = keys[i]
                    row = dict(category=categories[(i * 13) % len(categories)], bucket=(i * 7) % 11 - 5,
                               v=[((i % 23) - 11) / 8] * dimensions, content='body:' + str(k))
                    rows[k] = row
                    values.append('(' + ','.join([quote(json.dumps(row['v'])), quote(row['category']), str(k), str(row['bucket']), quote(row['content'])]) + ')')
                db.execute(f'INSERT INTO {table} VALUES' + ','.join(values) + ';')
            db.execute(f'CREATE VECTOR INDEX idx ON {table}(v);')

            def check(conditions, limit=17, mode='exact'):
                nonlocal checked
                def compare(a, op, b):
                    return {'=': a == b, '<': a < b, '<=': a <= b, '>': a > b, '>=': a >= b, '!=': a != b, '<>': a != b}[op]
                selected = []
                for key, row in rows.items():
                    if all(compare(key if column == 'id' else row[column], op, value) for column, op, value in conditions):
                        squared = 0.0
                        for x in row['v']:
                            delta = float(x) - 0.125
                            squared += delta * delta
                        selected.append((math.sqrt(squared), key))
                selected.sort()
                where = ' AND '.join(f'{column}{op}{quote(value) if isinstance(value, str) else value}' for column, op, value in conditions)
                sql = f"SELECT id,content,category,bucket,v,L2_DISTANCE(v,'{json.dumps([0.125] * dimensions)}') AS distance FROM {table} "
                if where:
                    sql += 'WHERE ' + where + ' '
                actual = db.execute(sql + f'ORDER BY distance LIMIT {limit};', mode)['rows']
                expected = selected[:limit]
                assert [r[0] for r in actual] == [key for _, key in expected], (dimensions, conditions, actual, expected)
                for record, (distance, key) in zip(actual, expected):
                    row = rows[key]
                    assert record[1:5] == [row['content'], row['category'], row['bucket'], row['v']]
                    assert math.isclose(record[5], distance, rel_tol=1e-14, abs_tol=0)
                checked += 1

            # The first filter introduces category, then bucket. Repeated mixed
            # predicates must work after either physical ordering has been chosen.
            for column, value in [('category', 'é'), ('bucket', 1), ('category', '中文'), ('id', -3)]:
                check([(column, '=', value)])
                for op in ['=', '<', '<=', '>', '>=', '!=', '<>']:
                    check([(column, op, value)], 10, 'ann')
                    check([(column, op, value), ('bucket', '>=', 0)], 300)
                check([])
            check([('category', '=', 'not-present')])
            check([('bucket', '>', 4), ('bucket', '<', -4)])
            check([('category', '=', 'a\x00b')], 1)
            check([('id', '>=', -(2**63)), ('id', '<=', 2**63 - 1)])
            db.execute('BEGIN;')
            db.execute(f'UPDATE {table} SET category=\'é\',bucket=0 WHERE id=-3;')
            old = rows[-3].copy(); rows[-3].update(category='é', bucket=0)
            check([('category', '=', 'é'), ('bucket', '=', 0)], 10)
            db.execute('ROLLBACK;'); rows[-3] = old
            check([('category', '=', 'é'), ('bucket', '=', 0)], 10)
            db.restart(crash=True)
            check([('category', '>=', 'é')], 10)
        return dict(passed=True, queries_checked=checked, dimensions=[3, 33, 128], rows_per_table=count,
                    unicode_and_nul=True, all_comparisons=True, cache_reordering=True, rollback_and_restart=True)
    finally:
        db.kill()


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    p = argparse.ArgumentParser(); p.add_argument('--output', required=True); args = p.parse_args()
    out = Path(args.output); out.mkdir(parents=True, exist_ok=False)
    try:
        result = run(out / 'data')
    except Exception as error:
        import traceback
        result = dict(passed=False, error=str(error), traceback=traceback.format_exc())
    (out / 'report.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result), flush=True)
    sys.exit(0 if result['passed'] else 1)
