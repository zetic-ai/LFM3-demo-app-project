# IQ9 CLI

The CLI runs LFM3 on the Dragonwing IQ9 using the bundled SDK. It uses files for image, audio, and video input and writes generated speech to WAV files. It does not use a camera, microphone, or audio player. Building it needs GNU `g++` and `make`; a Hexagon SDK or Qualcomm compiler is not needed. Running the model still needs an IQ9 with its FastRPC runtime and a v73 model package.

## Build

Run these commands from the repository root. The model is downloaded separately; see the [root README](../README.md#models).

```sh
# On Ubuntu IQ9, if the GNU tools are missing
sudo apt-get update
sudo apt-get install --no-install-recommends g++ make

# On the IQ9 (native GNU C++ toolchain)
make -C iq9 CXX=g++

# Or on an x86 Linux host with the AArch64 GNU cross compiler
make -C iq9
```

The executable is `iq9/out/lfm3_cli`. The Makefile links against `iq9/sdk/lib/` and embeds a relative library search path, so keep `iq9/out/` and `iq9/sdk/lib/` together when copying them to the board. `make -C iq9 clean` removes the executable.

## Run

The examples below assume the repository root is the current directory and the v73 package is under `models/qualcomm/lfm3-omni-2b/v73/` on the IQ9. Download it as shown in the root README, then copy that directory to the IQ9 if it was downloaded elsewhere.

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
