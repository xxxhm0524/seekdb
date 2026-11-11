#!/usr/bin/env python3
"""Run zero-extent alignment and real Linux overwrite/fallback checks."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', choices=['address', 'undefined'])
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mvd-sparse-model-') as temp:
        binary = Path(temp) / ('model.exe' if os.name == 'nt' else 'model')
        command = ['g++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-I'+str(ROOT/'src'),
                   str(ROOT/'tests/sparse_io_model.cpp'), '-o', str(binary)]
        if sys.platform.startswith('linux'):
            command += ['-Wl,--wrap=fallocate']
        if args.sanitize:
            command += ['-fsanitize='+args.sanitize, '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
        subprocess.run(command, check=True)
        subprocess.run([str(binary), str(Path(temp)/'model.dat')], check=True)
        if sys.platform.startswith('linux'):
            subprocess.run([str(binary), str(Path(temp)/'model.dat'), '--deny-punch'], check=True)


if __name__ == '__main__':
    main()
