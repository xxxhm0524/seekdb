#!/usr/bin/env python3
"""Compatibility entry point: run every public case over the JSONL protocol."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from local_driver import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
