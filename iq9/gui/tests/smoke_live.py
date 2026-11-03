#!/usr/bin/env python3
"""Exercise real models through the GUI API. Never substitutes mock inference."""

import argparse
import json
import sys
import urllib.request
import wave
import io


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--url", default="http://127.0.0.1:8765")
    p.add_argument("--suite", choices=["core", "speech", "all"], default="all")
    p.add_argument("--engine", choices=["zetic", "llama", "both"], default="both")
    args = p.parse_args()
    engines = ["zetic", "llama"] if args.engine == "both" else [args.engine]

    def get(path):
        return json.load(urllib.request.urlopen(args.url + path, timeout=180))

    def run(name, q):
        q.update(
            engines=engines,
            settings={
                "context": 2048,
                "max_tokens": 128,
                "temperature": 0,
                "seed": 1,
                "max_audio_seconds": 8,
            },
        )
        request = urllib.request.Request(
            args.url + "/api/run",
            json.dumps(q).encode(),
            {"Content-Type": "application/json"},
        )
        results = []
        with urllib.request.urlopen(request, timeout=300) as response:
            for line in response:
                event = json.loads(line)
                if event["event"] == "error":
                    raise RuntimeError(name + ": " + event["message"])
                if event["event"] == "done":
                    results.append(event)
                    print(json.dumps({"test": name, **event}), flush=True)
        assert len(results) == len(engines), (name, "missing engine result")
        for r in results:
            if q["model"].startswith("d1"):
                assert len(r.get("answers", [])) == len(q["questions"])
                for a in r["answers"]:
                    assert (
                        abs(sum(p["probability"] for p in a["probabilities"]) - 1)
                        < 0.02
                    )
            elif q.get("output", "text") == "text":
                assert r.get("text", "").strip(), name + " empty output"
            if q.get("output", "text") != "text":
                audio = urllib.request.urlopen(args.url + r["audio_url"]).read()
                with wave.open(io.BytesIO(audio), "rb") as wav:
                    assert wav.getnframes() > 1000
        return results

    def sample(name, output="text"):
        s = get("/api/sample/" + name)
        return {
            "model": s["model"],
            "output": s.get("output", output),
            "messages": [
                {
                    "role": "user",
                    "text": s.get("text", ""),
                    "attachments": s.get("attachments", []),
                }
            ],
            "state": s.get("text") or None,
            "attachments": s.get("attachments", []),
            "questions": s.get("questions", []),
        }

    if args.suite in ("core", "all"):
        q = {
            "model": "omni",
            "output": "text",
            "messages": [
                {
                    "role": "user",
                    "text": "What is the capital of France? Answer in one sentence.",
                }
            ],
        }
        r = run("text", q)
        assert all("Paris" in a["text"] for a in r)
        followup = {
            "model": "omni",
            "output": "text",
            "messages": q["messages"]
            + [
                {
                    "role": "assistant",
                    "text": r[0]["text"],
                    "generated_id": r[0].get("generated_id", ""),
                },
                {
                    "role": "user",
                    "text": "Which country is that city in? Answer in one sentence.",
                },
            ],
        }
        assert all("France" in a["text"] for a in run("multiturn", followup))
        for name in ("cats", "jfk", "summary", "ticket", "d1image", "d1audio"):
            run(name, sample(name))
    if args.suite in ("speech", "all"):
        run("tts", sample("tts"))
        run("voice", sample("voice"))
        q = sample("cats")
        q["output"] = "both"
        q["messages"][0]["text"] = "Describe this image in one short sentence."
        run("image-speech", q)
    print("PASS " + args.suite, flush=True)


if __name__ == "__main__":
    main()
