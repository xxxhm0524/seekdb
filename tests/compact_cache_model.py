#!/usr/bin/env python3
"""Bounded compressed cache and spool boundary/rewind differential model."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--sanitize', choices=['address','undefined'])
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='mvd-compact-cache-model-') as temporary:
    folder = Path(temporary)
    binary = folder/('model.exe' if os.name == 'nt' else 'model')
    command = ['g++','-std=c++20','-O2','-Wall','-Wextra','-Isrc',
        'tests/compact_cache_model.cpp','-o',str(binary)]
    if args.sanitize:
        command += ['-g','-fsanitize='+args.sanitize,'-fno-sanitize-recover=all','-fno-omit-frame-pointer']
    subprocess.run(command,cwd=ROOT,check=True)
    subprocess.run([str(binary),str(folder/'spool.tmp')],cwd=ROOT,check=True)
