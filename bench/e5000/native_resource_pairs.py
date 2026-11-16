"""Isolated per-sample cgroups; sampled memory is not mislabeled as memory.peak."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import threading
import time
from native_test_support import NativeSession, ROOT, save
from native_ab_e860 import workload, resources


def measure(a):
    out = ROOT / a.output; out.mkdir(exist_ok=False)
    base = Path(json.loads((ROOT / 'native-settings.json').read_text())['cgroup'])
    assert (base / 'cgroup.subtree_control').read_text().split()
    os.sched_setaffinity(0, {a.driver_cpu}); os.environ['MVD_NATIVE_CPU'] = str(a.server_cpu)
    inserts, queries, truth, body, fixture_hash = workload(out, 10000, 200, False, a.ordered_keys)
    programs = ({name: (ROOT / binary).resolve(strict=True) for name, binary in (item.split('=', 1) for item in a.program)} if a.program else
        {'e860': ROOT / 'build/baseline', 'b': ROOT / 'build/combined-b', 'c': ROOT / 'build/combined-c', 'd': ROOT / 'build/combined-d'})
    records = []
    for round_no in range(a.rounds):
        for kind in ('bigint', 'text'):
            for indexed in (False, True):
                names = list(programs)
                rotate = (round_no + int(indexed) + int(kind == 'text')) % len(names)
                names = names[rotate:] + names[:rotate]
                for name in names:
                    tag = f'resource-r{round_no + 1}-{kind}-idx{int(indexed)}-{name}'
                    cg = base / tag; cg.mkdir()
                    for key, value in [('cpu.max', '200000 100000'), ('memory.max', str(2 << 30)), ('memory.swap.max', '0'), ('pids.max', '128')]: (cg / key).write_text(value)
                    os.environ['MVD_NATIVE_CGROUP'] = str(cg)
                    samples = []; done = threading.Event(); errors = []
                    def sample():
                        while not done.is_set():
                            try: samples.append((time.perf_counter_ns(), int((cg / 'memory.current').read_text())))
                            except Exception as error: errors.append(str(error)); return
                            done.wait(.002)
                    sampler = threading.Thread(target=sample); sampler.start(); db = None
                    try:
                        db = NativeSession(out, tag, programs[name])
                        assert (cg / 'cgroup.procs').read_text().strip() == str(db.proc.pid)
                        db.execute('CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,category VARCHAR(4),v VECTOR(128),body VARCHAR(512));')
                        write_ns = 0
                        for sql in inserts:
                            start = time.perf_counter_ns(); response = db.execute(sql); write_ns += time.perf_counter_ns() - start
                            assert response['affected_rows'] == 100
                        start = time.perf_counter_ns()
                        if indexed: db.execute('CREATE VECTOR INDEX iv ON t(v);')
                        def check(response, i):
                            assert response['ok'] and response['columns'] == ['id', 'd'] and len(response['rows']) == len(truth[i])
                            for row, (key, distance) in zip(response['rows'], truth[i]):
                                assert row[0] == key and math.isclose(row[1], distance, rel_tol=1e-12, abs_tol=1e-12)
                        for i in range(10): check(db.execute(queries[kind][i]), i)
                        prepare_ns = time.perf_counter_ns() - start
                        query_ns = 0
                        for i, sql in enumerate(queries[kind]):
                            start = time.perf_counter_ns(); response = db.execute(sql); query_ns += time.perf_counter_ns() - start; check(response, i)
                        info = resources(db)
                        info['process_status'] = {k: v.strip() for k, v in (x.split(':', 1) for x in (Path('/proc') / str(db.proc.pid) / 'status').read_text().splitlines() if ':' in x) if k in ('VmRSS', 'RssAnon', 'RssFile')}
                        db.shutdown()
                        done.set(); sampler.join(); assert not errors, errors
                        # Correctness inventory is outside the measured memory window.
                        directory = Path(db.temp.name)
                        files = {str(f.relative_to(directory)): dict(bytes=f.stat().st_size, sha256=hashlib.sha256(f.read_bytes()).hexdigest())
                            for f in sorted(directory.rglob('*')) if f.is_file()}
                        assert files, 'no durable database files'
                        db_hash = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest(); size = sum(f['bytes'] for f in files.values())
                        records.append(dict(round=round_no + 1, kind=kind, indexed=indexed, version=name,
                            write_ms=write_ns / 1e6, prepare_ms=prepare_ns / 1e6, query_ms=query_ns / 1e6,
                            sampled_cgroup_max_bytes=max(v for _, v in samples), sample_interval_target_ms=2, samples=len(samples),
                            actual_memory_peak_bytes=int((cg / 'memory.peak').read_text()) if (cg / 'memory.peak').exists() else None,
                            cgroup_memory_stat=(cg / 'memory.stat').read_text(), cgroup_events=(cg / 'memory.events').read_text(),
                            raw_cgroup_io_stat=(cg / 'io.stat').read_text(), resources=info, database_sha256=db_hash, database_bytes=size, database_files=files,
                            physical_storage_bytes=sum(f.stat().st_blocks * 512 for f in directory.rglob('*') if f.is_file())))
                        save(out / (tag + '.json'), records[-1]); save(out / (tag + '-samples.json'), samples)
                        print(json.dumps({k: records[-1][k] for k in ('round', 'kind', 'indexed', 'version', 'write_ms', 'prepare_ms', 'query_ms', 'sampled_cgroup_max_bytes')}), flush=True)
                    finally:
                        done.set(); sampler.join()
                        if db is not None: db.close()
                        cg.rmdir()
    summary = {}
    for kind in ('bigint', 'text'):
        for indexed in (False, True):
            group = {name: [r for r in records if r['kind'] == kind and r['indexed'] == indexed and r['version'] == name] for name in programs}
            summary[kind + '-idx' + str(int(indexed))] = {name: {key: statistics.median(r[key] for r in values) for key in ['write_ms', 'prepare_ms', 'query_ms', 'sampled_cgroup_max_bytes']} for name, values in group.items()}
            if not a.allow_format_difference:
                for i in range(a.rounds): assert len({v[i]['database_sha256'] for v in group.values()}) == 1
    report = dict(passed=True, versions={n: hashlib.sha256(p.read_bytes()).hexdigest() for n, p in programs.items()}, fixture_sha256=fixture_hash,
        rounds=a.rounds, queries=200, ordered_keys=a.ordered_keys, verified_queries=len(records) * 200, records=records, summary=summary, identical_file_bytes_required=not a.allow_format_difference,
        scope='Fresh per-sample cgroups with 2 CPU/2 GiB/no swap. Native constrained default entrypoint. Sampled memory.current includes page cache but is a lower bound on the true peak. Raw device IO is retained without asserting the platform device attribution. No formal E is inferred.')
    save(out / 'report.json', report); print(json.dumps({k: v for k, v in report.items() if k != 'records'}), flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--output', default='native-evidence/resource-four-way'); p.add_argument('--rounds', type=int, default=3)
    p.add_argument('--program', action='append', help='name=relative/binary/path'); p.add_argument('--allow-format-difference', action='store_true')
    p.add_argument('--ordered-keys', action='store_true')
    p.add_argument('--driver-cpu', type=int, default=3); p.add_argument('--server-cpu', type=int, default=2)
    measure(p.parse_args())
