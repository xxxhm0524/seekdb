#!/usr/bin/env python3
"""Check actual WAL reads, offset reuse, append visibility and incomplete tails."""
import argparse
import os
from pathlib import Path
from build_support import engine_objects
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', choices=['address', 'undefined'])
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mvd-wal-read-') as temp:
        binary = Path(temp) / ('model.exe' if os.name == 'nt' else 'model')
        flags=['-std=c++20','-O1' if args.sanitize else '-O2','-fopenmp']
        if args.sanitize:flags+=['-fsanitize='+args.sanitize,'-fno-omit-frame-pointer','-fno-sanitize-recover=all']
        command=['g++',*flags,'-Wall','-Wextra','-I'+str(ROOT/'src'),str(ROOT/'tests/wal_read_model.cpp'),*engine_objects(ROOT,flags),'-o',str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary), str(Path(temp)/'wal.log')], check=True)


if __name__ == '__main__':
    main()
