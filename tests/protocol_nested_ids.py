"""Protocol IDs are opaque JSON values; no 64-level limit is published.

The expected values come from Python's JSON decoder and the protocol, not
another database parser. Replays exercise success, SQL error and shutdown.
"""
import argparse
import json
from pathlib import Path
import sys
from client import Client


def nested(depth, objects=False):
    text = '18446744073709551617'
    for level in range(depth):
        text = ('{"a":' + text + ',"b":true}') if objects and level % 2 else '[' + text + ']'
    return text


def run(out):
    db = Client(out / 'data', timeout=30)
    checks = 0
    def send(identifier, sql='SELECT id FROM t;', ok=True, op='execute', metadata=None):
        nonlocal checks
        wire = '{"id":' + identifier + ',"op":' + json.dumps(op)
        if op == 'execute': wire += ',"sql":' + json.dumps(sql)
        if metadata is not None: wire += ',"extra":' + metadata
        db.process.stdin.write((wire + '}\n').encode()); db.process.stdin.flush()
        response = db.receive()
        assert response.get('id') == json.loads(identifier), ('opaque ID changed', checks, response)
        assert response['ok'] is ok, response
        checks += 1
        return response
    try:
        db.execute('CREATE TABLE t(id BIGINT PRIMARY KEY);')
        db.execute('INSERT INTO t VALUES(7);')
        for depth in [63, 64, 65, 128, 256, 512]:
            for objects in [False, True]:
                ident = nested(depth, objects)
                assert send(ident)['rows'] == [[7]]
                send(ident, sql='SELECT unknown_column FROM t;', ok=False)
                assert send('"deep-metadata"', metadata=ident)['rows'] == [[7]]
        # Exact wire comparison also checks depths beyond this test process's
        # ordinary JSON decoder limit, without recursing in the test oracle.
        for depth in [1024, 8192]:
            ident = nested(depth, True)
            raw = '{"id":' + ident + ',"op":"execute","sql":"SELECT id FROM t;"}\n'
            db.process.stdin.write(raw.encode()); db.process.stdin.flush()
            line = db.lines.get(timeout=db.timeout)
            prefix = ('{"id":' + ident + ',').encode()
            assert line is not None and line.startswith(prefix) and line.endswith(b'\n')
            suffix = json.loads(b'{' + line[len(prefix):])
            assert suffix == dict(columns=['id'], ok=True, rows=[[7]])
            checks += 1
        # A duplicate key or malformed delimiter inside a deep unknown field
        # still invalidates the whole frame before its DELETE can execute.
        for bad in ['{"x":1,"x":2}', '{"x":1,}', '[1,]', '[1}', '"\\ud800"']:
            raw = '{"id":1,"op":"execute","sql":"DELETE FROM t;","extra":' + '['*128 + bad + ']'*128 + '}\n'
            db.process.stdin.write(raw.encode()); db.process.stdin.flush()
            assert db.receive()['ok'] is False
            assert send('"after-malformed"')['rows'] == [[7]]
        send(nested(512, True), op='shutdown')
        assert db.process.wait(timeout=10) == 0
        db.process.stdin.close(); db.reader.join(timeout=2)
        assert db.lines.get(timeout=2) is None, 'extra stdout after shutdown'
        return dict(passed=True, valid_requests=checks, invalid_frames=5,
                    maximum_container_depth=8192, source='STDIO_PROTOCOL 1.5: opaque JSON id',
                    success_sql_error_shutdown_and_unknown_metadata=True,
                    malformed_deep_frames_not_executed=True)
    finally: db.kill()


if __name__ == '__main__':
    # The product parser has no artificial recursion cap. The independent
    # decoder's own process limit is raised only in this test process.
    sys.setrecursionlimit(10000)
    parser = argparse.ArgumentParser(); parser.add_argument('--output', required=True)
    args = parser.parse_args(); out = Path(args.output).resolve(); out.mkdir(parents=True, exist_ok=False)
    try: result = run(out)
    except Exception as error:
        import traceback
        result = dict(passed=False, error=str(error), traceback=traceback.format_exc())
    (out / 'report.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False), flush=True)
    raise SystemExit(0 if result['passed'] else 1)
