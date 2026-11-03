# LFM3 zetic runtime benchmark

**Installing and running the demo? Start with the [step-by-step quick start](QUICKSTART.md).** The sections below are the technical reference.

A simple iOS SwiftUI demo with six example buttons, actual input, model output, text TTFT/decode tok/s, and generated-audio playback. Each example automatically chooses its designated optimized model. There is no model picker, import screen, input-length panel or measurement-details UI. Models and credentials are supplied separately; no llama.cpp runtime or comparative speed claim is embedded.

## Models and download

The optimized Core ML/Metal bundles are published in the gated [ZETIC-ai/LFM3](https://huggingface.co/ZETIC-ai/LFM3) repository, pinned here to revision **`24a56588cb8a84e25c821217a02ddc437ed7a30c`**. Before downloading:

1. Sign in to Hugging Face, open [ZETIC-ai/LFM3](https://huggingface.co/ZETIC-ai/LFM3), and select **Request access**.
2. Wait until the repository owner grants access. If access has already been granted, this step is unnecessary.
3. Install the [Hugging Face CLI](https://huggingface.co/docs/huggingface_hub/guides/cli) and the Python package in the environment used below. Sign in using the account that has access:

   ```sh
   python3 -m pip install --upgrade huggingface_hub
   hf auth login
   ```

Then download the models into a new directory:

```sh
python3 demo-ios/tools/download_models.py --output /absolute/new/model-download
```

The helper checks each publisher-pinned manifest fingerprint, paths, inventory, per-file size/SHA-256 and required model-family provenance, and downloads each model's README and original LICENSE alongside its `ios/` directory. Credentials use HF's environment/cache, never command arguments or logs. Existing model directories are never overwritten. Failed copies retain `.download-incomplete`; individually completed models remain available. Use `--model text|audio|vl|omni` to select one model, but the build-and-run helper requires all four. If the bundles are already present without the release documents, use `--docs-only --output /absolute/existing/model-download` to fetch the missing documents without changing model files. This documents-only option checks the manifest and documents but does not revalidate the payload files; the build-and-run helper verifies the payload files again. The lower-level `download_model.py` also accepts `--kind` and explicit release coordinates. These integrity/inventory checks do not compile Core ML or establish all SDK JSON semantics; the app uses the SDK verifier before execution. A package digest is not a publisher signature.

The build-and-run helper below copies the downloaded `apple` hierarchy to the app's Documents directory. Expected locations and trusted package IDs are in [ModelConfiguration.json](Resources/ModelConfiguration.json):

| Example | Model | Documents-relative directory |
|---|---|---|
| `summary` | LFM3-1.7B | `apple/lfm3-1.7b/ios` |
| `asr` | LFM3-Audio-2B | `apple/lfm3-audio-2b/ios` |
| `image` | LFM3-VL-2B | `apple/lfm3-vl-2b/ios` |
| `tts`, `vl_audio`, `voice` | LFM3-Omni-2B | `apple/lfm3-omni-2b/ios` |

The previous `Documents/OmniModel` directory is an explicit Omni-only fallback with the same trusted fingerprint. A missing or invalid required model produces an error; another model is never substituted. Original downloaded directories are immutable during use and are never deleted by input/audio cleanup. Only one selected model/session runs at a time. The download-and-run helpers target the pinned release above. A custom release requires separate verified model files and `-DDEMO_MODEL_CONFIG=/absolute/release.json`; do not copy a fingerprint from untrusted model files and call it authentication.

## Build and run on iPhone

The repository includes the iOS SDK. With Xcode, CMake 3.24+, Python 3.10+, an unlocked connected iPhone, and an Apple development team configured for signing, run:

```sh
python3 demo-ios/tools/run_device.py \
  --models /absolute/new/model-download \
  --build-dir /absolute/new/app-build \
  --device YOUR_IPHONE_DEVICE_ID \
  --team YOUR_APPLE_TEAM_ID \
  --bundle-id YOUR_NEW_APP_BUNDLE_ID
```

Use `xcrun devicectl list devices` to find the device ID, and choose a bundle ID that your team can sign. The helper verifies all four downloaded bundles and the bundled SDK checksum, builds a signed iOS Release app, installs it, copies the model files to its Documents directory and launches it. For data safety, it refuses a bundle ID that is already installed, an existing build directory, and an existing `Documents/apple` destination; it does not delete or overwrite app/model data. A failed build or copy leaves its files for inspection. Unlock the phone and enable Developer Mode as required by Xcode. Select a sample after the app opens; each model is verified again by the SDK when selected.

To use a compatible SDK installation, optionally pass `--sdk-dir /absolute/sdk-work/install/lib/cmake/LFM3IOS` to the helper. The bundled SDK and its notices are provided separately from the model files; include the relevant model README/LICENSE files when distributing the models.

The bundled SDK targets iPhone arm64. The app requires iOS 18 or later. No signing identity, provisioning profile or model weights are committed. Include installed SDK third-party notices and the model release's README/LICENSE files in distribution. The package is not an XCFramework or simulator SDK.

A separate `-DDEMO_UI_PREVIEW=ON` target with `-DCMAKE_OSX_SYSROOT=iphonesimulator` links an inference-disabled stub and produces no fake metrics. It must not be distributed as the inference demo. Simulator compilation does not establish screen rendering or inference.

## Actual input tasks and limits

Each preset uses a real, sentence-complete Darwin introduction excerpt as the material to summarize or answer from. Images and the spoken question affect the required answer. There is no unrelated system background or repeated padding. The actual selected document is visible in the input area.

| CLI preset | Actual task | Model | Text/template tokens | Full prompt / prefill rows |
|---|---|---|---:|---:|
| `summary` | Three-sentence document summary; thinking explicitly disabled. | Text | 975 | 975 / 975 |
| `tts` | One-sentence spoken summary, at most 12 words. | Omni | 983 | 983 / 982 |
| `image` | Count the circles, then summarize the document in that many sentences. | VL | 754 | 1012 / 1012 |
| `vl_audio` | Use the circle count to determine spoken-summary length. | Omni | 1006 | 1264 / 1263 |
| `asr` | Answer the document-related spoken question in text. | Audio | 1010 | 1335 / 1335 |
| `voice` | Give a short spoken answer to that question using the document. | Omni | 1003 | 1330 / 1329 |

These are **host tokenizer/metadata checks**, not performance results. Hidden CLI reports retain actual runtime row information. The historical `tts`/`asr` scenario IDs remain for compatibility: their current tasks are spoken summarization and spoken question answering, not full-document TTS or verbatim transcription. Audio outputs use Omni Interleaved mode and expose both the text and corresponding speech.

Every model uses its own tokenizer/template. Text uses the public SDK chat renderer followed by `<think></think>` to request a final answer without a reasoning preamble. Audio uses the Audio prompt contract and a concise document-question instruction. VL uses the public image template with a short two-step task after the document. Both image tasks put the request after the source material. Omni uses its verified multimodal template. Question audio is never transcribed into the text prompt.

Text plus template is capped at 1,024 tokens. The **entire** prefill cap is model-specific: Text 1,024; VL 1,024 including the 256 image features and two boundary tokens; Audio and Omni 1,536. Thus VL's text cannot also occupy 1,024 tokens. The builder uses real exported input shapes/capacities, retaining the longest complete source prefix that fits both limits. Total context is 2,048 in these bundles. Non-speech requests allow up to 256 generation steps. Omni speech allows up to **min(512, remaining context)** steps, shared by interleaved text and audio. Natural EOS, output-budget termination and context termination remain distinct. A full 1,000-token spoken reading is not promised within that output budget.

Samples and SHA-256 records are in [manifest.json](Resources/Samples/manifest.json). Darwin and the synthetic counting image originate in the Android examples; the PNG preserves original PPM pixels exactly. [DarwinQuestion-source.txt](Resources/Samples/DarwinQuestion-source.txt) describes the locally synthesized 26.007-second, mono 16 kHz document question, containing no answer. Its audio contributes 325 features. Legacy Android sample attribution remains in [SOURCE-README.md](Resources/Samples/SOURCE-README.md). The ZETIC icon uses the Android project's official vector.

## Timing and cancellation

Text, Audio and VL use their public SDK sessions. Their **TTFT starts after `session.open` completes**, then includes generate-time tokenization/setup, encoding, prefill and the first actual token selection. Audio/VL load their modules in open; the SDK does not expose separate load/setup observers for these APIs. Reports therefore retain the whole measured open interval, without pretending it is a model-load-only duration. Text tokenizer loading inside generation remains included. VL performs source-image decode/resize once inside `generateImage`, included in TTFT. Preparation hashes the original image file with a bounded streaming reader, without an additional inference RGB decode.

Omni verification/open occurs before its request timer. Its TTFT/first PCM subtracts the merged union of explicit staged model-load intervals clipped to the first event. Encoding and setup inside the request remain included. This staged-load subtraction differs from the Text/Audio/VL open-excluded boundary; cross-model numbers must retain these definitions. Run wall includes the measured open interval. UI input preparation and earlier model-release validation are outside the run timer.

Text decode tok/s is `(selected_count − 1)/(last_selection − first_selection)` using actual selected-token timestamps, including an EOS selection when reported. Speech APIs expose audio-frame callbacks but no selected-text-token timing: text tok/s is unavailable. First PCM uses the first nonempty callback. Audio realtime multiplier is generated duration divided by load-adjusted processing time through the last PCM callback; audio frames are never relabeled tokens.

Work runs off the UI thread on one serial worker. Stop immediately sets cancellation, but active model verification, encoding, Core ML calls or prefill may finish before a checkpoint. Audio/VL cancellation is observed through their output callback because these public APIs lack an independent cancellation hook. Terminal callbacks reopen admission before publishing, preventing immediate-next-request races. Replaced app-owned temporary inputs and WAVs are cleaned without deleting original media/model files. WAV output directories are recreated/checked immediately before writing because Caches can be purged.

## CLI scenario reports and validation

`--scenario summary|tts|image|vl_audio|asr|voice` follows the exact same model verification → sample → Prepare → Run methods as the buttons. No argument means no automatic inference. Each process launch runs one selected example; terminate/relaunch to choose another. Only this fixed-sample mode writes `Documents/LatestScenario.json`, atomically, with model kind/package ID, input hash, hidden capacities/rows, real output, metrics, timing boundary and terminal status/error. No report is uploaded. For VL, the input fingerprint includes original source-file SHA; other image paths fingerprint decoded RGB. Generated WAV paths refer only to the app-owned cache.

Host validation covers trusted model routing/invalid IDs, exact historical fixtures, actual preset strings, model-specific tokenizer/template/capacity, thinking-off suffix, task placement, bounded source-image SHA, bounded audio conversion/copy, atomic model metadata, downloader/provenance/partial bundles, cache purge/recreation and 1,000 concurrent completion→immediate-start handoffs. See [Tests/README.md](Tests/README.md). Release build/link and signing validation do not establish response quality or device speed; those require the separate physical-device results. No inference output is fabricated by host tests.
