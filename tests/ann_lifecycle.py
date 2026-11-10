#!/usr/bin/env python3
"""HNSW exact/ann switching, filtered recall, updates, rollback and restart."""
from contextlib import ExitStack
from pathlib import Path
import sys,tempfile
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parent))
from protocol_client import Client
from bench import vector_literal

def main():
    rng=np.random.default_rng(713)
    vectors={i:rng.random(128,dtype=np.float32) for i in range(1,4001)}
    categories={i:'a' if i%2 else 'b' for i in vectors}
    with tempfile.TemporaryDirectory(prefix='mvd-ann-') as data, ExitStack() as cleanup:
        client=Client(datadir=data,extra=['--cache-mb=1','--ann-min-rows=2048','--tx-memory-mb=1'])
        cleanup.callback(client.close)
        def sql(text,mode='exact',**fields):
            r=client.request(text,mode)
            assert r.get('ok') is True,(text[:160],r)
            for k,v in fields.items():assert r.get(k)==v,(text[:160],r)
            return r
        def check():
            ids=np.array(sorted(vectors),dtype=np.int64)
            matrix=np.array([vectors[int(i)] for i in ids],dtype=np.float64)
            cats=np.array([categories[int(i)] for i in ids])
            recalls={False:[],True:[]}
            for index in range(20):
                q=rng.random(128,dtype=np.float32)
                category='a' if index%2 else None
                mask=cats==category if category else np.ones(len(ids),dtype=bool)
                candidates=np.flatnonzero(mask)
                diff=matrix[candidates]-q.astype(np.float64)
                squared=np.einsum('ij,ij->i',diff,diff)
                truth=ids[candidates[np.lexsort((ids[candidates],squared))[:10]]].tolist()
                where=" WHERE cat='a'" if category else ''
                for mode in ('exact','ann'):
                    rows=sql("SELECT id,L2_DISTANCE(v,'%s') AS d FROM t%s ORDER BY d LIMIT 10"%(vector_literal(q),where),mode)['rows']
                    got=[r[0] for r in rows]
                    assert len(got)==len(truth)==len(set(got))
                    assert all(i in vectors and (not category or categories[i]==category) for i in got)
                    for ident,distance in rows:
                        expected=np.linalg.norm(vectors[ident].astype(np.float64)-q.astype(np.float64))
                        assert np.isclose(distance,expected,rtol=1e-7,atol=1e-7)
                    if mode=='exact':assert got==truth
                    else:recalls[bool(category)].append(len(set(got)&set(truth))/len(truth))
            assert all(np.mean(group)>=0.95 for group in recalls.values()),recalls
            # Unsupported cached attributes must still filter before TopK.
            rows=sql("SELECT id FROM t WHERE content='absent' ORDER BY L2_DISTANCE(v,'%s') LIMIT 10"%vector_literal(q),'ann')['rows']
            assert rows==[],rows
        sql('CREATE TABLE t(id BIGINT PRIMARY KEY,cat VARCHAR(8),content VARCHAR(256),v VECTOR(128))')
        for start in range(1,4001,100):
            values=','.join("(%d,'%s','payload','%s')"%(i,categories[i],vector_literal(vectors[i])) for i in range(start,start+100))
            sql('INSERT INTO t VALUES '+values,affected_rows=100)
        sql('CREATE VECTOR INDEX vi ON t(v)')
        check()
        original_vectors=dict(vectors);original_categories=dict(categories)
        sql('BEGIN')
        for i in range(1,31):
            vectors[i]=rng.random(128,dtype=np.float32);categories[i]='b'
            sql("UPDATE t SET cat='b',v='%s' WHERE id=%d"%(vector_literal(vectors[i]),i),affected_rows=1)
        sql("SELECT id FROM t WHERE cat='b' ORDER BY L2_DISTANCE(v,'%s') LIMIT 1"%vector_literal(vectors[1]),'ann',rows=[[1]])
        check()
        sql('ROLLBACK');vectors=original_vectors;categories=original_categories
        check()
        sql('BEGIN')
        for i in range(1,101):
            vectors[i]=rng.random(128,dtype=np.float32);categories[i]='b'
            sql("UPDATE t SET cat='b',v='%s' WHERE id=%d"%(vector_literal(vectors[i]),i),affected_rows=1)
        for i in range(101,121):
            sql('DELETE FROM t WHERE id=%d'%i,affected_rows=1);del vectors[i];del categories[i]
        for i in range(4001,4021):
            vectors[i]=rng.random(128,dtype=np.float32);categories[i]='a'
            sql("INSERT INTO t VALUES (%d,'a','payload','%s')"%(i,vector_literal(vectors[i])),affected_rows=1)
        sql('COMMIT')
        sql("SELECT id FROM t ORDER BY L2_DISTANCE(v,'%s') LIMIT 1"%vector_literal(vectors[1]),'ann',rows=[[1]])
        sql("SELECT id FROM t WHERE id=101 ORDER BY L2_DISTANCE(v,'%s') LIMIT 1"%vector_literal(vectors[1]),'ann',rows=[])
        check()
        client.kill();client.start();check()
        print('ANN lifecycle: PASS (exact/ann, filtered, mutations, rollback, crash rebuild)')
if __name__=='__main__':main()
