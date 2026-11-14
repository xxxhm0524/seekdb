"""Bulk quoted spans must preserve every escape, Unicode byte and delimiter."""
import argparse
import json
from pathlib import Path
import random
import sys
from client import Client


def quote(text):
    return "'" + text.replace('\\', '\\\\').replace("'", "''") + "'"


def run(directory):
    rng = random.Random(880134)
    alphabet = list("abc012'\\\"`[]/*,-\t\n\r\x00\b") + ['中', '🙂', 'é', '\u0301']
    texts = ['', "'", '\\', "a'\\'b", '\\' * 100, "'" * 100, 'body' * 20000]
    texts += [''.join(rng.choice(alphabet) for _ in range(rng.randrange(0, 200))) for _ in range(512)]
    manual = [(r"'a\nb'", 'a\nb'), (r"'a\rb'", 'a\rb'), (r"'a\tb'", 'a\tb'),
              (r"'a\0b'", 'a\x00b'), (r"'a\bb'", 'a\bb'), (r"'a\zb'", 'azb'),
              (r"'a\'b'", "a'b"), ("'a''b'", "a'b"), (r"'a\\b'", 'a\\b')]
    db = Client(directory, timeout=90)
    try:
        db.execute('CREATE TABLE `t``x`("i""d" BIGINT PRIMARY KEY,body VARCHAR(100000),v VECTOR(3));')
        for base in range(0, len(texts), 50):
            values = [f"({i},{quote(texts[i])},'[0.125,-0.25,0]')" for i in range(base, min(base + 50, len(texts)))]
            db.execute('INSERT INTO `t``x` VALUES' + ','.join(values) + ';')
        for literal, expected in manual:
            key = len(texts); texts.append(expected)
            db.execute(f"INSERT INTO `t``x` VALUES({key},{literal},'[0.125,-0.25,0]');")
        expected = [[i, text, [0.125, -0.25, 0.0]] for i, text in enumerate(texts)]
        assert db.execute('SELECT * FROM `t``x` ORDER BY "i""d";')['rows'] == expected
        for literal in ["'unterminated", "'trailing\\", "'embedded\\'", '`missing', '"missing']:
            db.execute(f"INSERT INTO `t``x` VALUES(99999,{literal},'[0,0,0]');", ok=False)
        db.execute('BEGIN;'); db.execute("UPDATE `t``x` SET body='pending\\nrow' WHERE \"i\"\"d\"=0;")
        db.execute('ROLLBACK;'); db.restart(crash=True)
        assert db.execute('SELECT * FROM `t``x` ORDER BY "i""d";')['rows'] == expected
        return dict(passed=True, roundtripped_literals=len(texts), malformed_rejected=5, large_literal_bytes=80000,
                    unicode_and_nul=True, quoted_identifiers=True, rollback_and_restart=True)
    finally:
        db.kill()


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    p=argparse.ArgumentParser();p.add_argument('--output',required=True);args=p.parse_args()
    out=Path(args.output);out.mkdir(parents=True,exist_ok=False)
    try:result=run(out/'data')
    except Exception as error:
        import traceback
        result=dict(passed=False,error=str(error),traceback=traceback.format_exc())
    (out/'report.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result),flush=True)
    raise SystemExit(0 if result['passed'] else 1)
