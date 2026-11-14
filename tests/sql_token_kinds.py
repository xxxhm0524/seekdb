"""Quoted strings/identifiers must never become SQL operators or keywords."""
import argparse
import json
from pathlib import Path
from client import Client


def run(out):
    db = Client(out / 'data'); rejected = 0; positive = 0
    expected = [[1, 10], [2, 20], [3, 30]]
    try:
        db.execute('CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT);')
        db.execute('INSERT INTO t VALUES(1,10),(2,20),(3,30);')
        for transaction in [False, True]:
            if transaction:
                db.execute('BEGIN;'); db.execute('UPDATE t SET n=11 WHERE id=1;')
                expected = [[1, 11], [2, 20], [3, 30]]
            for op in ['=', '<', '<=', '>', '>=', '!=', '<>']:
                assert db.execute(f'SELECT id FROM t WHERE n {op} 20;')['ok']; positive += 1
                for quote in ["'", '"', '`']:
                    token = quote + op + quote
                    for sql in [f'SELECT id FROM t WHERE n {token} 20;',
                                f'UPDATE t SET n=99 WHERE n {token} 20;',
                                f'DELETE FROM t WHERE n {token} 20;']:
                        db.execute(sql, ok=False); rejected += 1
                        assert db.execute('SELECT id,n FROM t ORDER BY id;')['rows'] == expected
            for sql in ['SELECT id FROM t "WHERE" n=20;', 'SELECT id FROM t WHERE n=20 "AND" id=2;',
                        'SELECT id FROM t "ORDER" BY id;', 'SELECT id FROM t ORDER "BY" id;',
                        'SELECT id FROM t "LIMIT" 1;', 'UPDATE t SET n "=" 99;',
                        'SELECT id FROM t LIMIT "1";']:
                db.execute(sql, ok=False); rejected += 1
                assert db.execute('SELECT id,n FROM t ORDER BY id;')['rows'] == expected
            if transaction: db.execute('COMMIT;')
        db.restart(crash=True)
        assert db.execute('SELECT id,n FROM t ORDER BY id;')['rows'] == expected
        return dict(passed=True, invalid_statements_rejected=rejected, positive_comparisons=positive,
                    no_side_effects=True, previous_transaction_statement_commits=True, restart=True)
    finally: db.kill()


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--output', required=True); args = p.parse_args()
    out = Path(args.output).resolve(); out.mkdir(parents=True, exist_ok=False)
    try: result = run(out)
    except Exception as error:
        import traceback
        result = dict(passed=False, error=str(error), traceback=traceback.format_exc())
    (out / 'report.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result), flush=True)
    raise SystemExit(0 if result['passed'] else 1)
