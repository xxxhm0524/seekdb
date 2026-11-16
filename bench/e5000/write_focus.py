"""Interleaved real write requests; exact file bytes and reopened f32 bits checked."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import statistics
import struct
import time
from native_test_support import NativeSession, save
from native_ab_e860 import resources


def run(a):
    out = Path(a.output); out.mkdir(exist_ok=False)
    os.sched_setaffinity(0, {a.driver_cpu})
    os.environ['MVD_NATIVE_CPU'] = str(a.server_cpu)
    programs = {'baseline': Path(a.baseline), 'candidate': Path(a.candidate)}
    records = []
    for dim, n, body_length in ([{'wide':(768,3000,16),'default':(128,10000,16),'long':(128,10000,1024),'wide-long':(768,3000,1024)}[a.shape]]):
        if a.rows is not None: n=a.rows
        assert n>=100 and n%100==0
        rng = random.Random(2026111511 + dim + body_length)
        vectors = [[struct.unpack('<f', struct.pack('<f', rng.uniform(-1, 1)))[0] for _ in range(dim)] for _ in range(n)]
        vectors[0][:4] = [-0.0, 0.0, 1.401298464324817e-45, 3.4028234663852886e38]
        bodies = [('row' + str(k) + ':' + ('x' if k % 7 else '\u4e2d\U0001f600') * body_length) for k in range(n)]
        order = list(range(n)); rng.shuffle(order)
        inserts = ['INSERT INTO t VALUES ' + ','.join(f"({k},'{bodies[k]}','{json.dumps(vectors[k], separators=(',', ':'))}')" for k in order[start:start + 100]) + ';' for start in range(0, n, 100)]
        fixture_hash = hashlib.sha256('\n'.join(inserts).encode()).hexdigest()
        shape = f'd{dim}-body{body_length}'
        for round_no in range(a.rounds):
            dbs = {}
            try:
                for name, binary in programs.items():
                    dbs[name] = NativeSession(out, f'{shape}-r{round_no + 1}-{name}', binary)
                    dbs[name].execute(f'CREATE TABLE t(id BIGINT PRIMARY KEY,body VARCHAR(4096),v VECTOR({dim}));')
                before = {name: resources(db) for name, db in dbs.items()}
                elapsed = {name: 0 for name in programs}; timings = {name: [] for name in programs}
                requests = []
                if a.whole:
                    names = list(programs)
                    if round_no % 2: names.reverse()
                    requests = [(name, i) for name in names for i in range(len(inserts))]
                else:
                    for index in range(len(inserts)):
                        names = list(programs)
                        if (round_no + index) % 2: names.reverse()
                        requests.extend((name, index) for name in names)
                for name, index in requests:
                    start = time.perf_counter_ns(); response = dbs[name].execute(inserts[index])
                    ns = time.perf_counter_ns() - start
                    assert response['affected_rows'] == 100
                    timings[name].append(ns / 1e6); elapsed[name] += ns
                hashes = {}
                for name, db in dbs.items():
                    path = Path(db.temp.name) / 'database.mvd'
                    hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
                    after = resources(db)
                    records.append(dict(shape=shape, round=round_no + 1, version=name, rows=n, dimensions=dim, body_length=body_length,
                        write_ms=elapsed[name] / 1e6, requests_ms=timings[name], resources=after,
                        write_io_delta={k: after['process_io'][k] - before[name]['process_io'][k] for k in after['process_io']},
                        database_sha256=hashes[name], database_bytes=path.stat().st_size, fixture_sha256=fixture_hash))
                assert len(set(hashes.values())) == 1, ('database bytes differ', hashes)
                for db in dbs.values():
                    db.restart(crash=True)
                    for k in [0, 1, 7, n // 2, n - 1]:
                        row = db.execute(f'SELECT id,body,v FROM t WHERE id={k};')['rows'][0]
                        assert row[:2] == [k, bodies[k]]
                        assert struct.pack('<' + str(dim) + 'f', *row[2]) == struct.pack('<' + str(dim) + 'f', *vectors[k])
                print(json.dumps(dict(shape=shape, round=round_no + 1, write_ms={k: v / 1e6 for k, v in elapsed.items()}, speedup=elapsed['baseline'] / elapsed['candidate'], identical_file=True)), flush=True)
            finally:
                for db in dbs.values():
                    try: db.shutdown()
                    finally: db.close()
    summary = {}
    for shape in sorted({r['shape'] for r in records}):
        group = {name: [r for r in records if r['shape'] == shape and r['version'] == name] for name in programs}
        ratios = [b['write_ms'] / c['write_ms'] for b, c in zip(group['baseline'], group['candidate'])]
        summary[shape] = dict(paired_speedups=ratios, median_paired_speedup=statistics.median(ratios))
    report = dict(passed=True, rounds=a.rounds, versions={n: hashlib.sha256(p.read_bytes()).hexdigest() for n, p in programs.items()},
        driver_cpu=a.driver_cpu, server_cpu=a.server_cpu, records=records, summary=summary,
        scope=('Actual JSONL durable writes; whole processes alternate by round.' if a.whole else 'Actual JSONL durable writes; versions alternate by batch.')+' Fixed CPUs are not reserved. No formal E is inferred.')
    save(out / 'report.json', report); print(json.dumps({k: v for k, v in report.items() if k != 'records'}), flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--output', required=True); p.add_argument('--baseline', required=True); p.add_argument('--candidate', required=True)
    p.add_argument('--rounds', type=int, default=4); p.add_argument('--driver-cpu', type=int, default=3); p.add_argument('--server-cpu', type=int, default=2)
    p.add_argument('--rows',type=int)
    p.add_argument('--whole',action='store_true')
    p.add_argument('--shape',choices=['wide','default','long','wide-long'],default='wide')
    p.add_argument('--gate-slack',type=float,default=0.01)
    p.add_argument('--gate',action='store_true')
    a=p.parse_args();run(a)
    report=json.loads((Path(a.output)/'report.json').read_text())
    ratios=next(iter(report['summary'].values()))['paired_speedups']
    regression=1/statistics.median(ratios)-1
    gate=dict(performance_gate_passed=regression<=a.gate_slack,median_regression_pct=regression*100,allowed_regression_pct=a.gate_slack*100,paired_speedups=ratios)
    save(Path(a.output)/'performance-gate.json',gate);print(json.dumps(gate),flush=True)
    if a.gate:raise SystemExit(0 if gate['performance_gate_passed'] else 2)
