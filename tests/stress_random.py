#!/usr/bin/env python3
"""Randomized model check: filtered/unfiltered TopK, updates, deletes,
restarts and SIGKILL recovery, in exact and ann modes."""
import argparse
import math
import os
import random
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from protocol_client import Client, DEFAULT_BINARY  # noqa: E402


def fmt(vec):
    return "[" + ",".join("%.7g" % x for x in vec) + "]"


def dist2(a, b):
    return sum((float(x) - float(y)) ** 2 for x, y in zip(a, b))


def run(rounds, seed, binary):
    rng = random.Random(seed)
    failures = []

    def vector():
        return [float(rng.randint(0, 4)) for _ in range(8)]

    with tempfile.TemporaryDirectory(prefix="mvd-stress-") as datadir:
        client = Client(binary, datadir)
        client.request("CREATE TABLE v (id BIGINT PRIMARY KEY, cat VARCHAR(4), emb VECTOR(8));")
        client.request("CREATE VECTOR INDEX vi ON v(emb);")
        model = {}

        def restart(crash=False):
            nonlocal client
            if crash:
                client.kill()
            else:
                client.shutdown()
            client = Client(binary, datadir)

        for round_no in range(rounds):
            values = []
            seen = set()
            for _ in range(25):
                ident = rng.randrange(400)
                if ident in model or ident in seen:
                    continue
                seen.add(ident)
                vec = vector()
                cat = "c%d" % rng.randrange(3)
                values.append("(%d,'%s','%s')" % (ident, cat, fmt(vec)))
                model[ident] = (cat, vec)
            if values:
                response = client.request("INSERT INTO v VALUES " + ",".join(values) + ";")
                if response.get("affected_rows") != len(values):
                    failures.append(f"round {round_no}: insert affected {response}")
            for _ in range(5):
                ident = rng.randrange(400)
                vec = vector()
                response = client.request(
                    "UPDATE v SET emb = '%s' WHERE id = %d;" % (fmt(vec), ident))
                expected = 1 if ident in model else 0
                if response.get("affected_rows") != expected:
                    failures.append(f"round {round_no}: update {ident} affected {response}, expected {expected}")
                if ident in model:
                    model[ident] = (model[ident][0], vec)
            del_id = rng.randrange(400)
            response = client.request(
                "DELETE FROM v WHERE id < %d AND id > %d;" % (del_id + 3, del_id))
            removed = [i for i in model if del_id < i < del_id + 3]
            if response.get("affected_rows") != len(removed):
                failures.append(f"round {round_no}: delete affected {response}, expected {len(removed)}")
            for ident in removed:
                del model[ident]
            if round_no % 7 == 3:
                restart(False)
                for mode in ("exact", "ann"):
                    client.request("SET vector_mode = %s;" % mode)
                    check_queries(client, model, rng, failures, round_no, mode)
            if round_no % 11 == 5:
                restart(True)
                for mode in ("exact", "ann"):
                    client.request("SET vector_mode = %s;" % mode)
                    check_queries(client, model, rng, failures, round_no, mode)
        client.shutdown()
    return failures


def check_queries(client, model, rng, failures, round_no, mode):
    for filter_cat in ("", "c1"):
        q = [float(rng.randint(0, 4)) for _ in range(8)]
        hits = []
        for ident, (cat, vec) in model.items():
            if filter_cat and cat != filter_cat:
                continue
            hits.append((math.sqrt(dist2(vec, q)), ident))
        hits.sort(key=lambda item: (item[0], item[1]))
        hits = hits[:10]
        where = (" WHERE cat = '%s'" % filter_cat) if filter_cat else ""
        sql = ("SELECT id, L2_DISTANCE(emb, '%s') AS dist FROM v%s "
               "ORDER BY dist LIMIT 10;" % (fmt(q), where))
        response = client.request(sql, mode)
        got = [(row[0], row[1]) for row in response.get("rows", [])]
        if len(got) != len(hits):
            failures.append(f"round {round_no} {mode} filter={filter_cat!r}: "
                            f"row count {len(got)} != {len(hits)}: {got}")
            continue
        for (gid, gd), (wd, wid) in zip(got, hits):
            if gid != wid or abs(float(gd) - wd) > 1e-4:
                failures.append(f"round {round_no} {mode} filter={filter_cat!r}: "
                                f"{got} != {[(i, round(d, 4)) for d, i in hits]}")
                break
    ids = [row[0] for row in client.request("SELECT id FROM v;", mode).get("rows", [])]
    if len(ids) != len(set(ids)):
        failures.append(f"round {round_no}: duplicate ids in full scan")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=40)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--binary", default=DEFAULT_BINARY)
    args = parser.parse_args()
    failures = run(args.rounds, args.seed, args.binary)
    if failures:
        for failure in failures[:20]:
            print("FAIL:", failure)
        print(f"{len(failures)} failure(s)")
        return 1
    print(f"randomized model check: PASS ({args.rounds} rounds, seed {args.seed})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
