#!/usr/bin/env python3
"""Run the same durable benchmark through the actual constrained Docker image."""
import argparse
import json
from pathlib import Path
import sys
import uuid

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'protocol_check'))
from _transport import Session
import bench


def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--image',required=True)
    parser.add_argument('--log-dir',type=Path,default=ROOT/'protocol-results/docker-benchmark-logs')
    args,rest = parser.parse_known_args()
    sys.argv = [sys.argv[0],*rest]
    args.log_dir.mkdir(parents=True,exist_ok=True)
    config = json.loads((ROOT/'protocol_check/config.json').read_text(encoding='utf-8'))

    class DockerClient:
        def __init__(self,binary,datadir,extra=None):
            self.session = Session(config,args.log_dir/(uuid.uuid4().hex+'.stderr.log'),image=args.image)
            self.active = False
            self.closed = False
            self.start()

        def start(self):
            self.session.start()
            self.active = True

        def request(self,sql,mode='exact'):
            return self.session.request(sql,mode)

        def kill(self):
            self.session.crash()
            self.active = False

        def shutdown(self):
            if self.active:
                self.session.shutdown()
                self.active = False

        def resource_snapshot(self):
            lines = self.session.docker('exec',self.session.name,'cat',
                '/sys/fs/cgroup/memory.peak','/sys/fs/cgroup/io.stat').stdout.decode().splitlines()
            return dict(memory_peak_bytes=int(lines[0]),io_stat=lines[1:])

        def close(self):
            if not self.closed:
                try:
                    self.shutdown()
                finally:
                    self.session.close()
                    self.closed = True

    return bench.main(client_factory=DockerClient)


if __name__=='__main__':
    sys.exit(main())
