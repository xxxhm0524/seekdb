#!/usr/bin/env python3
"""Independent filtered TopK model across predicate reuse and committed mutations."""
from contextlib import ExitStack
import copy
import math
import operator
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from protocol_client import Client


def main():
    categories = ['', 'a', 'b', '甲', '乙', "O'Reilly", '\0x', 'z', 'longer']
    ids = [-(2**63), *range(-128, 129), 2**63-1]
    model = {ident: dict(id=ident, cat=categories[i % len(categories)], n=i % 23,
        v=(i % 17, i % 11, i % 5, 1)) for i, ident in enumerate(ids)}
    comparisons = {'=': operator.eq, '<>': operator.ne, '<': operator.lt,
        '<=': operator.le, '>': operator.gt, '>=': operator.ge}
    predicates = [[('cat', op, cat)] for op in comparisons for cat in categories+['newgroup']]
    predicates += [[('n', op, bound)] for op in comparisons for bound in (0, 5, 16, 22, 23)]
    predicates += [[('cat', '=', cat), ('n', '>=', 5)] for cat in categories]
    predicates += [[('id', '=', ident)] for ident in (-(2**63), 0, 1000, 2000, 2**63-1)]
    predicates += [[], [('n', '>=', 0)]*400]
    def literal(value):
        return "'"+value.replace("'", "''")+"'" if isinstance(value, str) else str(value)
    def vector(value):
        return '['+','.join(str(x) for x in value)+']'
    with tempfile.TemporaryDirectory(prefix='mvd-filter-model-') as data, ExitStack() as cleanup:
        client = Client(datadir=data, extra=['--cache-mb=1'])
        cleanup.callback(client.close)
        def sql(text, mode='exact', **fields):
            response = client.request(text, mode)
            assert response.get('ok') is True, (text[:100], response)
            for key, expected in fields.items():
                assert response.get(key) == expected, (text[:100], response)
            return response
        def check():
            for conditions in predicates:
                where = ' AND '.join('%s%s%s' % (column, op, literal(value))
                    for column, op, value in conditions)
                eligible = [row for row in model.values() if all(comparisons[op](row[column], value)
                    for column, op, value in conditions)]
                for query, mode in (((1, 2, 3, 0), 'exact'), ((5, 0, 1, 2), 'ann')):
                    def distance(row):
                        return sum((a-b)**2 for a, b in zip(row['v'], query))
                    truth = sorted(eligible, key=lambda row: (distance(row), row['id']))[:10]
                    response = sql("SELECT id,L2_DISTANCE(v,'%s') AS d FROM t%s ORDER BY d LIMIT 10" %
                        (vector(query), ' WHERE '+where if where else ''), mode)
                    assert [row[0] for row in response['rows']] == [row['id'] for row in truth], (conditions, response)
                    for row, expected in zip(response['rows'], truth):
                        assert math.isclose(row[1], math.sqrt(distance(expected)), rel_tol=1e-7, abs_tol=1e-7), (conditions, query, row, expected, math.sqrt(distance(expected)))
        sql('CREATE TABLE t(id BIGINT PRIMARY KEY,cat VARCHAR(32),n BIGINT,v VECTOR(4))')
        records = list(model.values())
        for start in range(0, len(records), 100):
            batch = records[start:start+100]
            sql('INSERT INTO t VALUES '+','.join("(%d,%s,%d,'%s')" %
                (row['id'], literal(row['cat']), row['n'], vector(row['v'])) for row in batch), affected_rows=len(batch))
        sql('CREATE VECTOR INDEX vi ON t(v)')
        check()
        saved = copy.deepcopy(model)
        sql('BEGIN')
        sql("UPDATE t SET cat='newgroup',v='[0,0,0,0]' WHERE id=0", affected_rows=1)
        model[0]['cat'] = 'newgroup'; model[0]['v'] = (0, 0, 0, 0)
        check()
        sql('ROLLBACK'); model = saved
        check()
        sql('BEGIN')
        sql("UPDATE t SET cat='乙' WHERE id=0", affected_rows=1); model[0]['cat'] = '乙'
        sql("UPDATE t SET v='[0,0,0,0]' WHERE id=1", affected_rows=1); model[1]['v'] = (0, 0, 0, 0)
        sql('DELETE FROM t WHERE id=2', affected_rows=1); del model[2]
        sql("INSERT INTO t VALUES(1000,'newgroup',7,'[2,1,0,1]')", affected_rows=1)
        model[1000] = dict(id=1000, cat='newgroup', n=7, v=(2, 1, 0, 1))
        sql('COMMIT')
        check()
        response = client.request("INSERT INTO t VALUES(2000,'newgroup',1,'[0,0,0,0]'),(0,'dup',0,'[0,0,0,0]')")
        assert response.get('ok') is False, response
        check()
        client.kill(); client.start(); check()
        print('filter cache model: PASS (%d predicates, changing query vectors, mutation/rollback/failure/restart)' % len(predicates))


if __name__ == '__main__':
    main()
