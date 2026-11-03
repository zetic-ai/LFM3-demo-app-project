#!/usr/bin/env python3
"""Local IQ9 comparison app. Models remain on the board; no cloud inference."""

import argparse
import asyncio
import base64
import contextlib
import fcntl
import io
import json
import mimetypes
import os
from pathlib import Path
import shutil
import signal
import struct
import time
import uuid
import wave

from aiohttp import web, ClientSession, ClientTimeout, TCPConnector, WSMsgType

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent.parent
SAMPLES = REPO / "samples"
STATE = Path(os.environ.get("LFM3_GUI_STATE", Path.home() / ".local/share/lfm3-iq9"))
MAX_UPLOAD = 100 * 1024 * 1024


def expand(value):
    return str(Path(value).expanduser().resolve())


def pcm_wav(pcm, rate):
    out = io.BytesIO()
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)
    return out.getvalue()


async def command(*args):
    proc = await asyncio.create_subprocess_exec(
        *map(str, args), stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE
    )
    out, err = await proc.communicate()
    if proc.returncode:
        raise ValueError(err.decode(errors="replace")[-1200:])
    return out


class Worker:
    def __init__(self):
        self.proc = None
        self.log = None

    async def start(self):
        if self.proc and self.proc.returncode is None:
            return
        binary = REPO / "iq9/out/sdk_worker"
        if not binary.is_file():
            raise ValueError("SDK worker is not built. Run: make -C iq9 out/sdk_worker")
        self.log = (STATE / "sdk.log").open("ab")
        self.proc = await asyncio.create_subprocess_exec(
            str(binary),
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=self.log,
            limit=4 * 1024 * 1024,
        )
        event = json.loads(await asyncio.wait_for(self.proc.stdout.readline(), 30))
        if event.get("event") != "ready":
            raise ValueError("SDK worker failed to start")

    async def send(self, q):
        if self.proc and self.proc.returncode is None:
            self.proc.stdin.write((json.dumps(q) + "\n").encode())
            await self.proc.stdin.drain()

    async def events(self, q):
        await self.start()
        await self.send(q)
        while True:
            line = await asyncio.wait_for(self.proc.stdout.readline(), 300)
            if not line:
                raise ValueError("SDK worker exited. See SDK log in settings.")
            event = json.loads(line)
            if event.get("event") == "error":
                raise ValueError(event.get("message", "SDK error"))
            yield event
            if event.get("event") in ("done", "loaded"):
                return

    async def close(self):
        if self.proc and self.proc.returncode is None:
            await self.send({"op": "cancel"})
            self.proc.stdin.close()
            try:
                await asyncio.wait_for(self.proc.wait(), 8)
            except asyncio.TimeoutError:
                self.proc.terminate()
                await self.proc.wait()
        if self.log:
            self.log.close()
        self.proc = None


class Runtime:
    def __init__(self, config):
        self.config = config
        self.worker = Worker()
        self.llama = None
        self.llama_log = None
        self.active = None
        self.lock = asyncio.Lock()
        self.cancelled = False
        self.task = None
        self.client = None
        self.device_lock = None

    def claim_device(self):
        fault = self.device_fault()
        if fault:
            raise ValueError(fault)
        # File lock is released on process death; text marker allows existing board workflows to see ownership.
        marker = Path.home() / "BUSY_lfm3_comparison_gui"
        others = [p for p in Path.home().glob("BUSY_*") if p != marker]
        if others:
            raise ValueError("IQ9 is reserved by: " + ", ".join(p.name for p in others))
        if self.device_lock is None:
            self.device_lock = marker.open("a+")
            try:
                fcntl.flock(self.device_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                self.device_lock.close()
                self.device_lock = None
                raise ValueError("Another comparison app is using the IQ9")
            self.device_lock.seek(0)
            self.device_lock.truncate()
            self.device_lock.write(f"IQ9 comparison GUI pid={os.getpid()}\n")
            self.device_lock.flush()

    @staticmethod
    def device_fault():
        for index in (0, 1):
            state = Path(f"/sys/class/remoteproc/remoteproc{index}/state")
            if state.exists() and state.read_text().strip() == "crashed":
                return f"IQ9 NPU needs recovery: remoteproc{index} is crashed. Inference has stopped."
        return None

    async def release(self, delay=True):
        had_active = self.active is not None
        if self.llama and self.llama.returncode is None:
            self.llama.terminate()
            try:
                await asyncio.wait_for(self.llama.wait(), 8)
            except asyncio.TimeoutError:
                self.llama.kill()
                await self.llama.wait()
        self.llama = None
        if self.llama_log:
            self.llama_log.close()
            self.llama_log = None
        await self.worker.close()
        self.active = None
        if had_active and delay:
            await asyncio.sleep(self.config.get("switch_delay_seconds", 5))

    async def prepare(self, engine, model_id, settings, output, notify):
        self.claim_device()
        model = self.config["models"][model_id]
        speech = output != "text"
        key = (
            engine,
            model_id,
            settings["context"],
            speech if engine == "llama" else False,
            settings.get("set_size", 16),
            settings.get("calibrate", True),
            (
                tuple(
                    settings.get(k)
                    for k in (
                        "temperature",
                        "top_p",
                        "top_k",
                        "seed",
                        "repetition_penalty",
                    )
                )
                if engine == "llama" and speech
                else ()
            ),
        )
        if key == self.active:
            return 0.0
        if self.active:
            await notify(
                {
                    "event": "status",
                    "message": "Switching engine · releasing the previous NPU session…",
                }
            )
            await self.release()
        if self.cancelled:
            raise asyncio.CancelledError()
        start = time.perf_counter()
        await notify(
            {
                "event": "status",
                "message": f'Loading {model["name"]} · {engine.upper()}…',
            }
        )
        # Mark ownership before attempting a load so failure cleanup also releases the session.
        self.active = key
        try:
            if engine == "zetic":
                async for event in self.worker.events(
                    {
                        "id": str(uuid.uuid4()),
                        "op": "load",
                        "path": expand(model["sdk"]),
                        "context": settings["context"],
                        "decision": model["kind"] == "decision",
                        "set_size": settings.get("set_size", 16),
                        "calibrate": settings.get("calibrate", True),
                        "lib_dir": str(REPO / "iq9/sdk/lib"),
                    }
                ):
                    pass
            else:
                directory = Path(model["llama_dir"]).expanduser()
                if (
                    not (directory / "llama-server").is_file()
                    or not Path(model["gguf"]).expanduser().is_file()
                ):
                    raise ValueError(
                        "llama.cpp binary or GGUF is missing. Check model paths in config.json."
                    )
                args = [
                    str(directory / "llama-server"),
                    "-m",
                    expand(model["gguf"]),
                    "-dev",
                    "HTP0",
                    "-ngl",
                    "99",
                    "-fa",
                    "on",
                    "-t",
                    "6",
                    "-c",
                    str(settings["context"]),
                    "-b",
                    "4096" if model["kind"] == "decision" else "1024",
                    "-ub",
                    "4096" if model["kind"] == "decision" else "1024",
                    "-np",
                    "1",
                    "--host",
                    "127.0.0.1",
                    "--port",
                    str(self.config.get("llama_port", 18765)),
                ]
                args += [
                    "--temp",
                    str(settings["temperature"]),
                    "--top-p",
                    str(settings["top_p"]),
                    "--top-k",
                    str(settings["top_k"]),
                    "--seed",
                    str(settings["seed"]),
                    "--repeat-penalty",
                    str(settings["repetition_penalty"]),
                ]
                if model.get("mmproj"):
                    args += [
                        "--mmproj",
                        expand(model["mmproj"]),
                        "-mmdev",
                        "none" if speech else "HTP0",
                    ]
                    if "video" in model["capabilities"]:
                        args += ["--video-fps", "2"]
                env = os.environ.copy()
                env["LD_LIBRARY_PATH"] = str(directory)
                env["GGML_HEXAGON_DEVICES"] = "HTP0[0-1]"
                self.llama_log = (STATE / "llama.log").open("ab")
                self.llama = await asyncio.create_subprocess_exec(
                    *args,
                    cwd=directory,
                    env=env,
                    stdout=self.llama_log,
                    stderr=self.llama_log,
                )
                for _ in range(240):
                    if self.llama.returncode is not None:
                        raise ValueError(
                            "llama.cpp could not load. Check llama.log in the app data directory."
                        )
                    try:
                        async with self.client.get(
                            self.url("/health"), timeout=ClientTimeout(total=2)
                        ) as r:
                            if r.status == 200:
                                break
                    except (OSError, asyncio.TimeoutError):
                        pass
                    await asyncio.sleep(0.5)
                else:
                    raise ValueError("llama.cpp load timed out")
            return (time.perf_counter() - start) * 1000
        except BaseException:
            await self.release()
            raise

    def url(self, path):
        return f'http://127.0.0.1:{self.config.get("llama_port",18765)}{path}'

    async def cancel(self):
        if self.cancelled:
            return
        self.cancelled = True
        await self.worker.send({"op": "cancel"})
        # SDK supports cooperative cancel. HTTP/realtime tasks are cancelled and their model process released.
        if self.task and self.active and self.active[0] == "llama":
            self.task.cancel()

    async def run_zetic(self, q, notify):
        command = dict(q, op="decision" if q["kind"] == "decision" else "chat")
        pcm = bytearray()
        rate = 24000
        final = None
        async for ev in self.worker.events(command):
            if self.cancelled and ev["event"] not in ("done", "error"):
                continue
            if ev["event"] == "audio":
                pcm.extend(base64.b64decode(ev["pcm"]))
                rate = ev["rate"]
            if ev["event"] == "done":
                final = ev
            else:
                await notify(ev)
        if final is None:
            raise ValueError("No SDK result")
        if pcm:
            name = q["id"] + ".wav"
            (STATE / "media" / name).write_bytes(pcm_wav(pcm, rate))
            final["audio_url"] = "/media/" + name
        return final

    def llama_messages(self, messages):
        out = []
        for m in messages:
            content = []
            for a in m.get("attachments", []):
                media = (
                    Path(a["path"]).read_bytes()
                    if a.get("path") and a["type"] in ("image", "audio")
                    else b""
                )
                if a["type"] == "image":
                    content.append(
                        {
                            "type": "image_url",
                            "image_url": {
                                "url": "data:image/png;base64,"
                                + base64.b64encode(media).decode()
                            },
                        }
                    )
                elif a["type"] == "audio":
                    content.append(
                        {
                            "type": "input_audio",
                            "input_audio": {
                                "data": base64.b64encode(media).decode(),
                                "format": "wav",
                            },
                        }
                    )
                elif a["type"] == "video":
                    content.append(
                        {
                            "type": "input_video",
                            "input_video": {
                                "data": "data:video/x-matroska;base64,"
                                + base64.b64encode(
                                    Path(a["video_path"]).read_bytes()
                                ).decode()
                            },
                        }
                    )
            if m.get("text"):
                content.append({"type": "text", "text": m["text"]})
            out.append({"role": m.get("role", "user"), "content": content})
        return out

    async def run_llama_chat(self, q, notify):
        s = q["settings"]
        start = time.perf_counter()
        first = None
        text = ""
        timings = {}
        usage = {}
        body = {
            "messages": self.llama_messages(q["messages"]),
            "stream": True,
            "stream_options": {"include_usage": True},
            "max_tokens": s["max_tokens"],
            "temperature": s["temperature"],
            "top_p": s["top_p"],
            "top_k": s["top_k"],
            "repeat_penalty": s["repetition_penalty"],
            "seed": s["seed"],
            "cache_prompt": s.get("cache_prompt", True),
            "chat_template_kwargs": {"enable_thinking": False},
        }
        async with self.client.post(self.url("/v1/chat/completions"), json=body) as r:
            if r.status != 200:
                detail = (await r.text())[:1500]
                if "video input is not supported" in detail:
                    detail = "The installed llama.cpp build does not support video input; ZETIC video results are unaffected."
                raise ValueError(detail)
            while True:
                line = await r.content.readline()
                if not line:
                    break
                if not line.startswith(b"data: "):
                    continue
                data = line[6:].strip()
                if data == b"[DONE]":
                    break
                obj = json.loads(data)
                if obj.get("error"):
                    raise ValueError(str(obj["error"]))
                timings = obj.get("timings", timings)
                usage = obj.get("usage", usage)
                for c in obj.get("choices", []):
                    delta = c.get("delta", {}).get("content") or ""
                    if delta:
                        if first is None:
                            first = (time.perf_counter() - start) * 1000
                        text += delta
                        await notify({"event": "text", "delta": delta})
        elapsed = (time.perf_counter() - start) * 1000
        return {
            "event": "done",
            "text": text,
            "total_ms": elapsed,
            "finish": "stop",
            "usage": usage,
            "metrics": {
                "ttft_ms": first or elapsed,
                "tps": timings.get("predicted_per_second", 0),
                "engine_prefill_ms": timings.get("prompt_ms", 0),
                "decode_ms": timings.get("predicted_ms", 0),
            },
        }

    async def run_llama_decision(self, q, notify):
        questions = {}
        for item in q["questions"]:
            obj = {"type": item["type"], "instructions": item["instructions"]}
            if item["type"] == "choice":
                if item.get("multiple"):
                    raise ValueError(
                        "This llama.cpp System One API does not expose multiple-choice selection. Use single-choice or ZETIC."
                    )
                obj["criteria"] = {
                    c["label"]: c.get("description", "") for c in item["criteria"]
                }
            elif item["type"] == "score":
                obj["criteria"] = [c["label"] for c in item["criteria"]]
            questions[item["id"]] = obj
        files = []
        for a in q.get("attachments", []):
            mime = "image/png" if a["type"] == "image" else "audio/wav"
            files.append(
                f"data:{mime};base64,"
                + base64.b64encode(Path(a["path"]).read_bytes()).decode()
            )
        start = time.perf_counter()
        async with self.client.post(
            self.url("/v1/systemone"),
            json={"state": q.get("state"), "questions": questions, "files": files},
        ) as r:
            data = await r.json(content_type=None)
            if r.status != 200 or data.get("error"):
                raise ValueError(str(data))
        ms = (time.perf_counter() - start) * 1000
        answers = []
        for item in q["questions"]:
            a = data["answers"][item["id"]]
            out = dict(item)
            if item["type"] == "noul":
                p = a["noul"]
                out.update(
                    selected=p >= 0.5,
                    probabilities=[
                        {"label": "true", "probability": p},
                        {"label": "false", "probability": 1 - p},
                    ],
                )
            else:
                probabilities = a.get("probabilities", {})
                entries = (
                    list(probabilities.items())
                    if isinstance(probabilities, dict)
                    else [(str(i), p) for i, p in enumerate(probabilities)]
                )
                if item["type"] == "score":
                    entries = [
                        (
                            (
                                item["criteria"][int(k)]["label"]
                                if str(k).isdigit() and int(k) < len(item["criteria"])
                                else str(k)
                            ),
                            p,
                        )
                        for k, p in entries
                    ]
                out.update(
                    selected=a.get("choice", a.get("score")),
                    score=a.get("score"),
                    confidence=a.get("confidence"),
                    probabilities=[
                        {"label": str(k), "probability": p} for k, p in entries
                    ],
                )
            answers.append(out)
        return {
            "event": "done",
            "answers": answers,
            "metrics": {"ttft_ms": ms},
            "total_ms": ms,
            "usage": data.get("usage", {}),
            "score_semantics": "llama.cpp weighted expected level index",
        }

    async def run_llama_tts(self, q, notify, text):
        start = time.perf_counter()
        pcm = bytearray()
        pending = b""
        rate = 24000
        first = None
        body = {
            "input": text,
            "voice": "female",
            "stream": True,
            "response_format": "pcm",
            "seed": q["settings"]["seed"],
            "max_tokens": max(q["settings"]["max_tokens"], 512),
            "top_p": q["settings"]["top_p"],
            "top_k": q["settings"]["top_k"],
            "repeat_penalty": q["settings"]["repetition_penalty"],
        }
        async with self.client.post(self.url("/tts"), json=body) as response:
            if response.status != 200:
                raise ValueError((await response.text())[:1500])
            header = response.headers.get("Content-Type", "")
            if "encoding=float" not in header:
                raise ValueError("Unexpected llama.cpp TTS format: " + header)
            async for chunk in response.content.iter_any():
                pending += chunk
                count = len(pending) // 4
                if not count:
                    continue
                samples = struct.unpack("<" + str(count) + "f", pending[: count * 4])
                pending = pending[count * 4 :]
                data = struct.pack(
                    "<" + str(count) + "h",
                    *(int(max(-1, min(1, v)) * 32767) for v in samples),
                )
                room = max(
                    0, int(q["settings"]["max_audio_seconds"] * rate) * 2 - len(pcm)
                )
                data = data[:room]
                if not data:
                    break
                if first is None:
                    first = (time.perf_counter() - start) * 1000
                pcm.extend(data)
                await notify(
                    {
                        "event": "audio",
                        "pcm": base64.b64encode(data).decode(),
                        "rate": rate,
                    }
                )
                if len(pcm) >= int(q["settings"]["max_audio_seconds"] * rate) * 2:
                    break
        if not pcm:
            raise ValueError("llama.cpp returned no speech")
        name = q["id"] + ".wav"
        (STATE / "media" / name).write_bytes(pcm_wav(pcm, rate))
        seconds = len(pcm) / 2 / rate
        return {
            "event": "done",
            "text": text,
            "audio_url": "/media/" + name,
            "total_ms": (time.perf_counter() - start) * 1000,
            "finish": "stop",
            "metrics": {"first_audio_ms": first, "audio_seconds": seconds},
            "pipeline": "TTS",
        }

    async def run_llama_speech(self, q, notify):
        messages = [m for m in q["messages"] if m["role"] != "system"]
        if (
            len(messages) == 1
            and not messages[0].get("attachments")
            and q["output"] == "audio"
            and not any(m.get("text") for m in q["messages"] if m["role"] == "system")
        ):
            return await self.run_llama_tts(q, notify, messages[0]["text"])
        complex_history = any(
            m["role"] == "assistant"
            or any(a["type"] in ("image", "video") for a in m.get("attachments", []))
            for m in messages
        )
        if not complex_history:
            return await self.run_llama_audio(q, notify)
        # The installed realtime API accepts user text/audio only. Use full chat history for reasoning, then speak its answer.
        await notify(
            {"event": "status", "message": "llama.cpp · chat + speech synthesis…"}
        )
        start = time.perf_counter()

        async def chat_event(ev):
            if q["output"] != "audio":
                await notify(ev)

        result = await self.run_llama_chat(q, chat_event)
        speech = await self.run_llama_tts(q, notify, result["text"])
        speech["pipeline_timings"] = {
            "chat_ms": result["total_ms"],
            "tts_ms": speech["total_ms"],
        }
        speech["total_ms"] = (time.perf_counter() - start) * 1000
        speech["metrics"].update(result["metrics"])
        speech["pipeline"] = "Chat + TTS"
        speech["usage"] = result.get("usage", {})
        return speech

    async def run_llama_audio(self, q, notify):
        start = time.perf_counter()
        text = ""
        pcm = bytearray()
        first_text = None
        first_audio = None
        rate = 24000
        async with self.client.ws_connect(
            self.url("/v1/realtime"), max_msg_size=32 * 1024 * 1024
        ) as ws:
            ev = await ws.receive_json()
            if ev.get("type") == "error":
                raise ValueError(str(ev))
            audio = ev.get("session", {}).get("audio", {})
            rate = audio.get("output", {}).get("format", {}).get("rate", 24000)
            sess = {
                "type": "realtime",
                "output_modalities": (
                    ["audio"] if q["output"] == "audio" else ["text", "audio"]
                ),
                "max_output_tokens": q["settings"]["max_tokens"],
            }
            systems = [m["text"] for m in q["messages"] if m["role"] == "system"]
            sess["instructions"] = "\n".join(systems) or (
                "Perform TTS in American English using a female voice."
                if q["output"] == "audio"
                else "Respond with interleaved text and audio."
            )
            await ws.send_json({"type": "session.update", "session": sess})
            ev = await ws.receive_json()
            if ev.get("type") == "error":
                raise ValueError(str(ev))
            for m in q["messages"]:
                if m["role"] == "system":
                    continue
                for attachment in m.get("attachments", []):
                    if attachment["type"] != "audio":
                        raise ValueError("This realtime path accepts audio only")
                    with wave.open(attachment["path"], "rb") as wav:
                        raw = wav.readframes(wav.getnframes())
                    for i in range(0, len(raw), 3200):
                        await ws.send_json(
                            {
                                "type": "input_audio_buffer.append",
                                "audio": base64.b64encode(raw[i : i + 3200]).decode(),
                            }
                        )
                if m.get("text"):
                    await ws.send_json(
                        {
                            "type": "conversation.item.create",
                            "item": {
                                "type": "message",
                                "role": "user",
                                "content": [{"type": "input_text", "text": m["text"]}],
                            },
                        }
                    )
            await ws.send_json({"type": "response.create"})
            async for packet in ws:
                if packet.type != WSMsgType.TEXT:
                    if packet.type in (WSMsgType.CLOSE, WSMsgType.ERROR):
                        break
                    continue
                ev = json.loads(packet.data)
                kind = ev.get("type", "")
                if kind == "error":
                    raise ValueError(str(ev))
                if kind in (
                    "response.output_text.delta",
                    "response.audio_transcript.delta",
                    "response.output_audio_transcript.delta",
                ):
                    delta = ev.get("delta", "")
                    text += delta
                    if first_text is None:
                        first_text = (time.perf_counter() - start) * 1000
                    await notify({"event": "text", "delta": delta})
                elif kind == "response.output_audio.delta":
                    if first_audio is None:
                        first_audio = (time.perf_counter() - start) * 1000
                    pcm.extend(base64.b64decode(ev["delta"]))
                    await notify({"event": "audio", "pcm": ev["delta"], "rate": rate})
                    if len(pcm) / 2 / rate >= q["settings"]["max_audio_seconds"]:
                        await ws.send_json({"type": "response.cancel"})
                        break
                elif kind == "response.done":
                    break
        elapsed = (time.perf_counter() - start) * 1000
        limit = int(q["settings"]["max_audio_seconds"] * rate) * 2
        pcm = pcm[:limit]
        name = q["id"] + ".wav"
        (STATE / "media" / name).write_bytes(pcm_wav(pcm, rate))
        seconds = len(pcm) / 2 / rate
        if not seconds:
            raise ValueError("llama.cpp returned no audio")
        return {
            "event": "done",
            "text": text,
            "audio_url": "/media/" + name,
            "total_ms": elapsed,
            "finish": "stop",
            "metrics": {
                "ttft_ms": first_text or first_audio or elapsed,
                "first_audio_ms": first_audio or 0,
                "audio_seconds": seconds,
                "realtime_x": seconds / (elapsed / 1000),
            },
        }


SAMPLE_LIST = [
    {
        "id": "cats",
        "title": "Count the cats",
        "subtitle": "Image reasoning",
        "model": "omni",
        "text": "How many cats are in this image? Describe what they are doing.",
        "file": "d1_image/cats.ppm",
        "type": "image",
    },
    {
        "id": "jfk",
        "title": "Listen & understand",
        "subtitle": "Natural speech · 11 seconds",
        "model": "omni",
        "text": "Transcribe this speech.",
        "file": "asr/jfk.wav",
        "type": "audio",
    },
    {
        "id": "summary",
        "title": "Read the long version",
        "subtitle": "Long article · summarization",
        "model": "omni",
        "text": "Summarize the following article in three sentences:\n\n",
        "file": "summary/article.txt",
        "type": "text",
    },
    {
        "id": "tts",
        "title": "Give it a voice",
        "subtitle": "Text to speech",
        "model": "omni",
        "text": "The quick brown fox jumps over the lazy dog.",
        "output": "audio",
    },
    {
        "id": "voice",
        "title": "Talk it through",
        "subtitle": "Speech in, speech out",
        "model": "omni",
        "text": "Briefly explain what the speaker is asking citizens to do.",
        "file": "asr/jfk.wav",
        "type": "audio",
        "output": "both",
    },
    {
        "id": "ticket",
        "title": "Route a ticket",
        "subtitle": "Noul · Choice · Score",
        "model": "d1",
        "file": "d1_text/ticket.txt",
        "type": "text",
        "questions": [
            {
                "id": "refund",
                "type": "noul",
                "instructions": "Is the customer asking for a refund?",
                "criteria": [],
            },
            {
                "id": "team",
                "type": "choice",
                "instructions": "Which team should handle this?",
                "criteria": [
                    {"label": "billing", "description": "Charges, refunds, invoices"},
                    {"label": "shipping", "description": "Delivery and parcels"},
                    {"label": "technical", "description": "App or site faults"},
                ],
            },
            {
                "id": "urgency",
                "type": "score",
                "instructions": "How urgent is this ticket?",
                "criteria": [{"label": "low"}, {"label": "medium"}, {"label": "high"}],
            },
        ],
    },
    {
        "id": "d1image",
        "title": "Make the call",
        "subtitle": "D1 vision · custom options",
        "model": "d1omni",
        "text": "",
        "file": "d1_image/cats.ppm",
        "type": "image",
        "questions": [
            {
                "id": "count",
                "type": "choice",
                "instructions": "How many cats are in this image?",
                "criteria": [
                    {"label": str(i), "description": f"{i} cats"} for i in range(4)
                ],
            }
        ],
    },
    {
        "id": "d1audio",
        "title": "Understand the speaker",
        "subtitle": "D1 audio · decisions",
        "model": "d1omni",
        "text": "",
        "file": "asr/jfk.wav",
        "type": "audio",
        "questions": [
            {
                "id": "speech",
                "type": "noul",
                "instructions": "Is this a formal public speech in English?",
                "criteria": [],
            },
            {
                "id": "topic",
                "type": "choice",
                "instructions": "What is the speaker discussing?",
                "criteria": [
                    {
                        "label": "civic_duty",
                        "description": "What citizens can do for their country",
                    },
                    {"label": "weather", "description": "Weather conditions"},
                    {"label": "sports", "description": "Sports events"},
                ],
            },
        ],
    },
]


class App:
    def __init__(self, config):
        self.config = config
        self.runtime = Runtime(config)

    async def normalize(self, source, kind, name):
        token = uuid.uuid4().hex
        dest = STATE / "media" / token
        if kind == "text":
            text = source.read_text(encoding="utf-8")
            return {"type": "text", "text": text, "name": name}
        if not shutil.which("ffmpeg"):
            raise ValueError("ffmpeg is required for media. Run the setup script.")
        if kind == "image":
            dest = dest.with_suffix(".png")
            await command(
                "ffmpeg",
                "-v",
                "error",
                "-y",
                "-i",
                source,
                "-vf",
                "scale=512:512:force_original_aspect_ratio=decrease,pad=512:512:(ow-iw)/2:(oh-ih)/2:color=white",
                "-frames:v",
                "1",
                dest,
            )
        elif kind == "audio":
            dest = dest.with_suffix(".wav")
            await command(
                "ffmpeg",
                "-v",
                "error",
                "-y",
                "-i",
                source,
                "-ac",
                "1",
                "-ar",
                "16000",
                "-c:a",
                "pcm_s16le",
                dest,
            )
            with wave.open(str(dest), "rb") as w:
                seconds = w.getnframes() / w.getframerate()
            if seconds > 30.01:
                dest.unlink()
                raise ValueError(
                    f"Audio is {seconds:.1f}s; choose a clip up to 30 seconds. It was not truncated."
                )
        elif kind == "video":
            probe = json.loads(
                await command(
                    "ffprobe",
                    "-v",
                    "error",
                    "-show_entries",
                    "format=duration",
                    "-of",
                    "json",
                    source,
                )
            )
            duration = float(probe.get("format", {}).get("duration", 0))
            if not 0 < duration <= 60:
                raise ValueError(
                    "Choose a video up to 60 seconds; it was not truncated"
                )
            dest.mkdir()
            await command(
                "ffmpeg",
                "-v",
                "error",
                "-y",
                "-i",
                source,
                "-vf",
                "fps=2,scale=512:512:force_original_aspect_ratio=decrease,pad=512:512:(ow-iw)/2:(oh-ih)/2:color=white",
                dest / "%04d.png",
            )
            frames = sorted(dest.glob("*.png"))
            if not frames:
                raise ValueError("Video has no frames")
            await command(
                "ffmpeg",
                "-v",
                "error",
                "-y",
                "-framerate",
                "2",
                "-i",
                dest / "%04d.png",
                "-c:v",
                "ffv1",
                "-pix_fmt",
                "bgr0",
                dest / "clip.mkv",
            )
            return {
                "id": token,
                "type": "video",
                "name": name,
                "frames": [str(p) for p in frames],
                "fps": 2,
                "url": "/media/" + token + "/" + frames[0].name,
            }
        else:
            raise ValueError("Unsupported attachment type")
        return {
            "id": dest.name,
            "type": kind,
            "name": name,
            "url": "/media/" + dest.name,
            "path": str(dest),
        }

    def resolve_attachments(self, items):
        out = []
        for a in items:
            token = a.get("id", "")
            if not token or Path(token).name != token:
                raise ValueError("Invalid attachment")
            path = STATE / "media" / token
            if not path.exists():
                raise ValueError("Attachment is missing; please attach it again")
            kind = a.get("type")
            if kind not in ("image", "audio", "video"):
                raise ValueError("Invalid media type")
            item = {
                "id": token,
                "type": kind,
                "path": str(path),
                "name": a.get("name", token),
            }
            if kind == "video":
                item.update(
                    frames=[str(p) for p in sorted(path.glob("*.png"))],
                    fps=2,
                    video_path=str(path / "clip.mkv"),
                )
            out.append(item)
        return out

    def validate(self, q):
        model_id = q.get("model", "omni")
        if model_id not in self.config["models"]:
            raise ValueError("Unknown model")
        model = self.config["models"][model_id]
        s = q.setdefault("settings", {})
        defaults = {
            "context": 2048,
            "max_tokens": 256,
            "temperature": 0.1,
            "top_p": 0.95,
            "top_k": 50,
            "seed": 1,
            "repetition_penalty": 1.1,
            "max_audio_seconds": 30,
            "set_size": 16,
            "calibrate": True,
        }
        for key, value in defaults.items():
            s.setdefault(key, value)
        if not 512 <= int(s["context"]) <= 131072:
            raise ValueError("Context must be between 512 and 131072")
        if not 1 <= int(s["max_tokens"]) < int(s["context"]):
            raise ValueError("Output tokens must fit the context")
        if not 0 < float(s["max_audio_seconds"]) <= 30:
            raise ValueError("Maximum speech duration must be between 0 and 30 seconds")
        output = q.setdefault("output", "text")
        if output not in ("text", "audio", "both"):
            raise ValueError("Invalid output modality")
        if output != "text" and "speech" not in model["capabilities"]:
            raise ValueError("Selected model does not generate speech")
        q["kind"] = model["kind"]
        q["attachments"] = self.resolve_attachments(q.get("attachments", []))
        q["messages"] = q.get("messages", [])
        for m in q["messages"]:
            if m.get("role") not in ("user", "assistant", "system"):
                raise ValueError("Invalid message role")
            m["attachments"] = self.resolve_attachments(m.get("attachments", []))
        attachments = q["attachments"] + [
            a for m in q["messages"] for a in m.get("attachments", [])
        ]
        for a in attachments:
            if a["type"] not in model["capabilities"]:
                raise ValueError(f'{model["name"]} does not support {a["type"]}')
        if model["kind"] == "generation" and not any(
            m.get("text") or m.get("attachments") for m in q["messages"]
        ):
            raise ValueError("Enter a message or attach media")
        if model["kind"] == "decision":
            questions = q.get("questions", [])
            if not questions or len(questions) > 64:
                raise ValueError("Add between 1 and 64 questions")
            ids = set()
            for item in questions:
                if not item.get("instructions", "").strip():
                    raise ValueError("Question text cannot be empty")
                if item.get("id") in ids:
                    raise ValueError("Question IDs must be unique")
                ids.add(item.get("id"))
                if item.get("type") not in ("noul", "choice", "score"):
                    raise ValueError("Invalid question type")
                criteria = item.get("criteria", [])
                if item["type"] != "noul":
                    labels = [c.get("label", "").strip() for c in criteria]
                    if (
                        len(labels) < 2
                        or not all(labels)
                        or len(set(labels)) != len(labels)
                    ):
                        raise ValueError("Add at least two unique option labels")
                    if item["type"] == "score" and len(labels) > 10:
                        raise ValueError("Score supports 2–10 levels")
        return q

    async def run(self, request):
        if self.runtime.lock.locked():
            raise web.HTTPConflict(text="Another request is running")
        q = self.validate(await request.json())
        engines = q.get("engines", ["zetic"])
        if (
            not engines
            or len(engines) > 2
            or len(set(engines)) != len(engines)
            or any(e not in ("zetic", "llama") for e in engines)
        ):
            raise ValueError("Invalid engines")
        response = web.StreamResponse(
            headers={
                "Content-Type": "application/x-ndjson",
                "Cache-Control": "no-store",
            }
        )
        await response.prepare(request)

        async def write(obj):
            await response.write((json.dumps(obj, ensure_ascii=False) + "\n").encode())

        async with self.runtime.lock:
            self.runtime.cancelled = False
            self.runtime.task = asyncio.current_task()
            try:
                for engine in engines:
                    if self.runtime.cancelled:
                        break
                    run = dict(q, id=uuid.uuid4().hex)
                    first_text = None
                    first_audio = None
                    started = None

                    async def emit(obj):
                        nonlocal first_text, first_audio
                        if started is not None:
                            elapsed = (time.perf_counter() - started) * 1000
                            if obj.get("event") == "text" and first_text is None:
                                first_text = elapsed
                            if obj.get("event") == "audio" and first_audio is None:
                                first_audio = elapsed
                        await write(dict(obj, engine=engine, run_id=run["id"]))

                    await emit({"event": "run_start", "model": q["model"]})
                    try:
                        load_ms = await self.runtime.prepare(
                            engine, q["model"], q["settings"], q["output"], emit
                        )
                        if self.runtime.cancelled:
                            await emit({"event": "cancelled"})
                            break
                        await emit({"event": "loaded", "load_ms": load_ms})
                        started = time.perf_counter()
                        if engine == "zetic":
                            result = await self.runtime.run_zetic(run, emit)
                        elif q["kind"] == "decision":
                            result = await self.runtime.run_llama_decision(run, emit)
                        elif q["output"] != "text":
                            result = await self.runtime.run_llama_speech(run, emit)
                        else:
                            result = await self.runtime.run_llama_chat(run, emit)
                        wall_ms = (time.perf_counter() - started) * 1000
                        metrics = result.setdefault("metrics", {})
                        metrics["engine_ttft_ms"] = metrics.get("ttft_ms", 0)
                        metrics["ttft_ms"] = first_text or first_audio or wall_ms
                        if first_audio is not None:
                            metrics["first_audio_ms"] = first_audio
                        if metrics.get("audio_seconds", 0):
                            metrics["realtime_x"] = metrics["audio_seconds"] / (
                                wall_ms / 1000
                            )
                        result.update(
                            load_ms=load_ms,
                            wall_ms=wall_ms,
                            model=q["model"],
                            settings=q["settings"],
                            backend=(
                                "ZETIC · Hexagon"
                                if engine == "zetic"
                                else "llama.cpp · HTP 2-NSP"
                                + (" + CPU media" if q["output"] != "text" else "")
                            ),
                        )
                        await emit(result)
                        with (STATE / "results.jsonl").open("a") as f:
                            f.write(
                                json.dumps(
                                    dict(result, engine=engine, timestamp=time.time())
                                )
                                + "\n"
                            )
                    except asyncio.CancelledError:
                        await self.runtime.release()
                        await emit({"event": "cancelled"})
                        break
                    except Exception as e:
                        if self.runtime.cancelled:
                            await emit({"event": "cancelled"})
                            await self.runtime.release()
                            break
                        await emit({"event": "error", "message": str(e)})
                        await self.runtime.release()
                        if self.runtime.device_fault():
                            break
            except (ConnectionError, asyncio.CancelledError):
                await self.runtime.cancel()
                await self.runtime.release()
            finally:
                self.runtime.task = None
                await write({"event": "complete", "cancelled": self.runtime.cancelled})
        return response

    async def info(self, request):
        models = []
        for key, m in self.config["models"].items():
            models.append(
                {
                    "id": key,
                    "name": m["name"],
                    "kind": m["kind"],
                    "capabilities": m["capabilities"],
                    "available": {
                        "zetic": Path(m["sdk"]).expanduser().exists(),
                        "llama": Path(m["gguf"]).expanduser().is_file(),
                    },
                    "sdk_path": m["sdk"],
                    "gguf_path": m["gguf"],
                }
            )
        return web.json_response(
            {
                "models": models,
                "default_model": self.config.get("default_model", "omni"),
                "samples": [
                    {
                        k: v
                        for k, v in s.items()
                        if k not in ("file", "questions", "text")
                    }
                    for s in SAMPLE_LIST
                ],
                "state_dir": str(STATE),
                "busy": self.runtime.lock.locked(),
                "device_error": self.runtime.device_fault(),
                "device": "Dragonwing IQ9 · QCS9075",
                "reference": json.loads((ROOT / "reference.json").read_text()),
            }
        )

    async def sample(self, request):
        item = next(
            (dict(s) for s in SAMPLE_LIST if s["id"] == request.match_info["id"]), None
        )
        if not item:
            raise web.HTTPNotFound()
        if item.get("file"):
            a = await self.normalize(
                SAMPLES / item["file"], item["type"], Path(item["file"]).name
            )
            if a["type"] == "text":
                item["text"] = item.get("text", "") + a["text"]
            else:
                item["attachments"] = [
                    {k: v for k, v in a.items() if k not in ("path", "frames")}
                ]
        return web.json_response(item)

    async def upload(self, request):
        reader = await request.multipart()
        field = await reader.next()
        if field is None:
            raise ValueError("Choose a file")
        name = Path(field.filename or "attachment").name
        source = STATE / "media" / (uuid.uuid4().hex + Path(name).suffix.lower())
        size = 0
        with source.open("wb") as f:
            while chunk := await field.read_chunk():
                size += len(chunk)
                if size > MAX_UPLOAD:
                    source.unlink()
                    raise ValueError("File exceeds 100 MB")
                f.write(chunk)
        suffix = source.suffix
        kind = (
            "text"
            if suffix in (".txt", ".md", ".json", ".csv")
            else (
                "audio"
                if suffix in (".wav", ".mp3", ".flac", ".ogg", ".m4a")
                else "video" if suffix in (".mp4", ".webm", ".mov", ".mkv") else "image"
            )
        )
        try:
            result = await self.normalize(source, kind, name)
        finally:
            source.unlink(missing_ok=True)
        return web.json_response(
            {k: v for k, v in result.items() if k not in ("path", "frames")}
        )

    async def cancel(self, request):
        await self.runtime.cancel()
        return web.json_response({"ok": True})

    async def unload(self, request):
        if self.runtime.lock.locked():
            raise web.HTTPConflict(text="Stop the current request first")
        await self.runtime.release()
        if self.runtime.device_lock:
            marker = Path(self.runtime.device_lock.name)
            marker.unlink(missing_ok=True)
            self.runtime.device_lock.close()
            self.runtime.device_lock = None
        return web.json_response({"ok": True})

    async def prepare(self, request):
        if self.runtime.lock.locked():
            raise web.HTTPConflict(text="Another request is running")
        q = self.validate(await request.json())
        async with self.runtime.lock:
            self.runtime.cancelled = False

            async def ignore(_):
                pass

            ms = await self.runtime.prepare(
                q.get("engines", ["zetic"])[0],
                q["model"],
                q["settings"],
                q["output"],
                ignore,
            )
        return web.json_response({"ok": True, "load_ms": ms})

    async def startup(self, app):
        # Older llama-server builds close streamed HTTP replies; do not reuse their sockets.
        self.runtime.client = ClientSession(
            timeout=ClientTimeout(total=300), connector=TCPConnector(force_close=True)
        )

    async def cleanup(self, app):
        await self.runtime.cancel()
        await self.runtime.release(delay=False)
        await self.runtime.client.close()
        if self.runtime.device_lock:
            Path(self.runtime.device_lock.name).unlink(missing_ok=True)
            self.runtime.device_lock.close()


@web.middleware
async def errors(request, handler):
    origin = request.headers.get("Origin")
    if (
        request.method == "POST"
        and origin
        and origin not in (f"http://{request.host}", f"https://{request.host}")
    ):
        raise web.HTTPForbidden(text="Requests must originate from this app")
    try:
        return await handler(request)
    except web.HTTPException:
        raise
    except (ValueError, KeyError, TypeError, json.JSONDecodeError) as e:
        return web.json_response({"error": str(e)}, status=400)
    except Exception as e:
        return web.json_response({"error": str(e)}, status=500)


def create_app(config):
    (STATE / "media").mkdir(parents=True, exist_ok=True)
    handler = App(config)
    app = web.Application(client_max_size=MAX_UPLOAD, middlewares=[errors])
    app["gui"] = handler
    app.add_routes(
        [
            web.get("/api/info", handler.info),
            web.get("/api/sample/{id}", handler.sample),
            web.post("/api/upload", handler.upload),
            web.post("/api/run", handler.run),
            web.post("/api/cancel", handler.cancel),
            web.post("/api/prepare", handler.prepare),
            web.post("/api/unload", handler.unload),
            web.get("/", lambda r: web.FileResponse(ROOT / "static/index.html")),
        ]
    )
    app.router.add_static("/static/", ROOT / "static")
    app.router.add_static("/media/", STATE / "media", show_index=False)
    app.on_startup.append(handler.startup)
    app.on_cleanup.append(handler.cleanup)
    return app


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--config", type=Path, default=ROOT / "config.json")
    p.add_argument("--port", type=int)
    p.add_argument("--host")
    args = p.parse_args()
    source = args.config if args.config.exists() else ROOT / "config.example.json"
    config = json.loads(source.read_text())
    web.run_app(
        create_app(config),
        host=args.host or config.get("host", "127.0.0.1"),
        port=args.port or config.get("port", 8765),
    )


if __name__ == "__main__":
    main()
