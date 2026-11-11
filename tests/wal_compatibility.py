#!/usr/bin/env python3
"""Check WAL bytes, bidirectional crash recovery, and an incomplete tail."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "protocol_check"))
from _transport import Session
from docker_score_compare import workload

HEADER = struct.Struct("<IB3xI4xQI4x")
PAGE_METADATA = struct.Struct("<III")
PAGE_SIZE = 65536


def execute(session, sql, **expected):
    response = session.request(sql)
    assert response["ok"] is True, (sql[:120], response)
    for key, value in expected.items():
        assert response.get(key) == value, (key, value, response)
    return response


def read_wal(session):
    if session.image:
        raw = session.docker("exec", session.name, "cat", "/data/wal.log").stdout
        blocks, block_size = map(int, session.docker(
            "exec", session.name, "stat", "-c", "%b %B", "/data/wal.log").stdout.split())
        return raw, blocks * block_size
    return (Path(session.temp.name) / "wal.log").read_bytes(), None


def describe_wal(raw):
    offset = pages = commits = 0
    digest = hashlib.sha256()
    while offset < len(raw):
        header = HEADER.unpack_from(raw, offset)
        magic, kind, length, txid, checksum = header
        start = offset + HEADER.size
        end = start + length
        assert magic == 0x4D564457 and txid > 0 and end <= len(raw)
        payload = raw[start:end]
        assert zlib.crc32(payload) == checksum, (offset, checksum)
        if kind == 1:
            assert length == PAGE_METADATA.size + PAGE_SIZE
            assert PAGE_METADATA.unpack_from(payload)[2] == PAGE_SIZE
            pages += 1
        else:
            assert kind == 2 and length == 0
            commits += 1
        # Ignore padding bytes in the native record header.
        digest.update(HEADER.pack(*header))
        digest.update(payload)
        offset = end
    assert offset == len(raw)
    return dict(logical_bytes=offset, page_records=pages, commits=commits,
                record_sha256=digest.hexdigest())


def recover_with(config, output, writer, reader, data):
    vectors, categories, batches, _, _ = data
    session = Session(config, output, **writer)
    try:
        session.start()
        execute(session, "CREATE TABLE items(id BIGINT PRIMARY KEY,cat VARCHAR(8),v VECTOR(128))")
        for sql, count in batches:
            execute(session, sql, affected_rows=count)
        raw, allocation = read_wal(session)
        report = describe_wal(raw)
        assert report["commits"] == len(batches)
        report["allocated_bytes"] = allocation
        session.crash()
        session.image, session.command = reader.get("image"), reader.get("command")
        session.start()
        assert not read_wal(session)[0]
        for start in range(0, len(vectors), 256):
            stop = min(start + 256, len(vectors))
            rows = execute(session, "SELECT id,cat,v FROM items WHERE id>=%d AND id<=%d ORDER BY id"
                           % (start + 1, stop))["rows"]
            assert [row[0] for row in rows] == list(range(start + 1, stop + 1))
            assert [row[1] for row in rows] == categories[start:stop].tolist()
            assert np.array_equal(np.array([row[2] for row in rows], dtype=np.float32), vectors[start:stop])
        session.shutdown()
        report["recovered_rows"] = len(vectors)
        return report
    finally:
        session.close()


def verify_shortened_tail(config, output, candidate):
    session = Session(config, output, **candidate)
    try:
        session.start()
        execute(session, "CREATE TABLE t(id BIGINT PRIMARY KEY,x BIGINT)")
        execute(session, "INSERT INTO t VALUES (1,10)", affected_rows=1)
        execute(session, "INSERT INTO t VALUES (2,20)", affected_rows=1)
        raw, _ = read_wal(session)
        assert describe_wal(raw)["commits"] == 2
        end_of_page = len(raw) - HEADER.size
        cutoff = end_of_page - 4096
        assert raw[cutoff:end_of_page] == bytes(4096)
        # This fixture models a kill before the final page's zero suffix and
        # COMMIT become visible. Data pages are still only in the cache.
        if session.image:
            assert session.docker("exec", session.name, "stat", "-c", "%s", "/data/t_1.dat").stdout.strip() == b"0"
            session.docker("exec", session.name, "truncate", "-s", str(cutoff), "/data/wal.log")
            session.crash()
        else:
            directory = Path(session.temp.name)
            assert (directory / "t_1.dat").stat().st_size == 0
            session.crash()
            with (directory / "wal.log").open("r+b") as wal:
                wal.truncate(cutoff)
        session.start()
        execute(session, "SELECT id,x FROM t ORDER BY id", rows=[[1, 10]])
        execute(session, "INSERT INTO t VALUES (3,30)", affected_rows=1)
        session.crash()
        session.start()
        execute(session, "SELECT id,x FROM t ORDER BY id", rows=[[1, 10], [3, 30]])
        session.shutdown()
    finally:
        session.close()


def recover_wide(config, output, writer, reader):
    session = Session(config, output, **writer)
    count = 511
    dense = '\U0001f600' * 2048
    expected = {i: dense if i % 3 == 0 else 'x' for i in range(1, count+1)}
    try:
        session.start()
        execute(session, 'CREATE TABLE wide(id BIGINT PRIMARY KEY,s VARCHAR(2048),v VECTOR(2))')
        for start in range(1, count+1, 100):
            stop = min(start+100, count+1)
            execute(session, 'INSERT INTO wide VALUES '+','.join("(%d,'%s','[%d,0]')"
                % (i, expected[i], i % 17) for i in range(start, stop)), affected_rows=stop-start)
        execute(session, "UPDATE wide SET s='short' WHERE id<=200", affected_rows=200)
        for i in range(1, 201):
            expected[i] = 'short'
        execute(session, 'DELETE FROM wide WHERE id>=350', affected_rows=count-349)
        expected = {i: value for i, value in expected.items() if i < 350}
        raw, allocation = read_wal(session)
        report = describe_wal(raw)
        assert report['page_records'] > 100 and report['commits'] == 8
        report['allocated_bytes'] = allocation
        session.crash()
        session.image, session.command = reader.get('image'), reader.get('command')
        session.start()
        assert not read_wal(session)[0]
        for start in range(1, count+1, 20):
            rows = execute(session, 'SELECT id,s,v FROM wide WHERE id>=%d AND id<%d ORDER BY id'
                           % (start, start+20))['rows']
            assert rows == [[i, expected[i], [i % 17, 0]] for i in range(start, min(start+20, count+1)) if i in expected]
        session.shutdown()
        report['recovered_rows'] = len(expected)
        return report
    finally:
        session.close()


def main():
    parser = argparse.ArgumentParser()
    for version in ("reference", "candidate"):
        group = parser.add_mutually_exclusive_group(required=True)
        group.add_argument("--" + version + "-image")
        group.add_argument("--" + version + "-binary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert bool(args.reference_image) == bool(args.candidate_image)
    targets = {}
    for version in ("reference", "candidate"):
        image = getattr(args, version + "_image")
        binary = getattr(args, version + "_binary")
        targets[version] = (dict(image=image) if image else
                            dict(command=[str(binary.resolve()), "--data-dir", "{data_dir}"]))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    config = json.loads((ROOT / "protocol_check/config.json").read_text())
    data = workload(20261110, 10000, 2)
    report = {}
    for writer, reader in (("reference", "candidate"), ("candidate", "reference")):
        report[writer] = recover_with(config, args.output.with_suffix("." + writer + ".stderr.log"),
                                      targets[writer], targets[reader], data)
    assert report["reference"]["record_sha256"] == report["candidate"]["record_sha256"], report
    assert report["reference"]["logical_bytes"] == report["candidate"]["logical_bytes"], report
    if args.reference_image:
        assert report["candidate"]["allocated_bytes"] < report["reference"]["allocated_bytes"], report
    wide = {}
    for writer, reader in (("reference", "candidate"), ("candidate", "reference")):
        wide[writer] = recover_wide(config, args.output.with_suffix('.wide.'+writer+'.stderr.log'),
                                    targets[writer], targets[reader])
    assert wide['reference']['record_sha256'] == wide['candidate']['record_sha256'], wide
    if args.reference_image:
        assert wide['candidate']['allocated_bytes'] < wide['reference']['allocated_bytes'], wide
    report['wide_rows'] = wide
    verify_shortened_tail(config, args.output.with_suffix(".tail.stderr.log"), targets["candidate"])
    report.update(passed=True, byte_equivalent_records=True, shortened_tail_recovery=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report), flush=True)


if __name__ == "__main__":
    main()
