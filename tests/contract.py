"""Submission materials, dependency integrity and official-kit immutability."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

def unique_object(pairs):
    result = {}
    for k, v in pairs:
        assert k not in result, "duplicate JSON key: " + k
        result[k] = v
    return result

def strict(data):
    return json.loads(data, object_pairs_hook=unique_object,
                      parse_constant=lambda x: (_ for _ in ()).throw(ValueError(x)))

def main():
    material = (ROOT/"agent/conversation.json").read_bytes()
    assert material == (ROOT/"conversation.json").read_bytes()
    assert 0 < len(material) <= 128*1024
    assert not material.startswith(b"\xef\xbb\xbf")
    data = strict(material.decode("utf-8"))
    assert isinstance(data, dict)
    assert data["synthetic"] is False
    assert len([x for x in data["conversations"] if x["role"] == "user"]) == 2
    assert data["experiments"]
    dependencies = strict((ROOT/"third_party/DEPENDENCIES.json").read_text(encoding="utf-8"))
    for name in ["json.hpp", "json.LICENSE.MIT"]:
        assert hashlib.sha256((ROOT/"third_party"/name).read_bytes()).hexdigest() == dependencies["nlohmann/json"][name+".sha256"]
    assert hashlib.sha256((ROOT/"third_party/GCC-RUNTIME-EXCEPTION-3.1.txt").read_bytes()).hexdigest() == dependencies["gcc-runtime-exception"]["GCC-RUNTIME-EXCEPTION-3.1.txt.sha256"]
    # Official files must match the frozen upstream commit, including assertions.
    origin = "f4d4e26653af823a7601668802f80e374ad6643c"
    paths = ["STDIO_PROTOCOL.md", "mysqltest", "protocol_check"]
    changed = subprocess.check_output(["git", "diff", "--name-only", origin, "--", *paths], cwd=ROOT, text=True)
    assert not changed.strip(), "official kit was modified: " + changed
    print(json.dumps(dict(passed=True, material_bytes=len(material),
                          material_sha256=hashlib.sha256(material).hexdigest(),
                          dependency_integrity=True, official_kit_unchanged=True)))

if __name__ == "__main__":
    sys.exit(main())
