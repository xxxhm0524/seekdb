#!/usr/bin/env python3
"""Model-check dirty-page prefetch, eviction, overlays and crash recovery."""
import argparse
import math
import os
import random
import sys
import tempfile

sys.path.insert(0,os.path.dirname(__file__))
from protocol_client import Client


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rounds',type=int,default=20)
    parser.add_argument('--seed',type=int,default=27182)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    def vector():
        return [rng.randrange(6) for _ in range(4)]
    def literal(v):
        return '['+','.join(map(str,v))+']'
    with tempfile.TemporaryDirectory(prefix='mvd-cache-') as data:
        client = Client(datadir=data,extra=['--cache-mb=1','--query-threads=2'])
        model = {i:('a' if i%2 else 'b',vector()) for i in range(1,5001)}
        def sql(text,mode='exact',**fields):
            response = client.request(text,mode)
            assert response.get('ok') is True,(text[:120],response)
            for key,value in fields.items():
                assert response.get(key)==value,(text[:120],response)
            return response
        def check_queries():
            for mode in ('exact','ann'):
                for category in (None,'a'):
                    query = vector()
                    truth = sorted((sum((x-y)**2 for x,y in zip(v,query)),ident)
                        for ident,(cat,v) in model.items() if category is None or cat==category)[:10]
                    where = " WHERE s='%s'" % category if category is not None else ''
                    rows = sql("SELECT id,L2_DISTANCE(v,'%s') AS d FROM t%s ORDER BY d LIMIT 10" %
                        (literal(query),where),mode)['rows']
                    assert [r[0] for r in rows]==[ident for _,ident in truth],(mode,category,rows,truth)
                    assert all(math.isclose(row[1],math.sqrt(expected[0]),rel_tol=1e-7,abs_tol=1e-7)
                        for row,expected in zip(rows,truth)),rows
        def check_contents():
            rows = sql('SELECT id,s,v FROM t ORDER BY id')['rows']
            expected = [[ident,cat,v] for ident,(cat,v) in sorted(model.items())]
            assert rows==expected,'full contents differ from committed model'
        try:
            sql('CREATE TABLE t (id BIGINT PRIMARY KEY,s VARCHAR(256),v VECTOR(4))')
            sql('CREATE VECTOR INDEX vi ON t(v)')
            for start in range(1,5001,100):
                values = ','.join("(%d,'%s','%s')" % (i,model[i][0],literal(model[i][1]))
                    for i in range(start,start+100))
                sql('INSERT INTO t VALUES '+values,affected_rows=100)
            check_queries()
            for turn in range(args.rounds):
                before = dict(model)
                sql('BEGIN')
                for ident in rng.sample(list(model),5):
                    cat,v = rng.choice(('a','b')),vector()
                    sql("UPDATE t SET s='%s',v='%s' WHERE id=%d" % (cat,literal(v),ident),affected_rows=1)
                    model[ident]=(cat,v)
                removed = rng.choice(list(model))
                sql('DELETE FROM t WHERE id=%d' % removed,affected_rows=1)
                del model[removed]
                added = 9007199254740993+turn
                cat,v = 'a',vector()
                sql("INSERT INTO t VALUES (%d,'%s','%s')" % (added,cat,literal(v)),affected_rows=1)
                model[added]=(cat,v)
                # A failing multi-row statement must not leave a new row or
                # discard prior patches in the active transaction.
                duplicate = rng.choice(list(model))
                response = client.request("INSERT INTO t VALUES (-1,'a','[0,0,0,0]'),(%d,'a','[0,0,0,0]')" % duplicate)
                assert response.get('ok') is False,response
                check_queries()
                if turn%3==0:
                    sql('ROLLBACK'); model=before
                else:
                    sql('COMMIT')
                check_queries()
                if turn%5==4:
                    client.kill(); client.start()
                    check_contents();check_queries()
            check_contents()
        finally:
            client.close()
    print('cache coherence: PASS (%d rounds, seed %d, heap exceeds 1 MiB cache)' % (args.rounds,args.seed))


if __name__=='__main__':
    main()
