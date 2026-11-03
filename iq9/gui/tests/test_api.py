"""Model-independent API checks: validation, media normalization, and isolation."""

import asyncio
import copy
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
import wave

from aiohttp.test_utils import TestClient, TestServer

GUI = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("gui_server", GUI / "server.py")
server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)


class ApiTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.old_state = server.STATE
        server.STATE = Path(self.tmp.name)
        self.config = json.loads((GUI / "config.example.json").read_text())
        self.client = TestClient(TestServer(server.create_app(self.config)))
        await self.client.start_server()

    async def asyncTearDown(self):
        await self.client.close()
        server.STATE = self.old_state
        self.tmp.cleanup()

    async def test_samples_and_normalized_audio(self):
        sample = await (await self.client.get("/api/sample/summary")).json()
        self.assertGreater(len(sample["text"]), 5000)
        sample = await (await self.client.get("/api/sample/voice")).json()
        self.assertEqual(sample["attachments"][0]["name"], "jfk.wav")
        audio = await (await self.client.get(sample["attachments"][0]["url"])).read()
        with wave.open(io.BytesIO(audio), "rb") as wav:
            self.assertEqual(wav.getframerate(), 16000)
            self.assertEqual(wav.getnchannels(), 1)
            self.assertGreater(wav.getnframes() / 16000, 10)

    async def test_rejects_long_audio_without_truncating(self):
        source = server.STATE / "too-long.wav"
        source.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(source), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(bytes(31 * 16000 * 2))
        with self.assertRaisesRegex(ValueError, "not truncated"):
            await self.client.app["gui"].normalize(source, "audio", "too-long.wav")

    async def test_image_and_video_share_exact_normalized_pixels(self):
        handler = self.client.app["gui"]
        image = await handler.normalize(
            server.SAMPLES / "d1_image/cats.ppm", "image", "cats.ppm"
        )
        clip = server.STATE / "source.mkv"
        await server.command(
            "ffmpeg",
            "-v",
            "error",
            "-y",
            "-loop",
            "1",
            "-i",
            image["path"],
            "-t",
            "2",
            "-r",
            "2",
            "-c:v",
            "ffv1",
            clip,
        )
        video = await handler.normalize(clip, "video", "source.mkv")
        resolved = handler.resolve_attachments([video])[0]
        self.assertEqual(len(resolved["frames"]), 4)
        raw = await server.command(
            "ffmpeg",
            "-v",
            "error",
            "-i",
            resolved["frames"][0],
            "-pix_fmt",
            "rgb24",
            "-f",
            "rawvideo",
            "-",
        )
        muxed = await server.command(
            "ffmpeg",
            "-v",
            "error",
            "-i",
            resolved["video_path"],
            "-frames:v",
            "1",
            "-pix_fmt",
            "rgb24",
            "-f",
            "rawvideo",
            "-",
        )
        self.assertEqual(raw, muxed)
        self.assertEqual(len(raw), 512 * 512 * 3)

    async def test_validation_and_local_origin(self):
        handler = self.client.app["gui"]
        q = {"model": "omni", "messages": [{"role": "user", "text": "Hello"}]}
        self.assertEqual(
            handler.validate(copy.deepcopy(q))["settings"]["context"], 2048
        )
        for settings in [
            {"context": 10},
            {"context": 2048, "max_tokens": 4096},
            {"max_audio_seconds": 31},
        ]:
            bad = copy.deepcopy(q)
            bad["settings"] = settings
            response = await self.client.post("/api/run", json=bad)
            self.assertEqual(response.status, 400)
        response = await self.client.post(
            "/api/run", json=q, headers={"Origin": "https://external.invalid"}
        )
        self.assertEqual(response.status, 403)
        with self.assertRaisesRegex(ValueError, "Invalid attachment"):
            handler.resolve_attachments([{"id": "../private", "type": "image"}])
        response = await self.client.post(
            "/api/run", json={**q, "engines": ["unsupported"]}
        )
        self.assertEqual(response.status, 400)

    async def test_decision_options_require_unique_labels(self):
        q = {
            "model": "d1",
            "state": "A ticket",
            "questions": [
                {
                    "id": "q",
                    "type": "choice",
                    "instructions": "Route?",
                    "criteria": [{"label": "billing"}, {"label": "billing"}],
                }
            ],
        }
        response = await self.client.post("/api/run", json=q)
        self.assertEqual(response.status, 400)
        self.assertIn("unique", (await response.json())["error"])

    async def test_cancel_is_idempotent(self):
        runtime = self.client.app["gui"].runtime
        sent = []
        async def send(message):
            sent.append(message)
        runtime.worker.send = send
        await runtime.cancel()
        await runtime.cancel()
        self.assertEqual(sent, [{"op": "cancel"}])

    async def test_stop_during_load_does_not_start_inference(self):
        runtime = self.client.app["gui"].runtime
        async def prepare(*args):
            runtime.cancelled = True
            return 100
        runtime.prepare = prepare
        response = await self.client.post("/api/run", json={
            "model": "omni", "messages": [{"role": "user", "text": "Hello"}]
        })
        events = [json.loads(line) for line in (await response.text()).splitlines()]
        self.assertEqual([e["event"] for e in events], ["run_start", "cancelled", "complete"])
        self.assertTrue(events[-1]["cancelled"])


if __name__ == "__main__":
    unittest.main()
