#!/usr/bin/env python3
"""Sort-run merge/slicing and exact numeric-key round trips."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parent.parent
parser=argparse.ArgumentParser()
parser.add_argument('--sanitize',choices=['address','undefined'])
args=parser.parse_args()
with tempfile.TemporaryDirectory(prefix='mvd-sort-model-') as temporary:
    folder=Path(temporary)
    binary=folder/('model.exe' if os.name=='nt' else 'model')
    command=['g++','-std=c++20','-O2','-Wall','-Wextra','-Isrc','tests/external_sort_model.cpp','-o',str(binary)]
    if args.sanitize:command+=['-g','-fsanitize='+args.sanitize,'-fno-sanitize-recover=all','-fno-omit-frame-pointer']
    subprocess.run(command,cwd=ROOT,check=True)
    subprocess.run([str(binary),str(folder/'sort.tmp')],cwd=ROOT,check=True)
    assert not (folder/'sort.tmp').exists(),'query scratch file leaked'
