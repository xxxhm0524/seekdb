#!/usr/bin/env python3
"""JSONL timing, durable content checks, and float64 TopK truth.

SQL generation and correctness checks are outside timed regions.
These development timings do not reproduce the platform's cgroup T/B/M score.
"""
import argparse
from contextlib import ExitStack
import json
import math
import os
from pathlib import Path
import sys
import tempfile
import time

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from protocol_client import Client, DEFAULT_BINARY


def vector_literal(vector):
    # Nine significant digits round-trip every finite float32 value.
    return '[' + ','.join('%.9g' % float(x) for x in vector) + ']'


def main(client_factory=Client):
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', default=DEFAULT_BINARY)
    parser.add_argument('--rows', type=int, default=10000)
    parser.add_argument('--dim', type=int, default=128)
    parser.add_argument('--queries', type=int, default=1000)
    parser.add_argument('--topk', type=int, default=10)
    parser.add_argument('--seed', type=int, default=20261109)
    parser.add_argument('--search-mode', choices=['exact','ann'], default='exact')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    rng = np.random.default_rng(args.seed)
    vectors = rng.random((args.rows,args.dim), dtype=np.float32)
    cats = np.array(['c%d' % (i%10) for i in range(args.rows)])
    ids = np.arange(1,args.rows+1,dtype=np.int64)
    batches = []
    for start in range(0,args.rows,100):
        stop = min(start+100,args.rows)
        parts = ["(%d,'%s','%s')" % (ids[i],cats[i],vector_literal(vectors[i])) for i in range(start,stop)]
        batches.append(('INSERT INTO items VALUES '+','.join(parts), stop-start))
    query_vectors = np.random.default_rng(args.seed+1).random((args.queries,args.dim),dtype=np.float32)
    queries = []
    for i,query in enumerate(query_vectors):
        cat = 'c%d' % (i%10) if i < args.queries//2 else None
        where = " WHERE cat='%s'" % cat if cat is not None else ''
        sql = "SELECT id,L2_DISTANCE(v,'%s') AS d FROM items%s ORDER BY d LIMIT %d" % (
            vector_literal(query),where,args.topk)
        queries.append((sql,cat))
    with tempfile.TemporaryDirectory(prefix='mvdbench_') as data, ExitStack() as cleanup:
        client = client_factory(args.binary,data,extra=['--query-threads=2'])
        cleanup.callback(client.close)
        def execute(sql):
            response = client.request(sql,args.search_mode)
            assert response.get('ok') is True, (sql[:120],response)
            return response
        execute('CREATE TABLE items (id BIGINT PRIMARY KEY,cat VARCHAR(8),v VECTOR(%d))' % args.dim)
        before = time.perf_counter()
        for sql,count in batches:
            assert execute(sql).get('affected_rows') == count
        insert_time = time.perf_counter()-before
        write_resources = client.resource_snapshot() if hasattr(client,'resource_snapshot') else None
        client.kill()
        client.start()
        for start in range(0,args.rows,256):
            stop = min(start+256,args.rows)
            response = execute('SELECT id,cat,v FROM items WHERE id>=%d AND id<=%d ORDER BY id' % (start+1,stop))
            assert [r[0] for r in response['rows']] == ids[start:stop].tolist(), 'durable IDs differ'
            assert [r[1] for r in response['rows']] == cats[start:stop].tolist(), 'durable categories differ'
            actual = np.array([r[2] for r in response['rows']],dtype=np.float32)
            assert np.array_equal(actual,vectors[start:stop]), 'durable vectors differ'
        execute('CREATE VECTOR INDEX vi ON items(v)')
        for index in (0, min(args.queries-1,args.queries//2)):
            execute(queries[index][0])
        results = []
        before = time.perf_counter()
        for sql,_ in queries:
            results.append(execute(sql)['rows'])
        query_time = time.perf_counter()-before
        exact_mismatches = 0
        recall = {'filtered':[],'unfiltered':[]}
        double_vectors = vectors.astype(np.float64)
        for i,rows in enumerate(results):
            _,cat = queries[i]
            candidates = np.flatnonzero(cats==cat) if cat is not None else np.arange(args.rows)
            differences = double_vectors[candidates]-query_vectors[i].astype(np.float64)
            distances2 = np.einsum('ij,ij->i',differences,differences)
            order = np.lexsort((ids[candidates],distances2))[:args.topk]
            truth = ids[candidates][order].tolist()
            got = [r[0] for r in rows]
            assert len(got)==len(truth) and len(got)==len(set(got)), ('wrong count/duplicate IDs',i,got)
            assert all(1<=ident<=args.rows for ident in got), ('invalid ID',i,got)
            assert cat is None or all(cats[ident-1]==cat for ident in got), ('filter violation',i)
            for ident,distance in rows:
                expected = np.linalg.norm(double_vectors[ident-1]-query_vectors[i].astype(np.float64))
                assert math.isclose(distance,expected,rel_tol=1e-7,abs_tol=1e-7), ('wrong distance',i,ident,distance,expected)
            if truth:
                recall['filtered' if cat is not None else 'unfiltered'].append(len(set(got)&set(truth))/len(truth))
            if got != truth:
                exact_mismatches += 1
        mean_recall = {key:float(np.mean(value)) if value else 1.0 for key,value in recall.items()}
        assert all(value>=0.95 for value in mean_recall.values()), mean_recall
        if args.search_mode=='exact':
            assert exact_mismatches==0, ('exact TopK differs',exact_mismatches)
        query_resources = client.resource_snapshot() if hasattr(client,'resource_snapshot') else None
        client.shutdown()
    report = dict(rows=args.rows,dim=args.dim,queries=args.queries,topk=args.topk,seed=args.seed,
        mode=args.search_mode,insert_seconds=round(insert_time,6),query_seconds=round(query_time,6),
        ms_per_query=round(query_time*1000/args.queries,6),recall_at_k=mean_recall,
        exact_query_mismatches=exact_mismatches,durable_content_verified=True,
        timing_scope='prepared SQL over JSONL; includes responses and commit fsync; excludes SQL generation and validation')
    if write_resources is not None:
        report['docker_resources'] = dict(write=write_resources,query=query_resources,
            scope='diagnostic container peaks and raw io.stat; query setup includes durable-content checks; not official scoring')
    print(json.dumps(report,ensure_ascii=False,indent=2))
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    return 0


if __name__=='__main__':
    sys.exit(main())
