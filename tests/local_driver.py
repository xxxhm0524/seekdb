#!/usr/bin/env python3
"""Local driver for the public MiniVectorDB package (JSONL judge protocol)."""
import argparse
import json
import math
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from protocol_client import Client, DEFAULT_BINARY  # noqa: E402


def close_enough(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        if isinstance(a, int) and isinstance(b, int):
            return a == b
        return math.isclose(float(a), float(b), rel_tol=1e-5, abs_tol=1e-5)
    return type(a) is type(b) and a == b


def row_equal(a, b):
    return len(a) == len(b) and all(close_enough(x, y) for x, y in zip(a, b))


def compare_case(expected, response):
    if "error" in expected:
        if response.get("ok") is not False:
            return f"expected error {expected['error']!r}, got {response}"
        code = response.get("error", {}).get("code")
        if not isinstance(code, str) or not code:
            return f"missing error.code: {response}"
        if code != expected["error"]:
            return f"error: expected {expected['error']!r}, got {code!r}"
        return None
    if response.get("ok") is not True:
        return f"unexpected error: {response}"
    if "affected_rows" in expected:
        if response.get("affected_rows") != expected["affected_rows"]:
            return (f"affected_rows: expected {expected['affected_rows']}, "
                    f"got {response.get('affected_rows')}")
    if "columns" in expected:
        if response.get("columns") != expected["columns"]:
            return f"columns: expected {expected['columns']}, got {response.get('columns')}"
        erows = expected.get("rows", [])
        arows = response.get("rows", [])
        if expected.get("unordered"):
            if len(erows) != len(arows):
                return f"row count: expected {len(erows)}, got {len(arows)}"
            remaining = list(arows)
            for er in erows:
                index = next((i for i, ar in enumerate(remaining) if row_equal(er, ar)), None)
                if index is None:
                    return f"row not found: {er}; actual={arows}"
                remaining.pop(index)
            if remaining:
                return f"unexpected rows: {remaining}"
        else:
            if len(erows) != len(arows):
                return f"row count: expected {len(erows)}, got {len(arows)}"
            for i, (er, ar) in enumerate(zip(erows, arows)):
                if not row_equal(er, ar):
                    return f"row {i}: expected {er}, got {ar}"
    return None


def load_manifest(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def run_case(binary, case_name, verbose, package=None):
    package = package or os.path.join(ROOT, "mysqltest")
    manifest = load_manifest(os.path.join(package, "manifest.json"))
    entry = next(c for c in manifest["cases"] if c["id"] == case_name)
    test_path = os.path.join(package, entry["test"])
    result_path = os.path.join(package, entry["result"])
    with open(test_path, encoding="utf-8") as f:
        lines = [ln.strip() for ln in f if ln.strip()]
    with open(result_path, encoding="utf-8") as f:
        expected = json.load(f)
    if len(expected) != entry["statements"]:
        return [f"case {case_name}: manifest statements {entry['statements']} vs {len(expected)} expectations"]

    base = tempfile.mkdtemp(prefix="mvd_")
    datadir = os.path.join(base, "data")
    os.makedirs(datadir)
    client = Client(binary, datadir)
    failures = []
    idx, mode = 0, "exact"
    try:
        for line_number, line in enumerate(lines, 1):
            if line.startswith("#") or not line:
                continue
            if line == "--restart":
                client.shutdown()
                client = Client(binary, datadir)
                continue
            if line in ("--mode exact", "--mode ann"):
                mode = line.split()[1]
                continue
            if line.startswith("--"):
                failures.append(f"{case_name}: unsupported directive {line!r}")
                break
            if idx >= len(expected):
                failures.append(f"{case_name}: extra statement {line!r}")
                break
            try:
                response = client.request(line, mode)
            except Exception as exc:
                failures.append(f"{case_name}[{idx}] {line}: {exc}")
                break
            error = compare_case(expected[idx], response)
            if error:
                failures.append(f"{case_name}[{idx}] line {line_number} {line}: {error}")
            elif verbose:
                print(f"  ok {idx}: {line}")
            idx += 1
        if idx != len(expected):
            failures.append(f"{case_name}: executed {idx} statements, expected {len(expected)}")
        client.shutdown()
    finally:
        try:
            client.close()
        except Exception:
            pass
        if verbose and failures:
            print(f"  data dir kept: {datadir}")
        else:
            shutil.rmtree(base, ignore_errors=True)
    return failures


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=DEFAULT_BINARY)
    parser.add_argument("--case", default="")
    parser.add_argument("--package", default=os.path.join(ROOT, "mysqltest"))
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)
    manifest = load_manifest(os.path.join(args.package, "manifest.json"))
    cases = [args.case] if args.case else [c["id"] for c in manifest["cases"]]
    all_failures = []
    for case_name in cases:
        print(f"=== {case_name} ===")
        failures = run_case(args.binary, case_name, args.verbose, args.package)
        all_failures.extend(failures)
        for failure in failures:
            print("FAIL:", failure)
        if not failures:
            print("PASS")
    if all_failures:
        print(f"\n{len(all_failures)} failure(s)")
        return 1
    print("\nall cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
