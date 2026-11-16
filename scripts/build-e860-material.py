"""Compatibility entry point for the current technical-guidance material."""
import runpy
from pathlib import Path
runpy.run_path(str(Path(__file__).with_name("build-guidance-material.py")),run_name="__main__")
