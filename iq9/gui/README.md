# IQ9 comparison playground

A local Ubuntu desktop chatbot for **ZETIC Hexagon** and **llama.cpp**. It runs on the Dragonwing IQ9; the browser is the GUI, Python coordinates model ownership, and a persistent C++ worker calls the bundled SDK. No camera, microphone, cloud service, Node.js runtime, or custom Wayland configuration is required.

## Install and launch

Copy the repository to the IQ9. Keep `iq9/sdk/` and `samples/` with it. On Ubuntu 24.04 AArch64:

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends g++ make python3-venv ffmpeg curl firefox
./iq9/gui/setup.sh
```

Edit `iq9/gui/config.json` to point to your **existing** model packages, GGUF files, multimodal projectors, and llama-server directories. `config.example.json` shows all six model profiles. The example paths describe the development IQ9; change them to match your installation. A model's GGUF and projector must come from the same model family as its SDK package.

```sh
./iq9/gui/run.sh
```

The **LFM3 ZETIC playground** desktop entry launches the same script. The app opens in a separate browser profile. Closing that browser instance stops the app server, unloads models, and releases its board lock. The normal browser profile is preserved. With an already running app, a second launch opens its existing server.

For development or SSH access, start only the server:

```sh
./iq9/gui/run.sh --server-only
# On your workstation; replace ubuntu@IQ9 with your SSH destination:
ssh -N -L 8765:127.0.0.1:8765 ubuntu@IQ9
# Open http://127.0.0.1:8765 in your workstation browser.
```

The server binds to localhost by default. Its data, saved audio, and runtime logs are in `~/.local/share/lfm3-iq9/`. Browser conversations and generation settings are stored locally; use the sidebar export/import buttons to save a conversation as JSON. Exported JSON references local media, so keep the app data directory when moving conversations.

## Models and engines

| Model | Input | Output |
| --- | --- | --- |
| LFM3 Omni 2B | Text, image, audio, video | Text, speech, text + speech |
| LFM3 1.7B | Text | Text |
| LFM3 VL 2B | Text, image, video | Text |
| LFM3 Audio 2B | Text, audio | Text, speech, text + speech |
| D1 2B | Text/JSON, image | Typed decisions and probabilities |
| D1 Omni 2B | Text/JSON, image, audio | Typed decisions and probabilities |

These are selectable profiles, not a requirement to download every model. For the main demo, provide LFM3 Omni, D1, and D1 Omni. The SDK packages are available from the gated [ZETIC-ai/LFM3 repository](https://huggingface.co/ZETIC-ai/LFM3); use **v73** for IQ9. See the [IQ9 model download guide](../README.md#download-the-models). GGUF files and matching projectors are supplied separately for llama.cpp.

The default selection is **ZETIC + LFM3 Omni**, context **2048**. Send a message to load it, or use **Prepare model** ahead of time. Changing model or engine switches automatically on the next run. Only one NPU model runs at a time. Model loading is shown separately from inference.

llama.cpp uses the validated IQ9 **HTP 2-NSP** device group. The Omni profile requires a build that supports LFM3 Omni (the development board uses b12004); the other development profiles use b11946. Speech generation keeps the LM on HTP and the speech projector on CPU because those builds do not support the speech decoder/codec on HTP. Keep each build's shared libraries beside its server executable. No Hexagon SDK is needed to build the GUI's GNU C++ worker.

D1 exposes **Noul**, **Choice**, and **Score**, editable question text and criteria, multiple questions, and per-option probabilities. Choice multiple selection and the set-size/calibration controls use SDK features; the installed llama.cpp System One API has no native multiple-selection switch and reports that limitation explicitly. ZETIC Score returns a median level, while llama.cpp returns an expected level index; raw results and probabilities are retained in each response's details.

## Chat, media, and samples

- Write arbitrary messages, attach files, or choose a sample. Multi-turn history is sent with each request. Use a new conversation to clear it, and **Regenerate** to repeat the last turn.
- Samples include a COCO cat photo, natural JFK speech, a long Darwin excerpt, ticket routing, text-to-speech, and voice replies. The image credit is in [coco-cats.md](../../LICENSES/coco-cats.md); the speech and article have the same provenance as the repository's existing samples.
- Images are resized and letterboxed to 512×512 once before either engine sees them. Audio is converted to 16 kHz mono PCM16; clips over 30 seconds are rejected with their duration, not silently shortened. Text attachments accept UTF-8 `.txt`, `.md`, `.json`, and `.csv`.
- Video attachments up to 60 seconds are sampled at 2 fps and letterboxed to 512×512. ZETIC receives those frames through its video API; llama.cpp receives a lossless video made from the same frames through its native video input. Its build must include video support. Video soundtracks are not included; attach an audio file separately if needed.
- Each uploaded file is limited to 100 MB. If input and reserved output exceed context, the app shows an error and lets you increase context or start a new conversation. It does not silently drop old turns.
- Input and output audio have playback controls. Generated audio can play as it arrives, be replayed, or downloaded as WAV. **Stop** cancels generation and immediately stops playback.
- The installed llama.cpp realtime API accepts user text and audio but cannot import arbitrary assistant history or images. For those speech requests, the adapter uses full-history chat followed by TTS on the same selected model. This **Chat + TTS** pipeline is recorded in result details; single-turn voice replies use native realtime. ZETIC supports native interleaved responses.

## Compare and read the graph

**Compare** freezes one conversation snapshot and runs it sequentially on both engines, avoiding concurrent contention. Results appear side by side. Choose **Continue with this** to select which answer becomes the next turn's shared history; ZETIC is selected by default after a successful pair.

The sidebar begins with explicitly labeled **IQ9 recorded reference** results from the 2026-10-31 Omni report. The JSON includes the original measurement ranges' midpoint, device, backend configuration, input/output conditions, source, and historical caveats. These recorded module measurements are not claimed to be a measurement of the current SDK or conversation.

A completed comparison adds **live** first-response, decode throughput, first-audio, or realtime-speed graphs when both engines provide that metric. First response is measured in the app server from inference dispatch to the first text/audio event; D1 measures dispatch to completed decisions. Both include local adapter/transport overhead, and both exclude loading and the engine-switch wait. Engine-native timings are also retained. Decode throughput is the backend's native decode metric; it is not inferred from character count. A result can be slower, and the graph preserves that result.

The app releases the previous model process and waits five seconds before opening another NPU session, following the board's existing stability rule. This wait is displayed as switching time and excluded from inference metrics. If a remote processor is crashed, inference stops with a recovery message. The app never reboots the board.

## SDK and development

See the public API comments in [`../sdk/include/`](../sdk/include/), particularly `zetic_llm.h`, `zetic_media.h`, and `zetic_decision.h`. The worker uses the existing SDK binary without changing its version or model payloads. Its JSON protocol is internal to this GUI; existing CLI usage is unchanged.

```sh
make -C iq9 out/sdk_worker CXX=g++
python3 -m py_compile iq9/gui/server.py
# Run against real IQ9 models, with the GUI server already running:
python3 iq9/gui/tests/smoke_live.py --suite core
python3 iq9/gui/tests/smoke_live.py --suite speech
```

The vendored JSON header is nlohmann/json 3.12.0 under the [MIT license](../../LICENSES/nlohmann-json-LICENSE.MIT). The ZETIC logo is reused from the referenced ZETIC demo design. Frontend assets are bundled and require no external CDN.

## Verification status (2026-11-03)

On the development IQ9, both engines passed text chat, a follow-up turn, image understanding, natural-speech transcription, long-text summarization, D1 ticket questions covering all three types, D1 image questions, and D1 audio questions. GNU builds and five model-independent API tests passed. Browser checks covered sample previews, editable questions/options, settings, restored comparison graphs, and controls within a 1024×768 viewport.

While moving on to speech-output testing, IQ9 remoteproc0 entered `crashed` state with a kernel SMMU fault; subsequent SDK session opens returned `0x72`. NPU testing was stopped. The app detects that state and does not retry another engine. Reboot approval is pending, so final **ZETIC speech output, video inference, and cancellation under real inference** remain to be verified. Separately, CPU-only llama.cpp protocol checks passed TTS, natural-audio voice replies, and image-chat followed by TTS; these CPU runs are excluded from the GUI's recorded comparison graph.
