#!/usr/bin/env python3
"""Check actual WAL reads, offset reuse, append visibility and incomplete tails."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', choices=['address', 'undefined'])
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mvd-wal-read-') as temp:
        binary = Path(temp) / ('model.exe' if os.name == 'nt' else 'model')
        command = ['g++', '-std=c++20', '-O1', '-Wall', '-Wextra', '-fopenmp',
            '-I'+str(ROOT/'src'), str(ROOT/'tests/wal_read_model.cpp'),
            str(ROOT/'src/engine.cpp'), str(ROOT/'src/sql.cpp'), '-o', str(binary)]
        if args.sanitize:
            command += ['-fsanitize='+args.sanitize, '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
        subprocess.run(command, check=True)
        subprocess.run([str(binary), str(Path(temp)/'wal.log')], check=True)


if __name__ == '__main__':
    main()
