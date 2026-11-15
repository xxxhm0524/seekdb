"""Invoke the frozen official driver and assertions without modifying them."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile

root = Path(__file__).resolve().parent
p = argparse.ArgumentParser(); p.add_argument('--binary', required=True); p.add_argument('--output', required=True); a = p.parse_args()
sys.path.insert(0, str(root / 'official-kit/protocol_check'))
tempfile.tempdir = str(root / 'case-tmp')
os.environ['MVD_NATIVE_BINARY'] = str((root / a.binary).resolve(strict=True))
spec = importlib.util.spec_from_file_location('unchanged_official_driver', root / 'official-kit/protocol_check/run.py')
driver = importlib.util.module_from_spec(spec); spec.loader.exec_module(driver)
args = ['--command', json.dumps([sys.executable, str(root / 'native_launcher_hardened.py'), '{data_dir}']),
    '--package', str(root / 'official-kit/mysqltest'), '--output', str(root / a.output)]
status = driver.main(args)
metadata = dict(official_commit='f4d4e26653af823a7601668802f80e374ad6643c', binary_sha256=hashlib.sha256(Path(os.environ['MVD_NATIVE_BINARY']).read_bytes()).hexdigest(),
    native_default_entrypoint=True, docker_used=False, argv=args, driver_returncode=status,
    inputs={str(f.relative_to(root / 'official-kit')): hashlib.sha256(f.read_bytes()).hexdigest() for folder in ['protocol_check', 'mysqltest'] for f in sorted((root / 'official-kit' / folder).rglob('*')) if f.is_file() and '__pycache__' not in f.parts})
(root / a.output / 'provenance.json').write_text(json.dumps(metadata, indent=2) + '\n')
raise SystemExit(status)
