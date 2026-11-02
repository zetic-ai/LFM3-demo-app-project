# LFM3 demo

A multimodal SDK sample project with a six-example Android app and a file-based IQ9 CLI. The model is not included. Access to the private `ZETIC-ai/LFM3` Hugging Face repository and a Hugging Face login are required. The bundled SDK and its Qualcomm DSP libraries are subject to their respective licenses.

This demo uses manual model download and `adb push`. A distributed app should download the model separately with authentication, verify its package checksums, and store it in app-specific files; this demo does not implement that downloader.

## Models

Install the Hugging Face CLI and authenticate, then download the package for your device:

```sh
# Galaxy S25 (v79)
hf download ZETIC-ai/LFM3 --include "qualcomm/lfm3-omni-2b/v79/*" --local-dir models
# Galaxy S26 Ultra (v81)
hf download ZETIC-ai/LFM3 --include "qualcomm/lfm3-omni-2b/v81/*" --local-dir models
# IQ9 (v73)
hf download ZETIC-ai/LFM3 --include "qualcomm/lfm3-omni-2b/v73/*" --local-dir models
```

For Android, install the app first and copy the downloaded `qualcomm` directory to its external files directory:

```sh
adb push models/qualcomm /sdcard/Android/data/ai.zetic.lfm3demo/files/models/
```

## Android

With Android SDK, NDK 27.0.12077973 and JDK 17 installed:

```sh
cd android
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Open LFM3 Demo; the app loads Omni with a 2,048-position context. Tap one of six fixed examples: text, image, or audio input, each with text or speech output. The text-to-text example summarizes a long article. Image samples show the input image, and audio samples let you play the input WAV. The app shows the output and its TTFT and TPS. Speech results can be played again; WAV and text results are saved under the app's external `files/results/` directory. The app uses the same bundled sample inputs as the IQ9 CLI.

The SDK's Android CPU libraries and CLI executables and the app JNI library are built for 16 KB page alignment. DSP skeletons are Hexagon binaries and are not loaded as Android CPU libraries. Extracted native libraries are required for FastRPC; the app configures this at install time. If model loading fails, check the selected target, the files and the app logcat output for FastRPC or skeleton-loading errors. Do not run another NPU process simultaneously.

For automated sample runs, launch with `adb shell am start -n ai.zetic.lfm3demo/.MainActivity --es scenario summary` (replace `summary` with `tts`, `image`, `vl_audio`, `asr`, or `voice`; `all` runs all six). Results and speech WAV files are saved under the app's external `files/results/` directory.

## IQ9

With a GNU C++ toolchain installed on the IQ9, the included SDK is sufficient:

```sh
cd iq9
make
./out/lfm3_cli --model ../models/qualcomm/lfm3-omni-2b/v73 "What is the capital of France?"
./out/lfm3_cli --model ../models/qualcomm/lfm3-omni-2b/v73 --sample image
./out/lfm3_cli --model ../models/qualcomm/lfm3-omni-2b/v73 -i
```

Run `./out/lfm3_cli --help` for generation settings and file options. Images and video frames must be P6 PPM files at 512 × 512; audio input is WAV. `--tts out.wav` produces speech and `--voice out.wav` produces text plus speech. Video uses a directory of ordered PPM frames and `--fps`. The CLI does not capture or play media.

## Samples

The app's six examples are `summary` (long text → text), `tts` (text → audio), `image` (image → text), `vl_audio` (image → audio), `asr` (audio → text), and `voice` (audio → audio). The audio-output examples may also return text. The CLI has additional scenarios. Both use the same files in `samples/`; actual wording varies.

| Scenario | Input | Expected result |
| --- | --- | --- |
| `chat` | Three text turns | Related answers using earlier turns |
| `summary` | Long public-domain text | Summary of Darwin's introduction |
| `image` | Synthetic circles | Description of the image |
| `asr` | Kennedy speech clip | Transcript close to the spoken words |
| `tts` | Text | Spoken version in a WAV file |
| `vl_audio` | Synthetic circles and a prompt | Image description in text and speech |
| `voice` | Spoken question | Text and spoken answer naming Paris |

See [samples/README.md](samples/README.md) for file details and attribution. iOS is reserved for a future project.
