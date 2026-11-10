"""Bounded JSON-lines transport. Docker is the default submission boundary."""
import json
import os
import queue
import selectors
import signal
import subprocess
import tempfile
import threading
import time
import uuid
from pathlib import Path
def temporary_root():
    return tempfile.gettempdir()


class ProtocolError(RuntimeError):
    pass


def strict_json(text):
    def invalid(value):
        raise ValueError(f"non-finite JSON number: {value}")
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError(f"duplicate JSON key: {key}")
            result[key] = value
        return result
    return json.loads(text, parse_constant=invalid, object_pairs_hook=pairs)


class Session:
    def __init__(self, config, log, image=None, command=None, case_timeout=None, data_root=None, job_token=None):
        self.config, self.image, self.command = config, image, command
        self.name = "vectordb-judge-" + uuid.uuid4().hex
        self.volume = self.name + "-data"
        self.temp = tempfile.TemporaryDirectory(prefix="vectordb-", dir=temporary_root())
        self.log = open(log, "ab", buffering=0)
        self.proc = None
        self.sequence = 0
        self.buffer = b""
        self.selector = None
        self.reader = None
        self.chunks = None
        self.deadline = time.monotonic() + (case_timeout or config["case_timeout"])
        self.volume_created = False
        self.job_token = job_token
        self.bind_data = (tempfile.TemporaryDirectory(prefix="vectordb-data-", dir=data_root)
                          if data_root is not None else None)

    def docker(self, *args, check=True):
        return subprocess.run(["docker", *args], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=30, check=check)

    def start(self, ready=True):
        if self.image:
            if not self.volume_created and self.bind_data is None:
                self.docker("volume", "create", self.volume)
                self.volume_created = True
            argv = ["docker", "run", "--name", self.name, "--interactive",
                    "--network=none", "--cpus=" + self.config["cpus"],
                    "--memory=" + self.config["memory"],
                    "--memory-swap=" + self.config["memory"],
                    "--pids-limit=" + str(self.config["pids_limit"]),
                    "--cap-drop=ALL", "--security-opt=no-new-privileges",
                    "--read-only", "--mount", (f"type=bind,src={self.bind_data.name},dst=/data"
                        if self.bind_data else f"type=volume,src={self.volume},dst=/data"),
                    self.image]
            if self.job_token:
                argv[-1:-1] = ["--label", "org.ob-game-test.job=" + self.job_token]
        else:
            argv = [part.replace("{data_dir}", self.temp.name) for part in self.command]
        env = dict(os.environ, VECTORDB_DATA_DIR=self.temp.name)
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.log, start_new_session=True, env=env)
        if os.name == 'nt':
            # Windows selectors accept sockets, not subprocess pipes. Keep
            # the same deadlines/byte limits with a bounded reader queue.
            self.chunks = queue.Queue(maxsize=16)
            chunks, stdout = self.chunks, self.proc.stdout
            def pump():
                try:
                    while True:
                        chunk = os.read(stdout.fileno(), 65536)
                        chunks.put(chunk)
                        if not chunk:
                            break
                except (OSError, ValueError):
                    chunks.put(b'')
            self.reader = threading.Thread(target=pump, daemon=True)
            self.reader.start()
        else:
            os.set_blocking(self.proc.stdin.fileno(), False)
            self.selector = selectors.DefaultSelector()
            self.selector.register(self.proc.stdout, selectors.EVENT_READ)
        self.buffer = b""
        if ready:
            message = self.read(self.config["startup_timeout"])
            if message != {"type": "ready"}:
                raise ProtocolError(f"expected ready, got {message!r}")

    def remaining(self, timeout):
        value = min(timeout, self.deadline - time.monotonic())
        if value <= 0:
            raise TimeoutError("case deadline exceeded")
        return value

    def read(self, timeout):
        deadline = time.monotonic() + self.remaining(timeout)
        while b"\n" not in self.buffer:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("JSON response timeout")
            if self.chunks is not None:
                try:
                    chunk = self.chunks.get(timeout=left)
                except queue.Empty:
                    raise TimeoutError("JSON response timeout") from None
            else:
                if not self.selector.select(left):
                    raise TimeoutError("JSON response timeout")
                chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                raise ProtocolError("database exited before response")
            self.buffer += chunk
            if len(self.buffer) > self.config["max_response_bytes"]:
                raise ProtocolError("response exceeds byte limit")
        line, self.buffer = self.buffer.split(b"\n", 1)
        try:
            value = strict_json(line.decode("utf-8"))
        except (ValueError, UnicodeError) as exc:
            raise ProtocolError(f"invalid JSON response: {exc}; stdout line (first 240 bytes)={line[:240]!r}") from exc
        if not isinstance(value, dict):
            raise ProtocolError("response must be an object")
        return value

    def send(self, request):
        pending = (not self.chunks.empty() if self.chunks is not None
                   else self.selector.select(0))
        if self.buffer or pending:
            raise ProtocolError("unsolicited or duplicate response")
        data = memoryview((json.dumps(request, ensure_ascii=False, allow_nan=False) + "\n").encode())
        deadline = time.monotonic() + self.remaining(self.config["request_timeout"])
        if self.chunks is not None:
            done = queue.Queue(maxsize=1)
            stdin = self.proc.stdin
            def write_request():
                try:
                    stdin.write(data)
                    stdin.flush()
                    done.put(None)
                except (OSError, ValueError) as exc:
                    done.put(exc)
            threading.Thread(target=write_request, daemon=True).start()
            try:
                error = done.get(timeout=max(0, deadline-time.monotonic()))
            except queue.Empty:
                raise TimeoutError("request write timeout") from None
            if error is not None:
                raise ProtocolError("request write failed: " + str(error)) from error
            return
        with selectors.DefaultSelector() as writer:
            writer.register(self.proc.stdin, selectors.EVENT_WRITE)
            while data:
                left = deadline - time.monotonic()
                if left <= 0 or not writer.select(left):
                    raise TimeoutError("request write timeout")
                data = data[os.write(self.proc.stdin.fileno(), data):]

    def request(self, sql=None, mode="exact", op="execute", wait=True, ident=None):
        self.sequence += 1
        request = {"id": self.sequence if ident is None else ident, "op": op}
        if sql is not None:
            request.update(sql=sql, search_mode=mode)
        self.send(request)
        if not wait:
            return
        response = self.read(self.config["request_timeout"])
        if type(response.get("id")) is not type(request["id"]) or response["id"] != request["id"]:
            raise ProtocolError("response id differs from request")
        if type(response.get("ok")) is not bool:
            raise ProtocolError("missing boolean ok")
        return response

    def shutdown(self):
        response = self.request(op="shutdown")
        if response["ok"] is not True:
            raise ProtocolError("shutdown failed")
        self.proc.stdin.close()
        self.proc.wait(timeout=self.remaining(self.config["request_timeout"]))
        extra = self.buffer
        if self.chunks is None:
            extra += self.proc.stdout.read(self.config["max_response_bytes"])
        else:
            while True:
                try:
                    chunk = self.chunks.get(timeout=self.remaining(self.config['request_timeout']))
                except queue.Empty:
                    raise TimeoutError('stdout did not close after shutdown') from None
                if not chunk:
                    break
                extra += chunk
                if len(extra) > self.config['max_response_bytes']:
                    raise ProtocolError('response exceeds byte limit')
        if extra or self.proc.returncode:
            raise ProtocolError("extra stdout or nonzero exit after shutdown")
        self.stop()

    def stop(self):
        if self.image and self.proc is not None:
            # Killing the Docker CLI alone would leave the database alive.
            self.docker("rm", "--force", "--volumes", self.name, check=False)
        if self.proc is not None:
            if self.proc.poll() is None:
                if os.name == 'nt':
                    self.proc.kill()
                else:
                    os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait(timeout=10)
            if self.reader:
                # Drain before joining: an offending process may have filled
                # the bounded queue with unsolicited output.
                until = time.monotonic() + 10
                while self.reader.is_alive() and time.monotonic() < until:
                    try:
                        self.chunks.get(timeout=0.05)
                    except queue.Empty:
                        pass
                    self.reader.join(timeout=0.01)
                self.reader = None
            for stream in (self.proc.stdin, self.proc.stdout):
                if not stream.closed:
                    stream.close()
            self.proc = None
        self.chunks = None
        if self.selector:
            self.selector.close()
            self.selector = None

    def crash(self):
        if self.image:
            self.docker("kill", "--signal=KILL", self.name)
        self.stop()

    def close(self):
        try:
            self.stop()
        finally:
            try:
                if self.volume_created:
                    self.docker("volume", "rm", self.volume)
            finally:
                self.log.close()
                self.temp.cleanup()
                if self.bind_data:
                    self.bind_data.cleanup()
