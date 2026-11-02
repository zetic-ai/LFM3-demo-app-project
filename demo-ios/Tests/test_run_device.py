import hashlib
import io
import os
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import run_device
import download_models


class RunDeviceTests(unittest.TestCase):
    def test_release_documents_are_checked_and_never_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            target = root / "target"
            source.mkdir()
            target.mkdir()
            (source / "README.md").write_bytes(b"readme")
            (source / "LICENSE").write_bytes(b"license")
            expected = (hashlib.sha256(b"readme").hexdigest(), hashlib.sha256(b"license").hexdigest())
            with patch.object(download_models, "DOC_HASHES", {"text": expected}):
                fetch = lambda path: source / path.rsplit("/", 1)[-1]
                download_models.download_documents(fetch, target, "apple/text", "text")
                download_models.download_documents(fetch, target, "apple/text", "text")
                (target / "LICENSE").write_bytes(b"do not overwrite")
                with self.assertRaisesRegex(ValueError, "Existing release document differs"):
                    download_models.download_documents(fetch, target, "apple/text", "text")
                self.assertEqual((target / "LICENSE").read_bytes(), b"do not overwrite")

    def test_model_inventory_and_file_hashes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = {"models": {}}
            data = b"test model"
            for kind in ("text", "audio", "vl", "omni"):
                path = f"apple/{kind}/ios"
                config["models"][kind] = {"model_directory": path, "expected_package_id": "a" * 64}
                directory = root / path
                directory.mkdir(parents=True)
                (directory / "bundle_manifest.json").write_text("{}")
                (directory / "weights.bin").write_bytes(data)
                (directory.parent / "README.md").write_bytes(b"readme")
                (directory.parent / "LICENSE").write_bytes(b"license")
            manifest = {"files": [{"path": "weights.bin", "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}]}
            hashes = (hashlib.sha256(b"readme").hexdigest(), hashlib.sha256(b"license").hexdigest())
            with patch.object(run_device, "validate_manifest", return_value=manifest), patch.object(run_device, "DOC_HASHES", {k: hashes for k in config["models"]}):
                run_device.verify_models(root, config)
                (root / "apple/text/ios/weights.bin").write_bytes(b"bad model")
                with self.assertRaisesRegex(ValueError, "verification failed"):
                    run_device.verify_models(root, config)
                (root / "apple/text/ios/weights.bin").write_bytes(data)
                if hasattr(os, "mkfifo"):
                    fifo = root / "apple/text/ios/unexpected-pipe"
                    os.mkfifo(fifo)
                    with self.assertRaisesRegex(ValueError, "Non-regular file"):
                        run_device.verify_models(root, config)
                    fifo.unlink()
                (root / "apple/unexpected").mkdir()
                with self.assertRaisesRegex(ValueError, "exactly the four"):
                    run_device.verify_models(root, config)

    def archive(self, root, name="sdk/lib/cmake/LFM3IOS/LFM3IOSConfig.cmake", kind=tarfile.REGTYPE):
        archive = root / "sdk.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            content = b"cmake config"
            member = tarfile.TarInfo(name)
            member.type = kind
            member.size = len(content) if kind == tarfile.REGTYPE else 0
            if kind == tarfile.SYMTYPE:
                member.linkname = "../../outside"
            tar.addfile(member, io.BytesIO(content) if member.size else None)
        archive.with_name(archive.name + ".sha256").write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + "  " + archive.name + "\n")
        return archive

    def test_sdk_checksum_and_package_discovery(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = self.archive(root)
            found = run_device.unpack_sdk(archive, root / "out")
            self.assertTrue((found / "LFM3IOSConfig.cmake").is_file())
            archive.write_bytes(b"tampered")
            with self.assertRaisesRegex(ValueError, "checksum"):
                run_device.unpack_sdk(archive, root / "other")

    def test_sdk_rejects_traversal_and_links(self):
        for name, kind in (("../outside", tarfile.REGTYPE), ("sdk/link", tarfile.SYMTYPE)):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                with self.assertRaises(ValueError):
                    run_device.unpack_sdk(self.archive(root, name, kind), root / "out")
                self.assertFalse((root / "outside").exists())

    def test_existing_app_is_never_replaced(self):
        with patch.object(run_device, "device_json", return_value={"apps": [{"bundleIdentifier": "ai.example.app"}]}) as query:
            with self.assertRaisesRegex(ValueError, "already installed"):
                run_device.assert_new_app("device", "ai.example.app")
            self.assertIn("--include-all-apps", query.call_args.args[0])
            run_device.assert_new_app("device", "ai.example.other")

    def test_existing_model_destination_is_never_overwritten(self):
        root = {"files": [{"name": "Documents"}]}
        docs = {"files": [{"name": "apple"}]}
        with patch.object(run_device, "device_json", side_effect=[root, docs]):
            with self.assertRaisesRegex(ValueError, "already exists"):
                run_device.assert_empty_destination("device", "ai.example.app")
        with patch.object(run_device, "device_json", return_value={"files": []}):
            run_device.assert_empty_destination("device", "ai.example.app")


if __name__ == "__main__":
    unittest.main()
