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
    material_document = strict(material.decode("utf-8"))
    assert isinstance(material_document, dict)
    technical_guidance = material_document.get("document_kind") == "technical-guidance-and-validation-v1"
    if technical_guidance:
        assert "conversations" not in material_document and "owner_leadership_overview" not in material_document
        assert "助手" not in material.decode("utf-8") and "assistant" not in material.decode("utf-8").lower()
        instructions = strict((ROOT/"material/technical-guidance.json").read_text(encoding="utf-8"))
        assert material_document["guidance"] == instructions["guidance"]
        assert len(instructions["guidance"]) >= 6 and instructions["target_E"] == 5000
        records = material_document["verification_records"]
        record_ids = {r["id"] for r in records}
        assert len(record_ids) == len(records)
        for guide in instructions["guidance"]:
            assert guide["prompt"] and guide["method"] and set(guide["validation_refs"]) <= record_ids
    else:
        assert material_document["synthetic"] is False
        assert len([x for x in material_document["conversations"] if x["role"] == "user"]) >= 2
        records = material_document["experiments"]
        assert records
    dependencies = strict((ROOT/"third_party/DEPENDENCIES.json").read_text(encoding="utf-8"))
    for name in ["json.hpp", "json.LICENSE.MIT"]:
        assert hashlib.sha256((ROOT/"third_party"/name).read_bytes()).hexdigest() == dependencies["nlohmann/json"][name+".sha256"]
    assert hashlib.sha256((ROOT/"third_party/GCC-RUNTIME-EXCEPTION-3.1.txt").read_bytes()).hexdigest() == dependencies["gcc-runtime-exception"]["GCC-RUNTIME-EXCEPTION-3.1.txt.sha256"]
    vendor_headers = set()
    if "fast_float" in dependencies:
        fast_float = dependencies["fast_float"]
        assert fast_float["commit"] == "dbeabeeb3e6791cb96994c2a7a084c36e62ccfb2"
        assert len(fast_float["files_sha256"]) == 12
        for name, expected in fast_float["files_sha256"].items():
            relative = "third_party/fast_float/" + name
            assert hashlib.sha256((ROOT/relative).read_bytes()).hexdigest() == expected
            if name.endswith(".h"): vendor_headers.add(relative)
        oracle = strict((ROOT/"tests/fixtures/legacy_vector_literal.provenance.json").read_text(encoding="utf-8"))
        fixture = (ROOT/"tests/fixtures/legacy_vector_literal.cpp").read_bytes()
        assert hashlib.sha256(fixture).hexdigest() == oracle["fixture_sha256"]
        original = fixture.replace(b"namespace mvd::legacy_literal {", b"namespace mvd {", 1)
        assert hashlib.sha256(original).hexdigest() == oracle["original_sha256"]
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
    current_evidence = ROOT/("validation/e860-query-20261116" if (ROOT/"validation/e860-query-20261116").exists() else "validation/e860-write-20261116" if vendor_headers else "validation/e860-5000-20261116" if technical_guidance else "validation/e860-opt-20261116")
    current_lock = current_evidence/"runtime-inputs.json"
    current = strict(current_lock.read_text(encoding="utf-8")) if current_lock.exists() else None
    if current is not None:
        # Earlier green reports remain historical evidence. They must not be
        # rebound to changed production sources merely by retaining their files.
        assert hashlib.sha256((evidence/"runtime-source-sha256.json").read_bytes()).hexdigest() == "9217c7c79867452cc0383206fce3a32fae3d6de1e72fab5d14ca92443fbd4e26"
        assert current["base"] == "e86045306f5940613bfcf4aa84b0ee4bf74c20f1"
        assert current["formal_submitted"] is False and current["target_E"] == (5000 if technical_guidance else 4500)
        context = material_document["context"] if technical_guidance else material_document["current_context"]
        assert context["production_core"] == current["production_core"]
        for experiment in records:
            path = (ROOT/experiment["source_file"]).resolve()
            path.relative_to(ROOT.resolve())
            raw = path.read_bytes()
            assert hashlib.sha256(raw).hexdigest() == experiment["source_sha256"], experiment["id"]
            assert raw.decode("utf-8") == experiment["raw_report"], experiment["id"]
            report = strict(raw.decode("utf-8"))
            for diagnostic in report.get("diagnostic_inputs", []):
                diagnostic_path = (path.parent/diagnostic["file"]).resolve()
                diagnostic_path.relative_to(ROOT.resolve())
                diagnostic_bytes = diagnostic_path.read_bytes()
                assert hashlib.sha256(diagnostic_bytes).hexdigest() == diagnostic["sha256"]
                assert diagnostic_bytes.decode("utf-8") == diagnostic["raw_gate"]
            if "profile_file" in report:
                profile = (path.parent/report["profile_file"]).resolve()
                profile.relative_to(ROOT.resolve())
                profile_bytes = profile.read_bytes()
                assert hashlib.sha256(profile_bytes).hexdigest() == report["profile_sha256"]
                assert profile_bytes.decode("utf-8").startswith(report["raw_excerpt"])
                rejected = (path.parent/report["initial_rejection_file"]).resolve()
                rejected.relative_to(ROOT.resolve())
                assert hashlib.sha256(rejected.read_bytes()).hexdigest() == report["initial_rejection_sha256"]
            if "derived_from" in report:
                original = (path.parent/report["derived_from"]).resolve()
                original.relative_to(ROOT.resolve())
                original_bytes = original.read_bytes()
                assert hashlib.sha256(original_bytes).hexdigest() == report["derived_from_sha256"]
                original_report = strict(original_bytes.decode("utf-8"))
                for key, value in report.items():
                    if key not in ("derived_from", "derived_from_sha256", "selection"):
                        assert original_report[key] == value, experiment["id"] + ":" + key
        if not technical_guidance:
            history = strict((ROOT/"evidence/e860-history-20261116/approved-owner-history.json").read_text(encoding="utf-8"))["messages"]
            latest = strict((ROOT/"evidence/e860-history-20261116/latest-owner-history.json").read_text(encoding="utf-8"))["messages"]
            assert [m["content"] for m in material_document["conversations"]] == [m["content"] for m in history+latest], "actual source messages were rewritten"
        assert set(current["sha256_lf"]) == set(hashes) | vendor_headers
        for name, expected in current["sha256_lf"].items():
            assert hashlib.sha256((ROOT/name).read_bytes().replace(b"\r\n", b"\n")).hexdigest() == expected, name
        source = current_evidence/"production-source-match.json"
        assert hashlib.sha256(source.read_bytes()).hexdigest() == current["source_match_sha256"]
        source = strict(source.read_text(encoding="utf-8"))
        for name, expected in source["files_sha256_lf"].items():
            assert current["sha256_lf"][name] == expected, name
        for name, expected in current["evidence_reports"].items():
            raw = (current_evidence/name).read_bytes()
            assert hashlib.sha256(raw).hexdigest() == expected, name
            assert strict(raw.decode("utf-8"))["passed"] is True, name
    for name, expected in ({} if current is not None else hashes).items():
        raw = (ROOT/name).read_bytes().replace(b"\r\n", b"\n")
        if name == "Dockerfile":
            # Allow only the two portability edits relative to the recipe in
            # the historical CI reports. The live build is checked separately.
            raw = raw.replace(b"\nRUN make -j2 ", b"\nRUN --network=none make -j2 ")
            raw = raw.replace(b"\nRUN mkdir -p /data\n", b"\nRUN --network=none mkdir -p /data\n")
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
                          current_runtime_source_files=len(current["sha256_lf"]) if current else len(hashes),
                          vendored_parser_headers=len(vendor_headers),
                          historical_reports_are_current_runtime=current is None,
                          technical_guidance=technical_guidance,
                          guidance_count=len(material_document["guidance"]) if technical_guidance else 0,
                          current_evidence_reports=len(current["evidence_reports"]) if current is not None else 0,
                          requirement_entries=len(requirements), platform_pending=len(trace["platform_pending"]))))

if __name__ == "__main__":
    sys.exit(main())
