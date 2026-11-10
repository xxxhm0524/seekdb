#!/usr/bin/env python3
"""Paired T/B/M measurements, including setup and final writeback in B/M.

Uses a holding shell only for resource collection after the database exits.
The production Docker entrypoint and protocol checker are unchanged. The
reference is a specified source revision, not the unpublished official baseline.
"""
import argparse
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time
import uuid

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT/'protocol_check'))
from _transport import Session
from bench import vector_literal


def held_image(image):
    tag = 'mvd-score-hold-'+uuid.uuid4().hex
    with tempfile.TemporaryDirectory(prefix='mvd-score-image-') as temp:
        directory = Path(temp)
        (directory/'Dockerfile').write_text('ARG SOURCE_IMAGE\nFROM ${SOURCE_IMAGE}\n'
            'COPY hold.sh /app/score-hold.sh\nENTRYPOINT ["/bin/sh","/app/score-hold.sh"]\n', encoding='utf-8')
        (directory/'hold.sh').write_text('/app/minivectordb --data-dir=/data\n'
            'status=$?\nprintf "%s\\n" "$status" > /data/.score-exit-status\n'
            'exec /bin/sleep 600\n', encoding='utf-8')
        result = subprocess.run(['docker', 'build', '--build-arg', 'SOURCE_IMAGE='+image,
            '-t', tag, str(directory)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        if result.returncode:
            raise RuntimeError(result.stdout.decode(errors='replace')[-2000:])
    return tag


def drop_caches(enabled):
    if enabled:
        assert sys.platform == 'linux' and os.environ.get('GITHUB_ACTIONS') == 'true', \
            'Cache control is restricted to a dedicated GitHub Actions runner'
        subprocess.run(['sudo', 'sync'], check=True, timeout=30)
        subprocess.run(['sudo', 'tee', '/proc/sys/vm/drop_caches'], input=b'3\n',
            stdout=subprocess.DEVNULL, check=True, timeout=30)


def resources(session, device):
    raw = session.docker('exec', session.name, 'cat', '/sys/fs/cgroup/memory.peak',
        '/sys/fs/cgroup/io.stat').stdout.decode().splitlines()
    peak = int(raw[0])
    counters = {}
    for line in raw[1:]:
        fields = line.split()
        if fields:
            counters[fields[0]] = {key: int(value) for key, value in
                (field.split('=', 1) for field in fields[1:])}
    nonzero = [key for key, value in counters.items() if value.get('rbytes', 0)+value.get('wbytes', 0)]
    if not device:
        assert len(nonzero) == 1, ('Select the measured data device explicitly', counters)
        device = nonzero[0]
    assert device in counters and peak > 0, ('Missing cgroup measurement', counters, peak)
    values = counters[device]
    return dict(memory_peak_bytes=peak, io_bytes=values['rbytes']+values['wbytes'],
        io_device=device, io_stat=counters)


def workload(seed, rows, queries):
    vectors = np.random.default_rng(seed).random((rows, 128), dtype=np.float32)
    categories = np.array(['c%d' % (i % 10) for i in range(rows)])
    batches = []
    for start in range(0, rows, 100):
        stop = min(start+100, rows)
        batches.append(('INSERT INTO items VALUES '+','.join("(%d,'%s','%s')" %
            (i+1, categories[i], vector_literal(vectors[i])) for i in range(start, stop)), stop-start))
    query_vectors = np.random.default_rng(seed+1).random((queries, 128), dtype=np.float32)
    sql_queries = []
    for i, vector in enumerate(query_vectors):
        category = 'c%d' % (i % 10) if i < queries//2 else None
        where = " WHERE cat='%s'" % category if category else ''
        sql_queries.append(("SELECT id,L2_DISTANCE(v,'%s') AS d FROM items%s ORDER BY d LIMIT 10" %
            (vector_literal(vector), where), category))
    return vectors, categories, batches, query_vectors, sql_queries


def validate_queries(data, results, mode):
    vectors, categories, _, query_vectors, queries = data
    double_vectors = vectors.astype(np.float64)
    recall = {'filtered': [], 'unfiltered': []}
    for i, rows in enumerate(results):
        category = queries[i][1]
        eligible = np.flatnonzero(categories == category) if category else np.arange(len(vectors))
        delta = double_vectors[eligible]-query_vectors[i].astype(np.float64)
        distances = np.einsum('ij,ij->i', delta, delta)
        order = np.lexsort((eligible, distances))[:10]
        truth = (eligible[order]+1).tolist()
        actual = [row[0] for row in rows]
        assert len(actual) == len(truth) == len(set(actual)), (i, actual)
        assert all(1 <= ident <= len(vectors) and (category is None or categories[ident-1] == category)
            for ident in actual), (i, actual)
        for ident, distance in rows:
            expected = np.linalg.norm(double_vectors[ident-1]-query_vectors[i].astype(np.float64))
            assert math.isclose(distance, expected, rel_tol=1e-7, abs_tol=1e-7), (i, ident)
        if mode == 'exact':
            assert actual == truth, (i, actual, truth)
        recall['filtered' if category else 'unfiltered'].append(len(set(actual)&set(truth))/len(truth))
    means = {key: float(np.mean(value)) for key, value in recall.items()}
    assert all(value >= 0.95 for value in means.values()), means
    return means


def phase(image, held, name, data, args, config):
    vectors, categories, batches, _, queries = data
    log = args.output.parent/('score-'+uuid.uuid4().hex+'.stderr.log')
    session = Session(config, log, image=held)
    def sql(text):
        response = session.request(text, args.search_mode)
        assert response.get('ok') is True, (text[:80], response)
        return response
    try:
        drop_caches(args.drop_caches)
        session.start()
        sql('CREATE TABLE items(id BIGINT PRIMARY KEY,cat VARCHAR(8),v VECTOR(128))')
        before = time.perf_counter()
        for text, count in batches:
            assert sql(text).get('affected_rows') == count
        elapsed = time.perf_counter()-before
        index_time = None
        results = []
        if name == 'query':
            before = time.perf_counter()
            sql('CREATE VECTOR INDEX vi ON items(v)')
            index_time = time.perf_counter()-before
            for index in (0, len(queries)//2):
                sql(queries[index][0])
            before = time.perf_counter()
            for text, _ in queries:
                results.append(sql(text)['rows'])
            elapsed = time.perf_counter()-before
        assert session.request(op='shutdown')['ok'] is True
        until = time.monotonic()+config['request_timeout']
        while True:
            result = session.docker('exec', session.name, 'cat', '/data/.score-exit-status', check=False)
            if result.returncode == 0:
                assert result.stdout.strip() == b'0', result.stdout
                break
            assert time.monotonic() < until, 'database did not finish shutdown'
        session.docker('exec', session.name, 'sync', '-f', '/data')
        measurement = dict(seconds=elapsed, **resources(session, args.io_device))
        # End the measured cgroup before any content-validation requests.
        session.crash()
        if name == 'query':
            measurement['recall_at_10'] = validate_queries(data, results, args.search_mode)
            measurement['index_seconds'] = index_time
        session.image = image
        session.start()
        for start in range(0, len(vectors), 256):
            stop = min(start+256, len(vectors))
            rows = sql('SELECT id,cat,v FROM items WHERE id>=%d AND id<=%d ORDER BY id' % (start+1, stop))['rows']
            assert [row[0] for row in rows] == list(range(start+1, stop+1))
            assert [row[1] for row in rows] == categories[start:stop].tolist()
            assert np.array_equal(np.array([row[2] for row in rows], dtype=np.float32), vectors[start:stop])
        session.shutdown()
        measurement['content_verified_after_measurement'] = True
        return measurement
    finally:
        session.close()


def indicator(value, reference):
    total = 0.0
    for field, epsilon, weight in [('seconds', 0.001, 0.6), ('io_bytes', 2**20, 0.2),
            ('memory_peak_bytes', 2**20, 0.2)]:
        ratio = (reference[field]+epsilon)/(value[field]+epsilon)
        total += weight*ratio/(1+ratio)
    return total


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference-image', required=True)
    parser.add_argument('--candidate-image', required=True)
    parser.add_argument('--reference-sha', required=True)
    parser.add_argument('--rows', type=int, default=10000)
    parser.add_argument('--queries', type=int, default=1000)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--seed', type=int, default=20261110)
    parser.add_argument('--search-mode', choices=['exact', 'ann'], default='exact')
    parser.add_argument('--drop-caches', action='store_true')
    parser.add_argument('--io-device')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert args.rows >= 10 and args.queries >= 2 and args.rounds >= 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    config = json.loads((ROOT/'protocol_check/config.json').read_text(encoding='utf-8'))
    images = {'reference': args.reference_image, 'candidate': args.candidate_image}
    held = {name: held_image(image) for name, image in images.items()}
    report = dict(reference_sha=args.reference_sha, cold_cache_control=args.drop_caches,
        scope='separate write/query cgroups; B/M include startup, load, index, warmup, queries, shutdown and sync; verification is separate',
        shell_overhead='same holding shell in both images to preserve cgroups after database exit',
        official_score=False, rounds=[])
    try:
        for trial in range(args.rounds):
            data = workload(args.seed+trial*100, args.rows, args.queries)
            round_result = dict(seed=args.seed+trial*100)
            for name in (('reference', 'candidate') if trial % 2 == 0 else ('candidate', 'reference')):
                round_result[name] = {}
                for item in ('write', 'query'):
                    measured = phase(images[name], held[name], item, data, args, config)
                    round_result[name][item] = measured
                    print(json.dumps(dict(round=trial+1, version=name, item=item, **measured)), flush=True)
            report['rounds'].append(round_result)
            args.output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
        reference = {item: {field: statistics.median(row['reference'][item][field] for row in report['rounds'])
            for field in ('seconds', 'io_bytes', 'memory_peak_bytes')} for item in ('write', 'query')}
        indices = [(indicator(row['candidate']['write'], reference['write'])+
            2*indicator(row['candidate']['query'], reference['query']))/3 for row in report['rounds']]
        report.update(reference_medians=reference, candidate_comparison_indicators=indices,
            median_comparison_indicator=round(statistics.median(indices), 6), passed=True)
        args.output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
        print('PAIRED COMPARISON', report['median_comparison_indicator'], '(reference revision, not official baseline)', flush=True)
    finally:
        for image in held.values():
            subprocess.run(['docker', 'image', 'rm', image], stdout=subprocess.PIPE, stderr=subprocess.PIPE)


if __name__ == '__main__':
    main()
