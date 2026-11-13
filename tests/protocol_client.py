#!/usr/bin/env python3
"""Small JSONL protocol client used by the local test scripts."""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_BINARY = os.environ.get("MVD_BIN") or os.path.join(
    ROOT, "minivectordb.exe" if os.path.exists(os.path.join(ROOT, "minivectordb.exe"))
    else "minivectordb")


def create_fixture_client(directory, image=None, output=None):
    """Use the same checks against a native process or an actual Docker Session."""
    if not image:
        return Client(datadir=directory)
    from pathlib import Path
    sys.path.insert(0, str(Path(ROOT)/'protocol_check'))
    from _transport import Session
    config=json.loads((Path(ROOT)/'protocol_check/config.json').read_text())
    class ImageClient:
        def __init__(self):
            log=Path(output).with_suffix('.stderr.log') if output else Path(directory)/'docker.stderr.log'
            log.parent.mkdir(parents=True,exist_ok=True)
            self.session=Session(config,log,image=image);self.active=False
            try:self.start()
            except Exception:self.session.close();raise
        def start(self):self.session.start();self.active=True
        def request(self,text,mode='exact'):return self.session.request(text,mode)
        def kill(self):self.session.crash();self.active=False
        def close(self):
            try:
                if self.active:self.session.shutdown();self.active=False
            finally:self.session.close()
    return ImageClient()


class ProtocolError(RuntimeError):
    pass


class Client:
    def __init__(self, binary=None, datadir=".", extra=None):
        self.binary = binary or DEFAULT_BINARY
        self.datadir = datadir
        self.extra = list(extra or [])
        self.p = None
        self.seq = 0
        self.start()

    def start(self):
        self.p = subprocess.Popen(
            [self.binary, "--data-dir=" + self.datadir] + self.extra,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", bufsize=1)
        line = self.p.stdout.readline()
        if not line:
            raise ProtocolError("engine exited before ready: " + self.p.stderr.read())
        try:
            message = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ProtocolError("invalid ready line %r: %s" % (line, exc))
        if message != {"type": "ready"}:
            raise ProtocolError("expected ready, got %r" % (message,))
        self.seq = 0

    def request(self, sql, mode="exact"):
        if self.p.poll() is not None:
            raise ProtocolError("engine exited: " + self.p.stderr.read())
        self.seq += 1
        ident = self.seq
        request = {"id": ident, "op": "execute", "sql": sql, "search_mode": mode}
        self.p.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise ProtocolError("engine closed stdout; stderr=" + self.p.stderr.read())
        try:
            response = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ProtocolError("bad JSON response %r: %s" % (line, exc))
        if type(response.get("id")) is not type(ident) or response["id"] != ident:
            raise ProtocolError("response id differs: %r" % (response,))
        if type(response.get("ok")) is not bool:
            raise ProtocolError("missing boolean ok: %r" % (response,))
        return response

    def shutdown(self):
        if self.p is None:
            return
        if self.p.poll() is None:
            self.seq += 1
            ident = self.seq
            self.p.stdin.write(json.dumps({"id": ident, "op": "shutdown"}) + "\n")
            self.p.stdin.flush()
            line = self.p.stdout.readline()
            if not line:
                raise ProtocolError("no shutdown response")
            response = json.loads(line)
            if response != {"id": ident, "ok": True}:
                raise ProtocolError("bad shutdown response %r" % (response,))
            self.p.stdin.close()
            self.p.wait(timeout=15)
            if self.p.returncode != 0:
                raise ProtocolError("nonzero exit after shutdown: %s" % self.p.returncode)
            extra = self.p.stdout.read()
            if extra:
                raise ProtocolError("extra stdout after shutdown: %r" % extra[:200])
        for stream in (self.p.stdin, self.p.stdout, self.p.stderr):
            stream.close()
        self.p = None

    def kill(self):
        if self.p is not None:
            self.p.kill()
            self.p.wait()
            for stream in (self.p.stdin, self.p.stdout, self.p.stderr):
                stream.close()
            self.p = None

    def close(self):
        try:
            self.shutdown()
        except Exception:
            self.kill()
            raise
