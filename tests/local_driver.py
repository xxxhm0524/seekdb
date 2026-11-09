#!/usr/bin/env python3
"""Local MiniVectorDB test driver for the public round2 samples (JSON protocol)."""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BINARY = os.path.join(os.path.dirname(HERE), "minivectordb")
if not os.path.exists(BINARY):
    BINARY = os.path.join(os.path.dirname(HERE), "minivectordb.exe")


def close_enough(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        if isinstance(a, int) and isinstance(b, int):
            return a == b
        return math.isclose(float(a), float(b), rel_tol=1e-5, abs_tol=1e-5)
    return a == b


def row_equal(a, b):
    if len(a) != len(b):
        return False
    return all(close_enough(x, y) for x, y in zip(a, b))


def compare_case(expected, actual):
    if "error" in actual:
        return f"unexpected error: {actual}"
    if "affected_rows" in expected:
        if actual.get("affected_rows") != expected["affected_rows"]:
            return f"affected_rows: expected {expected['affected_rows']}, got {actual.get('affected_rows')}"
    if "columns" in expected:
        if actual.get("columns") != expected["columns"]:
            return f"columns: expected {expected['columns']}, got {actual.get('columns')}"
        erows = expected.get("rows", [])
        arows = actual.get("rows", [])
        if expected.get("unordered"):
            if len(erows) != len(arows):
                return f"row count: expected {len(erows)}, got {len(arows)}"
            used = [False] * len(arows)
            for er in erows:
                found = False
                for i, ar in enumerate(arows):
                    if not used[i] and row_equal(er, ar):
                        used[i] = True
                        found = True
                        break
                if not found:
                    return f"row not found: {er}; actual={arows}"
        else:
            if len(erows) != len(arows):
                return f"row count: expected {len(erows)}, got {len(arows)}"
            for i, (er, ar) in enumerate(zip(erows, arows)):
                if not row_equal(er, ar):
                    return f"row {i}: expected {er}, got {ar}"
    return None


class Engine:
    def __init__(self, binary, datadir, args):
        self.p = subprocess.Popen(
            [binary, "--data-dir=" + datadir, "--output=json"] + args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            bufsize=1,
        )

    def request(self, sql, timeout=None):
        if self.p.poll() is not None:
            raise RuntimeError("engine exited: " + self.p.stderr.read())
        self.p.stdin.write(sql + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("engine closed stdout; stderr=" + self.p.stderr.read())
        try:
            return json.loads(line)
        except json.JSONDecodeError as e:
            raise RuntimeError(f"bad JSON response {line!r}: {e}")

    def close(self):
        try:
            self.p.stdin.close()
        except Exception:
            pass
        try:
            self.p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()

    def kill(self):
        self.p.kill()
        self.p.wait()


def run_case(binary, case_name, verbose):
    case_dir = HERE
    test_path = os.path.join(case_dir, "cases", case_name + ".test")
    result_path = os.path.join(case_dir, "cases", case_name + ".result")
    with open(test_path, encoding="utf-8") as f:
        statements = [ln.strip() for ln in f if ln.strip()]
    with open(result_path, encoding="utf-8") as f:
        expected = json.load(f)
    if len(statements) != len(expected):
        return [f"case {case_name}: {len(statements)} statements vs {len(expected)} expectations"]
    base = tempfile.mkdtemp(prefix="mvd_")
    datadir = os.path.join(base, "data")
    os.makedirs(datadir)
    eng = Engine(binary, datadir, [])
    failures = []
    try:
        for i, (sql, exp) in enumerate(zip(statements, expected)):
            try:
                actual = eng.request(sql)
            except Exception as e:
                failures.append(f"{case_name}[{i}] {sql}: {e}")
                break
            err = compare_case(exp, actual)
            if err:
                failures.append(f"{case_name}[{i}] {sql}: {err}")
            elif verbose:
                print(f"  ok {i}: {sql}")
        eng.close()
    finally:
        if verbose and failures:
            print(f"  data dir kept: {datadir}")
        else:
            shutil.rmtree(base, ignore_errors=True)
    return failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=BINARY)
    ap.add_argument("--case", default="")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    manifest = json.load(open(os.path.join(HERE, "manifest.json"), encoding="utf-8"))
    cases = [args.case] if args.case else [c["id"] for c in manifest["cases"]]
    all_failures = []
    for c in cases:
        print(f"=== {c} ===")
        failures = run_case(args.binary, c, args.verbose)
        all_failures.extend(failures)
        for f in failures:
            print("FAIL:", f)
        if not failures:
            print("PASS")
    if all_failures:
        print(f"\n{len(all_failures)} failure(s)")
        return 1
    print("\nall cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
