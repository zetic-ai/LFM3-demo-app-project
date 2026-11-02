#!/usr/bin/env python3
"""Download a configured optimized LFM3 directory; never guess a release URL.

The expected package ID and immutable HF revision must come from the model
publisher. Integrity hashes are not a signature. Authentication is delegated to
huggingface_hub's environment/cache; credentials are never command arguments.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Callable

SCHEMA = "lfm3-ios-bundle-v1"
MANIFEST = "bundle_manifest.json"
INCOMPLETE = ".download-incomplete"
PACKAGES = ("prefill", "audio_preprocess", "audio_encoder", "vision_preprocess", "vision_encoder", "speech_depth", "speech_codec")
REQUIRED = {"metadata.json", "tokenizer.json", "embed_tokens_fp16.bin", "decode.metal/manifest.json", "decode.metal/weights.bin", "speech_feedback.bin"} | {
    f"{package}.mlpackage/{part}" for package in PACKAGES for part in ("Manifest.json", "Data/com.apple.CoreML/model.mlmodel", "Data/com.apple.CoreML/weights/weight.bin")
}


def safe_path(value: object) -> str:
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise ValueError("Unsafe model path")
    if any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise ValueError("Control character in model path")
    if any(part in ("", ".", "..") for part in value.split("/")):
        raise ValueError("Absolute, empty or parent model path")
    return value


def digest(value: object) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def package_fingerprint(entries: list[dict]) -> str:
    h = hashlib.sha256(SCHEMA.encode("ascii") + b"\0")
    for entry in sorted(entries, key=lambda item: item["path"].encode("utf-8")):
        h.update(entry["path"].encode() + b"\0" + str(entry["size"]).encode("ascii") + b"\0" + entry["sha256"].encode("ascii") + b"\n")
    return h.hexdigest()


def required_inventory(kind: str) -> set[str]:
    packages = {"text": ("prefill",), "audio": ("prefill", "audio_preprocess", "audio_encoder"),
                "vl": ("prefill", "vision_preprocess", "vision_encoder"), "omni": PACKAGES}
    if kind not in packages:
        raise ValueError("Unsupported model kind")
    required = {"metadata.json", "tokenizer.json", "embed_tokens_fp16.bin", "decode.metal/manifest.json", "decode.metal/weights.bin"}
    required |= {f"{package}.mlpackage/{part}" for package in packages[kind] for part in ("Manifest.json", "Data/com.apple.CoreML/model.mlmodel", "Data/com.apple.CoreML/weights/weight.bin")}
    if kind == "omni": required.add("speech_feedback.bin")
    if kind == "audio": required.add("audio_export_provenance.json")
    if kind == "vl": required.add("vl_export_provenance.json")
    return required


def validate_manifest(data: bytes, expected: str, provenance: str = "", kind: str = "omni") -> dict:
    if len(data) > 2_000_000 or not digest(expected):
        raise ValueError("Invalid manifest size or expected package fingerprint")
    value = json.loads(data)
    if not isinstance(value, dict) or value.get("schema") != SCHEMA:
        raise ValueError("This is not an optimized iOS bundle manifest")
    entries = value.get("files")
    if not isinstance(entries, list) or not 1 <= len(entries) <= 10000:
        raise ValueError("Invalid model file inventory")
    seen: set[str] = set()
    total = 0
    for entry in entries:
        if not isinstance(entry, dict):
            raise ValueError("Invalid model file record")
        name = safe_path(entry.get("path"))
        if name in seen or name in (MANIFEST, INCOMPLETE):
            raise ValueError("Duplicate or reserved model path")
        if type(entry.get("size")) is not int or not 0 <= entry["size"] <= 16 * 1024**3 or not digest(entry.get("sha256")):
            raise ValueError("Invalid file size or fingerprint")
        seen.add(name)
        total += entry["size"]
    if total > 30 * 1024**3:
        raise ValueError("Model inventory exceeds the download size limit")
    if package_fingerprint(entries) != expected or value.get("package_id") != expected:
        raise ValueError("Model package fingerprint does not match the trusted release")
    if not required_inventory(kind).issubset(seen):
        label = "Omni speech" if kind == "omni" else kind
        raise ValueError(f"Expected an optimized {label} bundle, not raw Hugging Face weights")
    if kind != "omni":
        if provenance:
            if kind != "vl" or not digest(provenance):
                raise ValueError("This model kind does not accept explicit provenance")
            recorded = next(entry["sha256"] for entry in entries if entry["path"] == "vl_export_provenance.json")
            if recorded != provenance:
                raise ValueError("VL provenance differs from the trusted release")
        return value
    reference = "omni_integrated_provenance.json" in seen
    exported = "speech_export_provenance.json" in seen
    if reference == exported:
        raise ValueError("Omni provenance must contain exactly one reference or explicit-export record")
    if exported:
        if not {"base_metadata.json", "omni_export_provenance.json"}.issubset(seen):
            raise ValueError("Explicit-export base provenance files are missing")
        recorded = next(entry["sha256"] for entry in entries if entry["path"] == "speech_export_provenance.json")
        if not digest(provenance) or provenance != recorded:
            raise ValueError("Explicit export requires the publisher-provided matching provenance SHA-256")
    elif provenance:
        raise ValueError("Reference bundles use the default policy, not an explicit-export provenance hash")
    return value


def materialize(fetch: Callable[[str], Path], output: Path, expected: str, subdir: str = "", provenance: str = "", kind: str = "omni") -> dict:
    if subdir:
        safe_path(subdir)
    prefix = subdir + "/" if subdir else ""
    raw = fetch(prefix + MANIFEST).read_bytes()
    manifest = validate_manifest(raw, expected, provenance, kind)
    if provenance and not digest(provenance):
        raise ValueError("Invalid provenance fingerprint")
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError("Output must be a new directory; existing data is never overwritten")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.mkdir()  # Atomic reservation: even an interrupted download stays visibly incomplete.
    marker = output / INCOMPLETE
    marker.write_text("Incomplete download. Do not import this directory.\n", encoding="utf-8")
    for entry in manifest["files"]:
        target = output / entry["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        source = fetch(prefix + entry["path"])
        h = hashlib.sha256()
        count = 0
        temporary = target.with_name(target.name + ".part")
        with source.open("rb") as src, temporary.open("xb") as dst:
            while chunk := src.read(1024 * 1024):
                count += len(chunk)
                if count > entry["size"]:
                    raise ValueError("Downloaded file is larger than declared")
                h.update(chunk)
                dst.write(chunk)
        if count != entry["size"] or h.hexdigest() != entry["sha256"]:
            raise ValueError("Downloaded file integrity check failed")
        if provenance and entry["path"] == ("vl_export_provenance.json" if kind == "vl" else "speech_export_provenance.json") and h.hexdigest() != provenance:
            raise ValueError("Export provenance does not match the trusted release")
        temporary.rename(target)
    if provenance and not (output / ("vl_export_provenance.json" if kind == "vl" else "speech_export_provenance.json")).is_file():
        raise ValueError("Requested export provenance is missing")
    (output / MANIFEST).write_bytes(raw)  # Publish the manifest only after every file passed.
    marker.unlink()
    return {"state": "complete", "package_id": expected, "files": len(manifest["files"]), "output": str(output)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=("text", "audio", "vl", "omni"), default="omni")
    parser.add_argument("--repo-id", required=True)
    parser.add_argument("--revision", required=True, help="Immutable 40-character HF commit SHA")
    parser.add_argument("--expected-package-id", required=True)
    parser.add_argument("--provenance-sha256", default="")
    parser.add_argument("--subdir", default="")
    parser.add_argument("--cache-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repo_id) or not re.fullmatch(r"[0-9a-f]{40}", args.revision):
        parser.error("Supply a publisher-provided repo ID and immutable revision")
    if not digest(args.expected_package_id) or (args.provenance_sha256 and not digest(args.provenance_sha256)):
        parser.error("Supply valid publisher-provided SHA-256 fingerprints")
    if args.subdir:
        try:
            safe_path(args.subdir)
        except ValueError:
            parser.error("Unsafe model repository subdirectory")
    try:
        from huggingface_hub import hf_hub_download
        def fetch(name: str) -> Path:
            return Path(hf_hub_download(repo_id=args.repo_id, revision=args.revision, filename=name, cache_dir=args.cache_dir))
        print(json.dumps(materialize(fetch, args.output, args.expected_package_id, args.subdir, args.provenance_sha256, args.kind)))
    except (ValueError, OSError, json.JSONDecodeError) as error:
        print(f"Download incomplete: {error}", file=sys.stderr)
        raise SystemExit(1) from None
    except ImportError:
        print("Install huggingface_hub to use this optional download helper.", file=sys.stderr)
        raise SystemExit(1) from None
    except Exception as error:
        # Do not echo authenticated request objects or arbitrary server output.
        print(f"Download incomplete ({type(error).__name__}); check repository access and the pinned release.", file=sys.stderr)
        raise SystemExit(1) from None


if __name__ == "__main__":
    main()
