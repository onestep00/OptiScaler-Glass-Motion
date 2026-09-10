"""Validate a local recording, run standalone FG, and inventory actual outputs.

Standard library only. No game attachment or quality score is inferred.
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--compare", type=Path)
    parser.add_argument("--expect-identical", action="store_true")
    args = parser.parse_args()
    if args.expect_identical and args.compare is None:
        parser.error("--expect-identical requires --compare")
    manifest = args.manifest.resolve(strict=True)
    config = json.loads(manifest.read_text(encoding="utf-8-sig"))

    def path(key):
        return (manifest.parent / config[key]).resolve()

    if config["formatProfile"] != "rgba8-mv16f-depth32":
        raise ValueError("Unsupported format profile")
    if "userInterfaceRecomposition" in config and type(config["userInterfaceRecomposition"]) is not bool:
        raise ValueError("userInterfaceRecomposition must be boolean")
    for key in ("outputWidth", "outputHeight", "renderWidth", "renderHeight"):
        if type(config[key]) is not int or not 1 <= config[key] <= 16384:
            raise ValueError(f"Invalid {key}")
    frames, phases = config["frames"], config["generatedCount"]
    if type(frames) is not int or not 1 <= frames <= 4096 or phases not in (1, 3):
        raise ValueError("Invalid frame or phase count")
    for key in ("applicationId", "sdkVersion"):
        if type(config[key]) is not int or config[key] < 0:
            raise ValueError(f"Invalid {key}")
    executable = args.executable.resolve(strict=True)
    output = path("output")
    log = output.with_name(output.name + ".log")
    report_path = output.with_name(output.name + ".report.json")
    if any(p.exists() for p in (output, log, report_path)):
        raise ValueError("Use a fresh output, log and report path")
    identities = {"executable": {"path": str(executable), "sha256": digest(executable)}}
    for key in ("provider", "unlock") if phases == 3 else ("provider",):
        value = digest(path(key))
        if value != config[key + "Sha256"].lower():
            raise ValueError(f"{key} identity changed")
        identities[key] = {"path": str(path(key)), "sha256": value}
    packet = path("arguments")
    with packet.open("rb") as stream:
        if stream.read(8) != struct.pack("<II", 0x31524647, frames):
            raise ValueError("Packet header does not match manifest")
    identities["arguments"] = {"path": str(packet), "sha256": digest(packet)}
    color_bytes = config["outputWidth"] * config["outputHeight"] * 4
    render_pixels = config["renderWidth"] * config["renderHeight"]
    inputs = []
    overrides = path("overrides") if "overrides" in config else None
    for frame in range(frames):
        for slot, size in ((0, color_bytes), (7, color_bytes), (4, render_pixels * 8), (5, render_pixels * 4)):
            name = f"frame-{frame:02d}-index-{slot}.bin"
            original = path("capture") / name
            selected = original
            if overrides and slot in (4, 5) and (overrides / name).exists():
                selected = overrides / name
            if original.stat().st_size != size or selected.stat().st_size != size:
                raise ValueError(f"Wrong input length: {name}")
            original_hash = digest(original)
            selected_hash = original_hash if selected == original else digest(selected)
            inputs.append({"frame": frame, "slot": slot, "path": str(selected), "bytes": size,
                           "originalSha256": original_hash, "selectedSha256": selected_hash,
                           "changed": original_hash != selected_hash})
        if "uiAlpha" in config:
            alpha = path("uiAlpha") / f"frame-{frame:02d}-ui-alpha.bin"
            if alpha.stat().st_size != color_bytes // 4:
                raise ValueError(f"Wrong UI alpha length: {alpha.name}")
            inputs.append({"frame": frame, "slot": "UIAlpha", "path": str(alpha),
                           "bytes": color_bytes // 4, "selectedSha256": digest(alpha), "configured": True})
    output.parent.mkdir(parents=True, exist_ok=True)
    with log.open("xb") as stream:
        result = subprocess.run([str(executable), str(manifest)], stdout=stream, stderr=subprocess.STDOUT)
    text = log.read_text(errors="replace")
    calls = re.findall(r"REPLAY_EVAL frame=(\d+) index=(\d+) generated=(\d+) result=(\w+)", text)
    expected_calls = [(str(f), str(p), str(phases), "00000001")
                      for f in range(frames) for p in range(1, phases + 1)]
    outputs = []
    for frame in range(frames):
        for phase in range(1, phases + 1):
            name = f"frame-{frame:02d}-generated-{phase * 100 // (phases + 1):02d}.bin"
            item = {"name": name, "complete": False}
            file = output / name
            if file.is_file() and file.stat().st_size == color_bytes:
                item.update(complete=True, sha256=digest(file))
                if args.compare:
                    reference = args.compare / name
                    item["referenceIdentical"] = (reference.is_file()
                        and reference.stat().st_size == color_bytes and digest(reference) == item["sha256"])
            outputs.append(item)
    complete = (result.returncode == 0 and calls == expected_calls and all(x["complete"] for x in outputs)
                and f"REPLAY_DONE frames={frames} generated_count={phases}" in text)
    identical = all(x.get("referenceIdentical", False) for x in outputs) if args.compare else None
    report = {"manifest": str(manifest), "manifestSha256": digest(manifest), "identities": identities,
              "inputs": inputs, "outputs": outputs, "exitCode": result.returncode,
              "evaluations": len(calls), "complete": complete, "referenceIdentical": identical,
              "creationSource": "configured defaults; not captured creation",
              "providerHistory": "fresh feature; not restored live history", "qualityAccepted": False}
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"report": str(report_path), "complete": complete,
                      "evaluations": len(calls), "referenceIdentical": identical}))
    return 0 if complete and (not args.expect_identical or identical) else 1


if __name__ == "__main__":
    raise SystemExit(main())
