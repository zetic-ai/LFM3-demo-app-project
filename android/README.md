# Android demo

Run the host commands below from the repository root. The app uses separately downloaded Qualcomm model packages and the bundled SDK. Android SDK, NDK 27.0.12077973, JDK 17, the Hugging Face CLI (`hf`), and `adb` are required.

## SDK API reference

This guide covers the three model packages used by the demo examples. The SDK also supports other compatible LFM3 model packages; supported inputs and outputs depend on the selected model and device target.

For SDK usage, see the public headers and their API comments in [`sdk/include/`](sdk/include/): [`zetic_llm.h`](sdk/include/zetic_llm.h) for generation, [`zetic_decision.h`](sdk/include/zetic_decision.h) for D1 decisions, and [`zetic_media.h`](sdk/include/zetic_media.h) for image/audio/video inputs. The Android JNI integration is in [`lfm3_jni.cpp`](app/src/main/cpp/lfm3_jni.cpp).

## Download the models

Request access to [ZETIC-ai/LFM3](https://huggingface.co/ZETIC-ai/LFM3) and wait for approval, then authenticate with the approved account:

```sh
hf auth login
```

Choose the target matching the phone:

| Device | Target |
| --- | --- |
| Galaxy S25 / SM8750 | `v79` |
| Xiaomi / SM8850 | `v81` |

The demo uses these three model packages:

| Package | App examples |
| --- | --- |
| `lfm3-omni-2b` | All six text/image/audio generation examples, including text → text |
| `d1-2b` | D1 ticket decisions from text |
| `d1-omni-2b` | D1 image and audio decisions |

Download all three for the selected phone target:

```sh
TARGET=v79  # Use v81 for SM8850
hf download ZETIC-ai/LFM3 --local-dir models-android \
  --include "qualcomm/lfm3-omni-2b/$TARGET/*" \
            "qualcomm/d1-2b/$TARGET/*" \
            "qualcomm/d1-omni-2b/$TARGET/*"
```

Keep the downloaded directory structure, including each target's `manifest.json` and payload files.

## Build, install, and copy the models

Set `DEVICE_SERIAL` to the serial of the intended phone. Install and open the app once before copying models so its external files directory exists.

```sh
DEVICE_SERIAL=YOUR_DEVICE_SERIAL
(cd android && ./gradlew assembleDebug)
adb -s "$DEVICE_SERIAL" install -r android/app/build/outputs/apk/debug/app-debug.apk
adb -s "$DEVICE_SERIAL" shell am start -n ai.zetic.lfm3demo/.MainActivity
adb -s "$DEVICE_SERIAL" shell mkdir -p /sdcard/Android/data/ai.zetic.lfm3demo/files/models
adb -s "$DEVICE_SERIAL" push models-android/qualcomm /sdcard/Android/data/ai.zetic.lfm3demo/files/models/
```

For example, the S25 manifests must end up at:

```text
files/models/qualcomm/lfm3-omni-2b/v79/manifest.json
files/models/qualcomm/d1-2b/v79/manifest.json
files/models/qualcomm/d1-omni-2b/v79/manifest.json
```

## Run

Reopen **LFM3 zetic runtime benchmark** after copying. The app prepares LFM3 Omni with context 2048. Tap **D1-2B · ticket**, **D1-Omni · image**, or **D1-Omni · audio** to switch to the matching model and run the example. The three **Prepare** buttons let you load a model ahead of time. One model is active at a time; examples using the current model reuse it.

D1 results show the question, selected answer, every option's probability, model load time, and decision time. The image example selects a cat count from 0, 1, 2, and 3. The audio example uses natural speech from the JFK sample and provides a **Play input audio** button. D1 returns decisions rather than generated text or speech, so TPS does not apply.

For an automated D1 run:

```sh
adb -s "$DEVICE_SERIAL" shell am start -n ai.zetic.lfm3demo/.MainActivity --es scenario d1_image
```

Use `d1_text` or `d1_audio` for the other examples. Results are saved in `/sdcard/Android/data/ai.zetic.lfm3demo/files/results/`. If a model is unavailable, check the phone target and directory structure above. S25 D1 app examples have been verified; Xiaomi app retesting is not included in this update.
