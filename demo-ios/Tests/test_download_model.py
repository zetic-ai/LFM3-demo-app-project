import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location("download_model", Path(__file__).parents[1] / "tools/download_model.py")
assert SPEC and SPEC.loader
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class DownloadTests(unittest.TestCase):
    def test_typed_text_audio_vl_inventory_and_partial_rejection(self):
        for kind in ("text", "audio", "vl"):
            entries = [{"path": name, "size": 1, "sha256": hashlib.sha256(b"x").hexdigest()}
                       for name in sorted(module.required_inventory(kind))]
            value = {"schema": module.SCHEMA, "files": entries, "package_id": module.package_fingerprint(entries)}
            module.validate_manifest(json.dumps(value).encode(), value["package_id"], kind=kind)
            for missing in ("prefill.mlpackage/Manifest.json", "audio_export_provenance.json" if kind == "audio" else "vl_export_provenance.json" if kind == "vl" else "decode.metal/weights.bin"):
                partial = {**value, "files": [e for e in entries if e["path"] != missing]}
                partial["package_id"] = module.package_fingerprint(partial["files"])
                with self.assertRaisesRegex(ValueError, "optimized"):
                    module.validate_manifest(json.dumps(partial).encode(), partial["package_id"], kind=kind)
    def source(self, root):
        names = sorted(module.REQUIRED | {"omni_integrated_provenance.json"})
        entries = []
        for name in names:
            p = root / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(name.encode())
            entries.append({"path": name, "size": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()})
        h = hashlib.sha256(module.SCHEMA.encode() + b"\0")
        for entry in sorted(entries, key=lambda item: item["path"].encode()):
            h.update(entry["path"].encode() + b"\0" + str(entry["size"]).encode() + b"\0" + entry["sha256"].encode() + b"\n")
        expected = h.hexdigest()
        value = {"schema": module.SCHEMA, "files": entries, "package_id": expected}
        (root / module.MANIFEST).write_text(json.dumps(value))
        return expected, value

    def test_complete_inventory_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / "source"
            root.mkdir()
            expected, _ = self.source(root)
            output = Path(d) / "model"
            result = module.materialize(lambda name: root / name, output, expected)
            self.assertEqual(result["state"], "complete")
            self.assertTrue((output / module.MANIFEST).is_file())
            self.assertFalse((output / module.INCOMPLETE).exists())
            with self.assertRaises(ValueError):
                module.materialize(lambda name: root / name, output, expected)

    def test_corruption_stays_incomplete(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / "source"
            root.mkdir()
            expected, _ = self.source(root)
            (root / "tokenizer.json").write_bytes(b"changed")
            output = Path(d) / "model"
            with self.assertRaises(ValueError):
                module.materialize(lambda name: root / name, output, expected)
            self.assertTrue((output / module.INCOMPLETE).is_file())
            self.assertFalse((output / module.MANIFEST).exists())

    def test_untrusted_identity_paths_and_duplicates(self):
        with tempfile.TemporaryDirectory() as d:
            expected, value = self.source(Path(d))
            with self.assertRaises(ValueError):
                module.validate_manifest(json.dumps(value).encode(), "0" * 64)
            for path in ("/outside", "../outside", "x/../../outside", "x\\outside", "x//outside", "C:/outside", "x/./z"):
                bad = json.loads(json.dumps(value))
                bad["files"][0]["path"] = path
                with self.assertRaises(ValueError):
                    module.validate_manifest(json.dumps(bad).encode(), expected)
            value["files"].append(value["files"][0])
            with self.assertRaises(ValueError):
                module.validate_manifest(json.dumps(value).encode(), expected)

    def test_self_consistent_partial_bundle_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            _, value = self.source(Path(d))
            value["files"] = [entry for entry in value["files"] if entry["path"] != "speech_codec.mlpackage/Manifest.json"]
            expected = module.package_fingerprint(value["files"])
            value["package_id"] = expected
            with self.assertRaisesRegex(ValueError, "optimized Omni speech bundle"):
                module.validate_manifest(json.dumps(value).encode(), expected)

    def test_provenance_policy_and_explicit_base_inventory(self):
        with tempfile.TemporaryDirectory() as d:
            _, reference = self.source(Path(d))
            def checked(value, provenance=""):
                expected = module.package_fingerprint(value["files"])
                value["package_id"] = expected
                return module.validate_manifest(json.dumps(value).encode(), expected, provenance)
            missing = json.loads(json.dumps(reference))
            missing["files"] = [e for e in missing["files"] if e["path"] != "omni_integrated_provenance.json"]
            with self.assertRaisesRegex(ValueError, "exactly one"):
                checked(missing)
            explicit = json.loads(json.dumps(missing))
            for name in ("speech_export_provenance.json", "base_metadata.json", "omni_export_provenance.json"):
                explicit["files"].append({"path": name, "size": 1, "sha256": hashlib.sha256(b"x").hexdigest()})
            both = json.loads(json.dumps(explicit))
            both["files"].append(next(e for e in reference["files"] if e["path"] == "omni_integrated_provenance.json"))
            with self.assertRaisesRegex(ValueError, "exactly one"):
                checked(both)
            for fingerprint in ("", "0" * 64):
                with self.assertRaisesRegex(ValueError, "matching provenance"):
                    checked(explicit, fingerprint)
            expected_provenance = hashlib.sha256(b"x").hexdigest()
            self.assertEqual(checked(explicit, expected_provenance)["schema"], module.SCHEMA)
            partial = json.loads(json.dumps(explicit))
            partial["files"] = [e for e in partial["files"] if e["path"] != "base_metadata.json"]
            with self.assertRaisesRegex(ValueError, "base provenance"):
                checked(partial, expected_provenance)
            with self.assertRaisesRegex(ValueError, "default policy"):
                checked(reference, expected_provenance)


if __name__ == "__main__":
    unittest.main()
