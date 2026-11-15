"""Live bytes of both recovery roots, real reclaim kills, errors and old-version reads."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
from client import Client


def checksum(b):
    value = 14695981039346656037
    for x in b: value = ((value ^ x) * 1099511628211) & ((1 << 64) - 1)
    return value


def live_bytes(path):
    with path.open('rb') as f:
        def read(offset, length):
            f.seek(offset); b = f.read(length); assert len(b) == length
            return b
        slots = {}
        for slot in (0, 1):
            h = read(slot * 4096, 4096)
            if h[:8] != b'MVDCP001' or checksum(h[:4088]) != int.from_bytes(h[4088:], 'little'): continue
            generation, offset, length, end, expected = struct.unpack_from('<5Q', h, 8)
            catalog = read(offset, length); assert checksum(catalog) == expected
            pages, rows = {}, {}
            def walk(offset):
                assert offset >= 8192 and offset % 4096 == 0 and offset + 4096 <= end
                assert offset not in pages, 'tree cycle/shared page inside a root'
                page = read(offset, 4096); assert checksum(page[:4088]) == int.from_bytes(page[4088:], 'little'), ('live page damaged', generation, offset)
                pages[offset] = hashlib.sha256(page).hexdigest(); count = int.from_bytes(page[2:4], 'little')
                if page[0] == 1:
                    assert count <= 169
                    for i in range(count):
                        ref, size = struct.unpack_from('<QI', page, 24 + 24 * i)
                        assert ref >= 8192 and ref + size <= end
                        rows[str(ref) + ':' + str(size)] = hashlib.sha256(read(ref, size)).hexdigest()
                else:
                    assert page[0] == 2 and count <= 254
                    walk(int.from_bytes(page[8:16], 'little'))
                    for i in range(count): walk(int.from_bytes(page[24 + 16*i:32 + 16*i], 'little'))
            for table in json.loads(catalog):
                if table['root']: walk(table['root'])
            slots[slot] = dict(generation=generation, header_sha256=hashlib.sha256(h).hexdigest(), catalog_sha256=hashlib.sha256(catalog).hexdigest(), pages=pages, records=rows)
        assert slots
        return slots


def run(out, candidate, reference):
    assert sys.platform.startswith('linux'), 'Linux native fault injection required'
    out.mkdir(exist_ok=False); rng = random.Random(2026111605)
    def db(directory, binary):
        return Client(directory, command=[str(binary), '--data-dir', str(directory)], timeout=60)
    a = db(out / 'e860', reference); b = db(out / 'candidate', candidate)
    requests = 0; checkpoints = []; before_failed = []
    trace = (out / 'requests.jsonl').open('w')
    def both(sql, ok=True):
        nonlocal requests
        x, y = a.execute(sql, ok=ok), b.execute(sql, ok=ok); assert x == y, (sql[:100], x, y)
        requests += 1; trace.write(json.dumps(dict(sql=sql, response=x)) + '\n'); return x
    def check(tag):
        x, y = live_bytes(a.directory / 'database.mvd'), live_bytes(b.directory / 'database.mvd'); assert x == y, ('live root bytes changed', tag)
        checkpoints.append(dict(tag=tag, roots=len(x), pages=sum(len(v['pages']) for v in x.values()), records=sum(len(v['records']) for v in x.values()),
            manifest_sha256=hashlib.sha256(json.dumps(x, sort_keys=True).encode()).hexdigest(),
            physical_bytes={name: (handle.directory / 'database.mvd').stat().st_blocks * 512 for name, handle in [('e860',a),('candidate',b)]}))
    try:
        both('CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,txt VARCHAR(4096),v VECTOR(33));')
        both('CREATE TABLE u(id BIGINT PRIMARY KEY,g BIGINT);')
        both('INSERT INTO u VALUES(1,0),(2,0);')
        order = list(range(1000)); rng.shuffle(order)
        for start in range(0, 1000, 50):
            text = "'" + ('x' * (start % 31)) + "\\0\u4e2d\U0001f600'"
            values = ','.join(f"({key},{key % 7},{text},'[{','.join(str((key % 19 - 9) / 8) for _ in range(33))}]')" for key in order[start:start+50])
            both('INSERT INTO t VALUES' + values + ';')
            if start % 250 == 0: check('insert-' + str(start))
        both('CREATE VECTOR INDEX iv ON t(v);'); check('indexed')
        query = "SELECT id,L2_DISTANCE(v,'[" + ','.join(['0.125'] * 33) + "]') AS d,txt FROM t WHERE g>=2 ORDER BY d LIMIT 10;"
        for step in range(42):
            both(query)
            if step % 6 == 0:
                both('BEGIN;'); both(f'UPDATE t SET g={step} WHERE id>=0 AND id<200;')
                both('UPDATE u SET g=9;'); both('ROLLBACK;')
            elif step % 6 == 1:
                both('BEGIN;'); both(f'UPDATE t SET g={step} WHERE id>=200 AND id<400;')
                both(f'UPDATE u SET g={step};'); both('COMMIT;')
            elif step % 6 == 2:
                both(f'UPDATE t SET g={step} WHERE id>=400 AND id<600;')
            elif step % 6 == 3:
                both('BEGIN;'); both('DELETE FROM t WHERE id>=600 AND id<800;'); both('ROLLBACK;')
            elif step % 6 == 4:
                old = hashlib.sha256((b.directory / 'database.mvd').read_bytes()).hexdigest()
                both("INSERT INTO t VALUES(5000,1,'staged','[" + ','.join(['0'] * 33) + "]'),(0,1,'duplicate','[" + ','.join(['0'] * 33) + "]');", ok=False)
                after = hashlib.sha256((b.directory / 'database.mvd').read_bytes()).hexdigest(); assert old == after
                before_failed.append(dict(step=step, raw_file_unchanged=True, sha256=after))
            else:
                both('BEGIN;'); both('DELETE FROM t WHERE id>=800;'); both('UPDATE u SET g=-8;')
                for handle in (a, b): handle.restart(crash=True)
            check('step-' + str(step))
        result = dict(passed=True, requests=requests, checkpoints=checkpoints, failed_statement_bytes=before_failed,
            both_recovery_roots_live_bytes_identical=True, persistent_format_unchanged=True)
    finally:
        trace.close(); a.kill(); b.kill()
    shim = out / 'reclaim-fault.so'
    subprocess.run(['cc','-shared','-fPIC','-O2',str(ROOT/'tests/reclaim_fault.c'),'-o',str(shim),'-ldl'], check=True)
    faults = []
    for mode in ('before', 'after', 'eio', 'enosys'):
        directory = out / ('fault-' + mode); handle = db(directory, candidate)
        try:
            handle.execute('CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR(33));')
            handle.execute('INSERT INTO t VALUES' + ','.join(f"({i},0,'[{','.join(['1']*33)}]')" for i in range(500)) + ';')
            handle.shutdown()
            arm, marker, counter = [directory / name for name in ('reclaim.arm','reclaim.marker','reclaim.counter')]
            handle.env.update(LD_PRELOAD=str(shim),MVD_RECLAIM_ARM=str(arm),MVD_RECLAIM_MARKER=str(marker),MVD_RECLAIM_COUNTER=str(counter),MVD_RECLAIM_INJECT=mode)
            handle.start(); handle.execute('UPDATE t SET g=1;')
            handle.execute("SELECT id,L2_DISTANCE(v,'[" + ','.join(['0']*33) + "]') AS d FROM t ORDER BY d LIMIT 10;")
            arm.write_text('armed'); outcome = {}
            def commit():
                try: outcome['response'] = handle.execute('UPDATE t SET g=2;')
                except Exception as error: outcome['interrupted'] = str(error)
            if mode in ('before','after'):
                worker = threading.Thread(target=commit); worker.start(); until = time.monotonic()+15
                while not marker.exists() and time.monotonic()<until:
                    assert handle.process.poll() is None; time.sleep(.005)
                assert marker.exists() and marker.read_text()==mode, 'actual reclaim syscall was not reached'
                handle.kill(); worker.join(timeout=15); assert not worker.is_alive() and 'response' not in outcome
            else:
                commit(); assert outcome['response']['ok']
                handle.shutdown()
            injected = counter.stat().st_size; assert injected > 0
            for key in list(handle.env):
                if key.startswith('MVD_RECLAIM_') or key=='LD_PRELOAD':handle.env.pop(key)
            handle.start(); assert handle.execute('SELECT id,g FROM t ORDER BY id;')['rows']==[[i,2] for i in range(500)]
            handle.shutdown()
            path = directory/'database.mvd'; roots=live_bytes(path); latest=max(roots,key=lambda x:roots[x]['generation'])
            with path.open('r+b') as f:f.seek(latest*4096);f.write(b'X')
            handle.start();assert handle.execute('SELECT id,g FROM t ORDER BY id;')['rows']==[[i,1] for i in range(500)];handle.shutdown()
            old = db(directory,reference)
            try:assert old.execute('SELECT id,g FROM t ORDER BY id;')['rows']==[[i,1] for i in range(500)]
            finally:old.kill()
            faults.append(dict(mode=mode,actual_injected_calls=injected,latest_root_recovered=True,previous_root_survived=True,old_binary_read=True,acknowledged=bool(outcome.get('response'))))
            print(json.dumps(faults[-1]),flush=True)
        finally:handle.kill()
    result.update(faults=faults,binary_sha256=hashlib.sha256((candidate).read_bytes()).hexdigest(),scope='Real native dynamic ELF syscall injection and byte manifests of live data; no formal submission or score.')
    (out/'report.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k not in ('checkpoints','failed_statement_bytes')}),flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',required=True);p.add_argument('--binary',required=True);p.add_argument('--reference-bin',required=True);a=p.parse_args();run(Path(a.output),Path(a.binary).resolve(strict=True),Path(a.reference_bin).resolve(strict=True))
