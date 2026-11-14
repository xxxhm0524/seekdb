"""Submission materials, dependency integrity and official-kit immutability."""
import hashlib
import json
from pathlib import Path
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
    assert len([x for x in data["conversations"] if x["role"] == "user"]) >= 2
    assert data["experiments"]
    dependencies = strict((ROOT/"third_party/DEPENDENCIES.json").read_text(encoding="utf-8"))
    for name in ["json.hpp", "json.LICENSE.MIT"]:
        assert hashlib.sha256((ROOT/"third_party"/name).read_bytes()).hexdigest() == dependencies["nlohmann/json"][name+".sha256"]
    assert hashlib.sha256((ROOT/"third_party/GCC-RUNTIME-EXCEPTION-3.1.txt").read_bytes()).hexdigest() == dependencies["gcc-runtime-exception"]["GCC-RUNTIME-EXCEPTION-3.1.txt.sha256"]
    # Official files must match the frozen upstream commit, including assertions.
    manifest = strict((ROOT/"validation/official-kit-sha256.json").read_text(encoding="utf-8"))
    assert manifest["upstream_commit"] == "f4d4e26653af823a7601668802f80e374ad6643c"
    assert len(manifest["sha256_lf"]) == 49
    for relative, expected in manifest["sha256_lf"].items():
        data = (ROOT/relative).read_bytes().replace(b"\r\n", b"\n")
        assert hashlib.sha256(data).hexdigest() == expected, "official kit was modified: " + relative
    # A frozen candidate must retain the actual reports it cites and the exact
    # runtime source tested by that CI. A green document alone is not evidence.
    evidence = ROOT/"validation/full-review-20261114"
    provenance = strict((evidence/"provenance.json").read_text(encoding="utf-8"))
    assert len(provenance) == 54
    for name, source in provenance.items():
        raw = (evidence/name).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == source["report_sha256"], name
        assert strict(raw.decode("utf-8"))["passed"] is True, name
    hashes = strict((evidence/"runtime-source-sha256.json").read_text(encoding="utf-8"))
    assert len(hashes) == 26
    for name, expected in hashes.items():
        raw = (ROOT/name).read_bytes().replace(b"\r\n", b"\n")
        assert hashlib.sha256(raw).hexdigest() == expected, "runtime differs from validated core: " + name
    trace = strict((evidence/"requirements-traceability.json").read_text(encoding="utf-8"))
    requirements = strict((ROOT/"tests/protocol_requirements.json").read_text(encoding="utf-8"))
    requirements += strict((ROOT/"tests/contest_requirements.json").read_text(encoding="utf-8"))
    assert len(trace["requirements"]) == len(requirements) == 64
    assert len({item["id"] for item in requirements}) == 64
    assert trace["platform_pending"] == ["X01", "X02", "X03", "X04"]
    for actual, expected in zip(trace["requirements"], requirements):
        for key, value in expected.items(): assert actual[key] == value, expected["id"]
        if actual["kind"] == "test":
            assert actual["passing_evidence"]
            for source in actual["passing_evidence"]:
                assert source["report_sha256"] == provenance[source["report"]]["report_sha256"]
    print(json.dumps(dict(passed=True, material_bytes=len(material),
                          material_sha256=hashlib.sha256(material).hexdigest(),
                          dependency_integrity=True, official_kit_unchanged=True,
                          retained_evidence_reports=len(provenance), runtime_source_files=len(hashes),
                          requirement_entries=len(requirements), platform_pending=len(trace["platform_pending"]))))

if __name__ == "__main__":
    sys.exit(main())
