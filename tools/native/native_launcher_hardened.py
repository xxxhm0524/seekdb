"""Exact candidate entrypoint with private /data, read-only root and no network."""
import json
import os
from pathlib import Path
import sys

workspace=Path(__file__).resolve().parent
assert len(sys.argv)==2
data=Path(sys.argv[1]).resolve(strict=True)
assert data.parent==(workspace/'case-tmp').resolve(strict=True) and data.name.startswith('vectordb-')
os.chown(data,65534,65534)
settings=json.loads((workspace/'native-settings.json').read_text())
base_cgroup=Path(settings['cgroup']).resolve(strict=True)
run_cgroup=Path(os.environ.get('MVD_NATIVE_CGROUP',str(base_cgroup/'shared'))).resolve(strict=True)
assert run_cgroup.parent==base_cgroup
(run_cgroup/'cgroup.procs').write_text(str(os.getpid()))
program=Path(os.environ.get('MVD_NATIVE_BINARY',str(workspace/'build/minivectordb'))).resolve(strict=True)
assert program.parent==(workspace/'build').resolve(strict=True)
script='mount --bind "$1" /data; mount -o remount,bind,ro /; exec /usr/bin/setpriv --reuid=65534 --regid=65534 --clear-groups --no-new-privs "$2"'
os.execv('/usr/bin/unshare',['unshare','--mount','--net','--propagation','private','/bin/sh','-ceu',script,'mvd-native',str(data),str(program)])
