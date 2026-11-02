#!/usr/bin/env python3
"""Download pinned typed LFM3 bundles into the app's Documents directory layout."""
from __future__ import annotations
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from download_model import materialize, safe_path, digest, validate_manifest

# Document fingerprints for the pinned ZETIC-ai/LFM3 release.
DOC_HASHES = {
    "text": ("c2a09d917c7ad6e1d0b1032ea8f9cd3de3cc12d7b6fe0f5b5fe5d3abe3d96bf0", "4d28ca14dedc0b3d0fcc2b3339f0e79931faa33874f3d24f522183a8fc70068c"),
    "audio": ("d38b4bb7e147f4ded206d17cffb1ae79a2dfa745c9bdd5ba8e73c69429be7690", "4d28ca14dedc0b3d0fcc2b3339f0e79931faa33874f3d24f522183a8fc70068c"),
    "vl": ("25757af09c2c9212a1b262e53d3417ea663a8bd689551362b5aea9311b268162", "30adf9d6478191fb87f2424f63ba0728598335aaf99cd2848ef17e8e545fe94b"),
    "omni": ("9b556c235932563b8e061fdcb3b7e60c9a0d5b6353a8f33eff02ecaa16f2a2e6", "4d28ca14dedc0b3d0fcc2b3339f0e79931faa33874f3d24f522183a8fc70068c"),
}


def download_documents(fetch, directory: Path, remote_dir: str, kind: str) -> None:
    """Add pinned release documents without overwriting existing files."""
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError("Model directory is missing or is a symlink")
    pending = {}
    for filename, expected in zip(("README.md", "LICENSE"), DOC_HASHES[kind]):
        target = directory / filename
        if target.is_symlink():
            raise ValueError("Release document is a symlink")
        source = fetch(remote_dir + "/" + filename)
        if source.stat().st_size > 1_000_000:
            raise ValueError("Release document is too large")
        data = source.read_bytes()
        if len(data) > 1_000_000 or hashlib.sha256(data).hexdigest() != expected:
            raise ValueError("Release document checksum mismatch")
        if target.exists():
            if not target.is_file() or target.read_bytes() != data:
                raise ValueError("Existing release document differs; nothing was overwritten")
        else:
            pending[target] = data
    for target, data in pending.items():
        with target.open("xb") as output:
            output.write(data)


def reject_output_symlinks(root: Path, subdir: str) -> None:
    current = root
    if current.is_symlink():
        raise ValueError("Output root is a symlink")
    for part in safe_path(subdir).split("/"):
        current /= part
        if current.is_symlink():
            raise ValueError("Output path contains a symlink")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path(__file__).parents[1] / "Resources/ModelConfiguration.json")
    parser.add_argument("--model", choices=("all", "text", "audio", "vl", "omni"), default="all")
    parser.add_argument("--output", type=Path, required=True, help="New/local Documents-layout root; individual model directories must not exist")
    parser.add_argument("--cache-dir", type=Path)
    parser.add_argument("--docs-only", action="store_true", help="Add release README/LICENSE to previously downloaded bundles; payload files are not revalidated")
    args = parser.parse_args()
    try:
        config = json.loads(args.config.read_text())
        repo, revision = config["repo_id"], config["revision"]
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo) or not re.fullmatch(r"[0-9a-f]{40}", revision):
            raise ValueError("A trusted repository and immutable revision are required")
        if repo != "ZETIC-ai/LFM3" or revision != "24a56588cb8a84e25c821217a02ddc437ed7a30c":
            raise ValueError("Release document fingerprints are only configured for the pinned ZETIC-ai/LFM3 release")
        from huggingface_hub import hf_hub_download
        def fetch(name: str) -> Path:
            return Path(hf_hub_download(repo_id=repo, revision=revision, filename=name, cache_dir=args.cache_dir))
        for kind in (config["models"] if args.model == "all" else [args.model]):
            if kind not in ("text", "audio", "vl", "omni"):
                raise ValueError("Unsupported model kind")
            item = config["models"][kind]
            subdir = safe_path(item["model_directory"])
            reject_output_symlinks(args.output, subdir)
            if not digest(item["expected_package_id"]):
                raise ValueError("Missing trusted model package fingerprint")
            destination = args.output / subdir
            if args.docs_only:
                manifest_path = destination / "bundle_manifest.json"
                if manifest_path.is_symlink() or (destination / ".download-incomplete").exists():
                    raise ValueError("Model bundle is incomplete or has a symlinked manifest")
                raw = manifest_path.read_bytes()
                manifest = validate_manifest(raw, item["expected_package_id"], item.get("provenance_sha256", ""), kind)
                result = {"state": "documents_added_payload_not_revalidated", "package_id": item["expected_package_id"], "files": len(manifest["files"]), "output": str(destination)}
            else:
                result = materialize(fetch, destination, item["expected_package_id"], subdir, item.get("provenance_sha256", ""), kind)
            download_documents(fetch, destination.parent, subdir.rsplit("/", 1)[0], kind)
            print(json.dumps({"model": kind, "revision": revision, **result}), flush=True)
    except (ValueError, KeyError, OSError, json.JSONDecodeError) as error:
        print(f"Download incomplete: {error}", file=sys.stderr)
        raise SystemExit(1) from None
    except Exception as error:
        print(f"Download incomplete ({type(error).__name__}); check access and the pinned release.", file=sys.stderr)
        raise SystemExit(1) from None


if __name__ == "__main__":
    main()
