# IQ9 CLI

The CLI runs LFM3 on the Dragonwing IQ9 using the bundled SDK. It uses files for image, audio, and video input and writes generated speech to WAV files. It does not use a camera, microphone, or audio player. Building it needs GNU `g++` and `make`; a Hexagon SDK or Qualcomm compiler is not needed. Running the model still needs an IQ9 with its FastRPC runtime and a v73 model package.

## SDK API reference

This guide covers the three model packages used by the demo examples. The SDK also supports other compatible LFM3 model packages; supported inputs and outputs depend on the selected model and device target.

For SDK usage, see the public headers and their API comments in [`sdk/include/`](sdk/include/): [`zetic_llm.h`](sdk/include/zetic_llm.h) for generation, [`zetic_decision.h`](sdk/include/zetic_decision.h) for D1 decisions, and [`zetic_media.h`](sdk/include/zetic_media.h) for image/audio/video inputs. CLI integration examples are in [`main.cpp`](src/main.cpp) and [`d1_example.cpp`](src/d1_example.cpp).

## Build

Run these commands from the repository root. Download the models separately as described below.

```sh
# On Ubuntu IQ9, if the GNU tools are missing
sudo apt-get update
sudo apt-get install --no-install-recommends g++ make

# On the IQ9 (native GNU C++ toolchain)
make -C iq9 CXX=g++

# Or on an x86 Linux host with the AArch64 GNU cross compiler
make -C iq9
```

The executables are `iq9/out/lfm3_cli` and `iq9/out/d1_example`. The Makefile links against `iq9/sdk/lib/` and embeds a relative library search path, so keep `iq9/out/` and `iq9/sdk/lib/` together when copying them to the board. `make -C iq9 clean` removes the executables.

## Download the models

The demo uses these three model packages on IQ9:

| Package | CLI examples |
| --- | --- |
| `lfm3-omni-2b` | Text generation and the six text/image/audio generation samples |
| `d1-2b` | D1 ticket decisions from text |
| `d1-omni-2b` | D1 image and audio decisions |

Request access to [ZETIC-ai/LFM3](https://huggingface.co/ZETIC-ai/LFM3) and wait for approval. With the Hugging Face CLI (`hf`) installed, authenticate with the approved account and download the IQ9 v73 packages:

```sh
hf auth login
hf download ZETIC-ai/LFM3 --local-dir models \
  --include "qualcomm/lfm3-omni-2b/v73/*" \
            "qualcomm/d1-2b/v73/*" \
            "qualcomm/d1-omni-2b/v73/*"
```

Run the download from the repository root on the IQ9, or copy the downloaded `models/` directory from the host to the repository root on the IQ9, preserving its structure. Keep the complete packages, including their payload files. The manifests must be at:

```text
models/qualcomm/lfm3-omni-2b/v73/manifest.json
models/qualcomm/d1-2b/v73/manifest.json
models/qualcomm/d1-omni-2b/v73/manifest.json
```

## Run

The examples below assume the repository root is the current directory and the v73 package is under `models/qualcomm/lfm3-omni-2b/v73/` on the IQ9.

```sh
MODEL=models/qualcomm/lfm3-omni-2b/v73

# One text turn and an interactive conversation
./iq9/out/lfm3_cli --model "$MODEL" "What is the capital of France?"
./iq9/out/lfm3_cli --model "$MODEL" -i

# Bundled samples (the same assets used by the Android app)
./iq9/out/lfm3_cli --model "$MODEL" --sample summary
./iq9/out/lfm3_cli --model "$MODEL" --sample image
./iq9/out/lfm3_cli --model "$MODEL" --sample asr
./iq9/out/lfm3_cli --model "$MODEL" --sample tts
./iq9/out/lfm3_cli --model "$MODEL" --sample vl_audio
./iq9/out/lfm3_cli --model "$MODEL" --sample voice

# Files supplied directly
./iq9/out/lfm3_cli --model "$MODEL" --image samples/image_count/count.ppm "Describe this image."
./iq9/out/lfm3_cli --model "$MODEL" --audio samples/asr/jfk.wav "Transcribe the audio."
./iq9/out/lfm3_cli --model "$MODEL" --tts reply.wav "The quick brown fox jumps over the lazy dog."
./iq9/out/lfm3_cli --model "$MODEL" --audio samples/voice/question.wav --voice voice.wav
```

The CLI also has `chat`, `image_count`, and `image_ocr` bundled samples. It prints the output and timings, including TTFT and tokens/s. Speech WAVs from `--tts` and `--voice` are written to the requested path; a speech sample writes `sample_<name>.wav` in the current directory. Images and video frames must be binary PPM (P6), 512 × 512. WAV audio can use any sample rate or channel count, up to 30 seconds.

Use `--video-frames <directory> --fps 2` for ordered PPM frames. In the interactive mode, `/image <file.ppm>` and `/audio <file.wav>` attach media to the next turn; `/reset` starts a new conversation and `/exit` leaves. Other options include `--ctx` (default 4096), `--max-tokens`, `--temperature`, `--top-k`, `--top-p`, `--repetition-penalty`, `--seed`, `--verify`, `--lib-dir`, and `--samples-dir`. Run `./iq9/out/lfm3_cli --help` for the complete option list.

## D1 decisions

The bundled SDK also supports D1 models through `Decision`, which returns calibrated probabilities after one decision pass. After building and downloading the v73 packages above, run from the repository root:

```sh
./iq9/out/d1_example --model models/qualcomm/d1-2b --mode ticket --text-file samples/d1_text/ticket.txt
./iq9/out/d1_example --model models/qualcomm/d1-omni-2b --mode image --ppm samples/d1_image/cats.ppm
./iq9/out/d1_example --model models/qualcomm/d1-omni-2b --mode audio --wav samples/asr/jfk.wav
```

The ticket example asks about refund, routing team and urgency. D1-Omni classifies a two-cat photo and natural speech from the Kennedy inaugural address. The image credit is in `LICENSES/coco-cats.md`. These examples reuse the installed SDK and need no Hexagon SDK compiler.
