#!/usr/bin/env python3
"""Check TopK selection against precise-value fixtures, including ULP edges."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--sanitize', choices=['undefined', 'address'])
args = parser.parse_args()
flags = ['-std=c++20', '-O2', '-fopenmp']
if args.sanitize:
    flags += ['-g', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer',
              '-fsanitize=' + ('address,undefined' if args.sanitize == 'address' else 'undefined')]
with tempfile.TemporaryDirectory(prefix='mvd-heap-model-') as temp:
    binary = Path(temp) / ('heap-model.exe' if os.name == 'nt' else 'heap-model')
    subprocess.run([os.environ.get('CXX', 'g++'), *flags,
        str(ROOT/'tests/heap_rejection_model.cpp'), str(ROOT/'src/sql.cpp'), '-o', str(binary)],
        check=True, timeout=120)
    subprocess.run([str(binary)], check=True, timeout=30)
