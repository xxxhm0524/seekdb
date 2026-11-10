#!/usr/bin/env python3
"""Compile and run the deterministic float32 conversion differential model."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent

with tempfile.TemporaryDirectory(prefix='mvd-float-model-') as temp:
    binary = Path(temp)/('float-model.exe' if os.name == 'nt' else 'float-model')
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-O2', '-I'+str(ROOT/'src'),
        str(ROOT/'tests/float_parse_model.cpp'), '-o', str(binary)], check=True, timeout=60)
    subprocess.run([str(binary)], check=True, timeout=30)
