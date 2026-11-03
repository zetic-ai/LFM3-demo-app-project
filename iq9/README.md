# IQ9: build and run the CLI

Follow steps 1–5 on the **Dragonwing IQ9 (`v73`)**, in the same Bash session. The native build uses GNU `g++` and `make`. A Hexagon SDK or Qualcomm compiler is not required; the board must have its FastRPC runtime installed.

## 1. Prepare the IQ9

Install Git and the GNU build tools:

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends git g++ make
```

Install the [Hugging Face CLI](https://huggingface.co/docs/huggingface_hub/en/guides/cli) so `hf` is available, then get the project:

```sh
git clone https://github.com/zetic-ai/LFM3-zetic-apps.git
cd LFM3-zetic-apps
```

Already cloned it? Open a terminal at the repository root instead. Run all remaining commands there.

## 2. Build the CLI programs

```sh
make -C iq9 CXX=g++
ls iq9/out/lfm3_cli iq9/out/d1_example
```

**Check:** the build finishes without errors and both executables are listed.

## 3. Download the three demo models

1. Open [ZETIC-ai/LFM3](https://huggingface.co/ZETIC-ai/LFM3).
2. Request access and wait for approval, if you do not already have access.
3. Log in with the approved account and download the IQ9 `v73` packages:

```sh
hf auth login
hf download ZETIC-ai/LFM3 --local-dir models --include "qualcomm/lfm3-omni-2b/v73/*"
hf download ZETIC-ai/LFM3 --local-dir models --include "qualcomm/d1-2b/v73/*"
hf download ZETIC-ai/LFM3 --local-dir models --include "qualcomm/d1-omni-2b/v73/*"
ls models/qualcomm/*/v73/manifest.json
```

**Check:** `ls` lists these three files:

```text
models/qualcomm/lfm3-omni-2b/v73/manifest.json
models/qualcomm/d1-2b/v73/manifest.json
models/qualcomm/d1-omni-2b/v73/manifest.json
```

| Package | Used for |
| --- | --- |
| `lfm3-omni-2b` | Text generation and all six text/image/audio generation samples |
| `d1-2b` | D1 ticket decisions |
| `d1-omni-2b` | D1 image and audio decisions |

Downloading on a separate computer? Copy its complete `models/` directory to the repository root on the IQ9, preserving this structure and all payload files.

## 4. Run the first text example

```sh
./iq9/out/lfm3_cli --model models/qualcomm/lfm3-omni-2b/v73 "What is the capital of France?"
```

**Check:** the answer names **Paris**, followed by a timing line containing **TTFT** and **tokens/s**. Exact wording and timings can vary. You have completed the first run.

## 5. Run the D1 examples

Run one command at a time and wait for it to finish.

### Ticket → decisions

```sh
./iq9/out/d1_example --model models/qualcomm/d1-2b --mode ticket --text-file samples/d1_text/ticket.txt
```

**Check:** the terminal prints the refund question with `true`/`false` probabilities, team choices, and urgency levels. Each question includes a `Selected:` answer. The final line reports decision time and pass count.

### Image → decisions

```sh
./iq9/out/d1_example --model models/qualcomm/d1-omni-2b --mode image --ppm samples/d1_image/cats.ppm
```

**Check:** the cat-count question offers `0`, `1`, `2`, and `3`, with **2** selected for the bundled image. The animal question selects **cats**. All option probabilities are printed.

### Audio → decisions

```sh
./iq9/out/d1_example --model models/qualcomm/d1-omni-2b --mode audio --wav samples/asr/jfk.wav
```

**Check:** the English-speech question selects **true** and the topic question selects **civic_duty** for the natural JFK clip. Probabilities and decision time follow. D1 returns decisions rather than generated speech or text, so it has no tokens/s metric.

## More generation samples

```sh
MODEL=models/qualcomm/lfm3-omni-2b/v73
```

| Run | Expected result |
| --- | --- |
| `./iq9/out/lfm3_cli --model "$MODEL" --sample summary` | Summary of Darwin's introduction |
| `./iq9/out/lfm3_cli --model "$MODEL" --sample image` | Description of the sample image |
| `./iq9/out/lfm3_cli --model "$MODEL" --sample asr` | Transcript of the JFK clip |
| `./iq9/out/lfm3_cli --model "$MODEL" --sample tts` | `sample_tts.wav` |
| `./iq9/out/lfm3_cli --model "$MODEL" --sample vl_audio` | Text and `sample_vl_audio.wav` |
| `./iq9/out/lfm3_cli --model "$MODEL" --sample voice` | Text and `sample_voice.wav` |

Speech WAVs are written to the current directory. The CLI reads media from files and does not capture or play audio/video.

## If a step fails

| Problem | Check |
| --- | --- |
| Hugging Face download returns 401/403 | Confirm access approval and run `hf auth login` with that account. |
| A manifest or payload is missing | Keep the complete model package under the paths in step 3. |
| Shared library cannot be loaded | Keep `iq9/out/` alongside the bundled `iq9/sdk/lib/`. |
| FastRPC or NPU loading fails | Check the board runtime and close any other process using the NPU. |

## Additional CLI options

Run `./iq9/out/lfm3_cli --help` for all options. Common examples:

```sh
# Interactive text conversation
./iq9/out/lfm3_cli --model "$MODEL" -i

# Read an image or WAV directly
./iq9/out/lfm3_cli --model "$MODEL" --image samples/image_count/count.ppm "Describe this image."
./iq9/out/lfm3_cli --model "$MODEL" --audio samples/asr/jfk.wav "Transcribe the audio."

# Write generated speech
./iq9/out/lfm3_cli --model "$MODEL" --tts reply.wav "The quick brown fox jumps over the lazy dog."
```

Images and video frames must be binary PPM (P6), 512 × 512. Audio input is WAV, up to 30 seconds. The interactive commands `/image <file.ppm>`, `/audio <file.wav>`, `/reset`, and `/exit` attach media, reset, or exit. Use `--video-frames <directory> --fps 2` for ordered video frames. The CLI default context is 4096; use `--ctx` to select another supported size.

<details>
<summary>Build on an x86 Linux computer instead</summary>

Install an AArch64 GNU cross compiler, then run from the repository root:

```sh
make -C iq9
```

Copy `iq9/out/`, `iq9/sdk/lib/`, and `samples/` to the IQ9 with the same relative directory structure. Download or copy the `models/` directory as in step 3, then run on the IQ9.

</details>

## SDK API reference

These three packages are the demo examples. The SDK also supports other compatible LFM3 model packages; inputs and outputs depend on the model and device target.

| API | Header |
| --- | --- |
| Generation | [`sdk/include/zetic_llm.h`](sdk/include/zetic_llm.h) |
| D1 decisions | [`sdk/include/zetic_decision.h`](sdk/include/zetic_decision.h) |
| Image/audio/video inputs | [`sdk/include/zetic_media.h`](sdk/include/zetic_media.h) |

See the API comments in those headers and the integration examples in [`main.cpp`](src/main.cpp) and [`d1_example.cpp`](src/d1_example.cpp). Input attribution is in [`samples/README.md`](../samples/README.md).
