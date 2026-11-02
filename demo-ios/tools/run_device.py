#!/usr/bin/env python3
"""Build, install, stage verified models and launch on a connected iPhone."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

from download_model import validate_manifest, safe_path
from download_models import DOC_HASHES

APP = Path(__file__).resolve().parents[1]
SDK_ARCHIVE = APP.parent / "sdk/zetic-lfm3-ios-arm64.tar.gz"


def verify_models(root: Path, config: dict) -> None:
    if root.is_symlink() or not root.is_dir():
        raise ValueError("The downloaded model root must be a directory, not a symlink")
    paths = [safe_path(config["models"][kind]["model_directory"]) for kind in ("text", "audio", "vl", "omni")]
    if any(len(p.split("/")) != 3 or p.split("/")[0] != "apple" or p.split("/")[2] != "ios" for p in paths):
        raise ValueError("Model directories must be apple/<model>/ios")
    apple = root / "apple"
    if apple.is_symlink() or not apple.is_dir() or {p.name for p in apple.iterdir()} != {p.split("/")[1] for p in paths}:
        raise ValueError("Downloaded apple directory must contain exactly the four configured models")
    for kind, path in zip(("text", "audio", "vl", "omni"), paths):
        parent = root / "/".join(path.split("/")[:2])
        if parent.is_symlink() or not parent.is_dir() or {p.name for p in parent.iterdir()} != {"ios", "README.md", "LICENSE"}:
            raise ValueError("Each downloaded model directory must contain its ios bundle, README and LICENSE")
        for name, expected in zip(("README.md", "LICENSE"), DOC_HASHES[kind]):
            doc = parent / name
            if doc.is_symlink() or not doc.is_file() or doc.stat().st_size > 1_000_000 or hashlib.sha256(doc.read_bytes()).hexdigest() != expected:
                raise ValueError(f"{kind} release document verification failed: {name}")
    for kind in ("text", "audio", "vl", "omni"):
        release = config["models"][kind]
        rel = safe_path(release["model_directory"])
        directory = root
        for part in rel.split("/"):
            directory /= part
            if directory.is_symlink():
                raise ValueError(f"Symlink in {kind} model directory")
        manifest_path = directory / "bundle_manifest.json"
        if not manifest_path.is_file() or manifest_path.is_symlink():
            raise ValueError(f"Missing {kind} model manifest: {manifest_path}")
        manifest = validate_manifest(manifest_path.read_bytes(), release["expected_package_id"], release.get("provenance_sha256", ""), kind)
        expected = {e["path"] for e in manifest["files"]} | {"bundle_manifest.json"}
        actual = set()
        for path in directory.rglob("*"):
            if path.is_symlink():
                raise ValueError(f"Symlink in {kind} model bundle")
            if path.is_file():
                actual.add(path.relative_to(directory).as_posix())
            elif not path.is_dir():
                raise ValueError(f"Non-regular file in {kind} model bundle")
        if actual != expected:
            raise ValueError(f"{kind} model files do not match its manifest")
        for entry in manifest["files"]:
            path = directory / entry["path"]
            before = path.stat()
            h = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    h.update(block)
            after = path.stat()
            if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns) or before.st_size != entry["size"] or h.hexdigest() != entry["sha256"]:
                raise ValueError(f"{kind} model file verification failed: {entry['path']}")
        print(f"Verified {kind} model ({len(manifest['files'])} files)", flush=True)


def device_json(command: list[str]) -> dict:
    with tempfile.TemporaryDirectory(prefix="lfm3-device-") as temp:
        output = Path(temp) / "result.json"
        subprocess.run(command + ["--json-output", str(output)], check=True, stdout=subprocess.DEVNULL)
        result = json.loads(output.read_text())
        if result.get("info", {}).get("outcome") != "success":
            raise RuntimeError("Device command did not report success")
        return result["result"]


def assert_new_app(device: str, bundle_id: str) -> None:
    result = device_json(["xcrun", "devicectl", "device", "info", "apps", "--device", device, "--include-all-apps"])
    if not isinstance(result.get("apps"), list):
        raise RuntimeError("Could not inspect installed apps")
    if any(str(app.get("bundleIdentifier", "")).casefold() == bundle_id.casefold() for app in result["apps"]):
        raise ValueError("This bundle ID is already installed; choose a fresh ID to preserve its app and model data")


def assert_empty_destination(device: str, bundle_id: str) -> None:
    command = ["xcrun", "devicectl", "device", "info", "files", "--device", device,
               "--domain-type", "appDataContainer", "--domain-identifier", bundle_id]
    root = device_json(command + ["--no-recurse"])
    if not isinstance(root.get("files"), list):
        raise RuntimeError("Could not inspect the app container")
    if not any(item.get("name") == "Documents" for item in root["files"]):
        return
    result = device_json(command + ["--subdirectory", "Documents", "--no-recurse"])
    if not isinstance(result.get("files"), list):
        raise RuntimeError("Could not inspect the app Documents directory")
    if any(str(item.get("name", "")).casefold() == "apple" for item in result["files"]):
        raise ValueError("Documents/apple already exists; refusing to overwrite existing data")


def unpack_sdk(archive: Path, destination: Path) -> Path:
    checksum_file = archive.with_name(archive.name + ".sha256")
    parts = checksum_file.read_text().strip().split()
    if len(parts) != 2 or not re.fullmatch(r"[0-9a-f]{64}", parts[0]) or parts[1].lstrip("*") != archive.name:
        raise ValueError("Invalid bundled SDK checksum file")
    h = hashlib.sha256()
    with archive.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    if h.hexdigest() != parts[0]:
        raise ValueError("Bundled SDK archive checksum mismatch")
    with tarfile.open(archive, "r:gz") as tar:
        members = tar.getmembers()
        if not members or len(members) > 1000 or sum(m.size for m in members) > 2 * 1024**3:
            raise ValueError("Invalid bundled SDK archive inventory")
        seen = set()
        for member in members:
            name = member.name.rstrip("/")
            safe_path(name)
            if name in seen or not (member.isdir() or member.isfile()):
                raise ValueError("Bundled SDK archive contains duplicate, linked or special entries")
            seen.add(name)
        destination.mkdir(parents=True, exist_ok=False)
        for member in members:
            path = destination / member.name.rstrip("/")
            if member.isdir():
                path.mkdir(parents=True, exist_ok=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                source = tar.extractfile(member)
                if source is None:
                    raise ValueError("Bundled SDK file could not be read")
                with source, path.open("xb") as output:
                    shutil.copyfileobj(source, output)
    configs = list(destination.rglob("LFM3IOSConfig.cmake"))
    if len(configs) != 1:
        raise ValueError("Bundled SDK must contain exactly one CMake package")
    return configs[0].parent


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--models", type=Path, required=True, help="Output directory of download_models.py")
    parser.add_argument("--sdk-dir", type=Path, help="Optional compatible SDK install/lib/cmake/LFM3IOS")
    parser.add_argument("--build-dir", type=Path, required=True, help="New build directory; never overwritten")
    parser.add_argument("--device", required=True, help="Connected iPhone device identifier")
    parser.add_argument("--team", required=True, help="Your Apple development team identifier")
    parser.add_argument("--bundle-id", required=True, help="New bundle ID not already installed on the iPhone")
    args = parser.parse_args()
    try:
        if not re.fullmatch(r"[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+", args.bundle_id):
            raise ValueError("Provide a valid reverse-DNS bundle ID")
        if not re.fullmatch(r"[A-Za-z0-9]+", args.team):
            raise ValueError("Provide your Apple development team ID")
        if args.build_dir.exists() or args.build_dir.is_symlink():
            raise ValueError("Build directory already exists; choose a new one")
        if args.sdk_dir and not (args.sdk_dir / "LFM3IOSConfig.cmake").is_file():
            raise ValueError("SDK CMake package not found in the supplied directory")
        config = json.loads((APP / "Resources/ModelConfiguration.json").read_text())
        verify_models(args.models, config)
        assert_new_app(args.device, args.bundle_id)
        sdk_dir = args.sdk_dir or unpack_sdk(SDK_ARCHIVE, args.build_dir / "bundled-sdk")
        subprocess.run(["cmake", "-S", str(APP), "-B", str(args.build_dir), "-G", "Xcode",
                        "-DCMAKE_SYSTEM_NAME=iOS", "-DCMAKE_OSX_SYSROOT=iphoneos",
                        "-DCMAKE_OSX_ARCHITECTURES=arm64", "-DCMAKE_OSX_DEPLOYMENT_TARGET=18.0",
                        f"-DLFM3IOS_DIR={sdk_dir}", f"-DDEMO_BUNDLE_ID={args.bundle_id}",
                        f"-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM={args.team}",
                        "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Automatic"], check=True)
        subprocess.run(["cmake", "--build", str(args.build_dir), "--config", "Release", "--parallel", "2",
                        "--", "-allowProvisioningUpdates"], check=True)
        app = args.build_dir / "Release-iphoneos" / "ZeticOmniDemo.app"
        if not app.is_dir():
            raise RuntimeError(f"Built app not found: {app}")
        subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)
        # Recheck immediately before installing to avoid replacing an app installed during build.
        assert_new_app(args.device, args.bundle_id)
        subprocess.run(["xcrun", "devicectl", "device", "install", "app", "--device", args.device, str(app)], check=True)
        assert_empty_destination(args.device, args.bundle_id)
        subprocess.run(["xcrun", "devicectl", "device", "copy", "to", "--device", args.device,
                        "--source", str(args.models / "apple"), "--destination", "Documents/apple",
                        "--domain-type", "appDataContainer", "--domain-identifier", args.bundle_id], check=True)
        subprocess.run(["xcrun", "devicectl", "device", "process", "launch", "--device", args.device, args.bundle_id], check=True)
        print("App launched. Select an example; the app verifies the chosen model before inference.")
    except (ValueError, KeyError, OSError, RuntimeError, json.JSONDecodeError, tarfile.TarError, subprocess.CalledProcessError) as error:
        print(f"Setup failed: {error}", file=sys.stderr)
        raise SystemExit(1) from None


if __name__ == "__main__":
    main()
