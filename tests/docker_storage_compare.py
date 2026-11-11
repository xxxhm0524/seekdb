#!/usr/bin/env python3
"""Paired real >2 GiB requests, retaining reference timeouts as evidence."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference-image', required=True)
    parser.add_argument('--candidate-image', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    results = []
    # Alternate order across the independent workloads on this same runner.
    for scenario, order in [('transaction', ('reference', 'candidate')),
                            ('unicode', ('candidate', 'reference')),
                            ('update', ('reference', 'candidate'))]:
        for version in order:
            image = getattr(args, version+'_image')
            output = args.output / (scenario+'-'+version+'.json')
            command = [sys.executable, str(ROOT/'tests'/('docker_resource_boundaries.py'
                if scenario == 'update' else 'docker_transaction_spill.py')),
                '--image', image, '--output', str(output)]
            if scenario == 'unicode':
                command.append('--long-unicode')
            before = time.perf_counter()
            try:
                process = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, timeout=650)
                code, log = process.returncode, process.stdout
            except subprocess.TimeoutExpired as error:
                code, log = -1, error.stdout or b''
            output.with_suffix('.log').write_bytes(log)
            row = dict(scenario=scenario, version=version, image=image, exit_code=code,
                total_seconds=round(time.perf_counter()-before, 3), report=output.name)
            if output.exists():
                row['result'] = json.loads(output.read_text(encoding='utf-8'))
            results.append(row)
            (args.output/'summary.json').write_text(json.dumps(dict(official_score=False,
                request_timeout_seconds=60, results=results), indent=2)+'\n', encoding='utf-8')
            print(json.dumps({k:v for k,v in row.items() if k != 'result'}), flush=True)
            if code:
                print(log[-2000:].decode(errors='replace'), flush=True)
    assert all(row['exit_code'] == 0 for row in results if row['version'] == 'candidate'), \
        'A candidate storage workload failed; inspect the preserved reports'


if __name__ == '__main__':
    main()
