#!/usr/bin/env python3
"""Round-trip arbitrary page bytes and reject malformed cache packets."""
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
    with tempfile.TemporaryDirectory(prefix='mvd-page-codec-') as temp:
        binary = Path(temp)/('model.exe' if os.name == 'nt' else 'model')
        command = ['g++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-I'+str(ROOT/'src'),
            str(ROOT/'tests/page_image_codec_model.cpp'), '-o', str(binary)]
        if args.sanitize:
            command += ['-fsanitize='+args.sanitize, '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
