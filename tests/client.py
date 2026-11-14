"""Portable transport; assertions are imported from the unmodified official kit."""
import importlib.util
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
BINARY = os.environ.get("MVD_BIN", str(ROOT / ("minivectordb.exe" if os.name == "nt" else "minivectordb")))
spec = importlib.util.spec_from_file_location("official_assertions", ROOT / "protocol_check/_assertions.py")
official = importlib.util.module_from_spec(spec)
spec.loader.exec_module(official)
CONFIG = json.loads((ROOT / "protocol_check/config.json").read_text())

class Client:
    def __init__(self, directory, command=None, timeout=15, env=None):
        self.directory, self.command, self.timeout = Path(directory), command, timeout
        self.env = dict(os.environ, **(env or {}))
        self.counter = 0
        self.start()

    def start(self):
        self.directory.mkdir(parents=True, exist_ok=True)
        self.stderr = open(self.directory / "stderr.log", "ab")
        self.container = None
        command = self.command or [BINARY, "--data-dir", str(self.directory)]
        if self.command is None and self.env.get("MVD_DOCKER_IMAGE"):
            self.container = "mvd-clean-" + uuid.uuid4().hex
            command = ["docker", "run", "--rm", "-i", "--read-only", "--network=none",
                       "--name", self.container, "--cpus=2", "--memory=" + self.env.get("MVD_DOCKER_MEMORY", "2g"),
                       "--memory-swap=" + self.env.get("MVD_DOCKER_MEMORY", "2g"), "--pids-limit=128",
                       "-v", str(self.directory.resolve()) + ":/data"]
            for name in ["MVD_PAGE_CACHE_MIB", "MVD_VECTOR_CACHE_MIB"]:
                if name in self.env:
                    command += ["-e", name + "=" + self.env[name]]
            command += [self.env["MVD_DOCKER_IMAGE"]]
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.stderr, env=self.env)
        self.lines = queue.Queue()
        def reader():
            try:
                for line in self.process.stdout:
                    self.lines.put(line)
            finally:
                self.lines.put(None)
        self.reader = threading.Thread(target=reader, daemon=True)
        self.reader.start()
        assert self.receive() == {"type": "ready"}, "incorrect ready handshake"

    def receive(self):
        line = self.lines.get(timeout=self.timeout)
        if line is None:
            raise AssertionError("database exited before responding")
        return json.loads(line, parse_constant=lambda x: (_ for _ in ()).throw(ValueError(x)))

    def send(self, request):
        self.process.stdin.write((json.dumps(request, ensure_ascii=False) + "\n").encode())
        self.process.stdin.flush()
        response = self.receive()
        assert response.get("id") == request.get("id")
        assert type(response.get("ok")) is bool
        return response

    def execute(self, sql, mode="exact", ok=True):
        self.counter += 1
        response = self.send(dict(id=self.counter, op="execute", sql=sql, search_mode=mode))
        if ok is not None:
            assert response["ok"] is ok, (sql, response)
        return response

    def shutdown(self):
        if self.process.poll() is not None:
            self.stderr.close()
            return
        response = self.send(dict(id="shutdown", op="shutdown"))
        assert response == dict(id="shutdown", ok=True), response
        self.process.stdin.close()
        assert self.process.wait(timeout=self.timeout) == 0
        self.reader.join(timeout=2)
        assert self.lines.get(timeout=2) is None, "extra stdout after shutdown"
        self.stderr.close()

    def kill(self):
        if self.container and self.process.poll() is None:
            subprocess.run(["docker", "kill", "--signal=KILL", self.container],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=10)
        self.reader.join(timeout=2)
        self.process.stdin.close()
        self.process.stdout.close()
        self.stderr.close()
        if self.container:
            subprocess.run(["docker", "rm", "-f", self.container],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)

    def restart(self, crash=False):
        self.kill() if crash else self.shutdown()
        self.start()

def public_cases(output, command=None):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((ROOT / "mysqltest/manifest.json").read_text())
    report = []
    for case in manifest["cases"]:
        start, db = time.monotonic(), None
        row = {"id": case["id"], "passed": False}
        try:
            db = Client(output / case["id"], command=command)
            expected = json.loads((ROOT / "mysqltest" / case["result"]).read_text(encoding="utf-8"))
            count, mode = 0, "exact"
            for line in (ROOT / "mysqltest" / case["test"]).read_text(encoding="utf-8").splitlines():
                sql = line.strip()
                if not sql or sql.startswith("#"):
                    continue
                if sql == "--restart":
                    db.restart()
                elif sql.startswith("--mode "):
                    mode = sql.split()[1]
                else:
                    result = db.execute(sql, mode, ok=None)
                    official.assert_response(result, expected[count], CONFIG)
                    count += 1
            assert count == len(expected)
            db.shutdown()
            row["passed"] = True
        except Exception as error:
            row["error"] = str(error)
        finally:
            if db:
                db.kill()
        row["seconds"] = time.monotonic() - start
        report.append(row)
        print(json.dumps(row, ensure_ascii=False), flush=True)
    result = {"passed": all(x["passed"] for x in report), "cases": report,
              "scope": "official assertions with portable local transport; not an official score"}
    (output / "report.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    return result["passed"]

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--example", action="store_true")
    args = parser.parse_args()
    import sys
    sys.stdout.reconfigure(encoding="utf-8")
    command = [sys.executable, str(ROOT / "protocol_check/example_stdio.py")] if args.example else None
    sys.exit(0 if public_cases(args.output, command) else 1)
