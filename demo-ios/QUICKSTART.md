# iOS: install and run the demo

Follow steps 1–5 on a **Mac** to run the demo on a connected iPhone. Use the same terminal session throughout. Detailed model, timing, and validation notes are in the [technical reference](README.md).

## 1. Prepare the Mac and iPhone

| Requirement | Check before continuing |
| --- | --- |
| Xcode | Installed, with an Apple development account/team configured for signing |
| CMake | Version 3.24 or later (`cmake --version`) |
| Python | Version 3.10 or later (`python3 --version`) |
| iPhone | iOS 18 or later, connected by USB, unlocked, and Developer Mode enabled |

Accept the iPhone's trust prompt when connecting it to the Mac.

## 2. Get the project and sign in to Hugging Face

```sh
git clone https://github.com/zetic-ai/LFM3-zetic-apps.git
cd LFM3-zetic-apps
```

Already cloned it? Open a terminal at the repository root instead.

Install the downloader in a Python environment:

```sh
python3 -m venv "$HOME/.venvs/lfm3-demo"
source "$HOME/.venvs/lfm3-demo/bin/activate"
python3 -m pip install --upgrade huggingface_hub
```

1. Open [ZETIC-ai/LFM3](https://huggingface.co/ZETIC-ai/LFM3).
2. Request access and wait for approval, if you do not already have access.
3. Sign in with the approved account:

```sh
hf auth login
```

**Check:** login succeeds using the account with model access.

## 3. Download the four iOS model bundles

Choose a new download directory and run the existing helper:

```sh
IOS_MODELS_DIR="$PWD/models/ios-quickstart"
python3 demo-ios/tools/download_models.py --output "$IOS_MODELS_DIR"
ls "$IOS_MODELS_DIR"/apple/*/ios/bundle_manifest.json
```

**Check:** the helper finishes successfully, and `ls` lists one manifest for each bundle:

| Bundle | App examples |
| --- | --- |
| `apple/lfm3-1.7b/ios` | Text → text |
| `apple/lfm3-audio-2b/ios` | Audio → text |
| `apple/lfm3-vl-2b/ios` | Image → text |
| `apple/lfm3-omni-2b/ios` | Text → audio, Image → audio, Audio → audio |

Keep all four complete bundles and their downloaded README/LICENSE files. The build-and-run helper requires all four. If you already have this complete download, set `IOS_MODELS_DIR` to its absolute path and continue to step 4.

## 4. Build, install, copy the models, and launch

Find the connected iPhone's identifier:

```sh
xcrun devicectl list devices
```

Fill in the three values below. Use your Apple development team ID and a bundle ID that your team can sign and that is not already installed on this iPhone.

```sh
IOS_DEVICE_ID=YOUR_IPHONE_DEVICE_ID
IOS_TEAM_ID=YOUR_APPLE_TEAM_ID
IOS_BUNDLE_ID=com.yourcompany.lfm3demo
IOS_BUILD_DIR="$PWD/build/ios-quickstart"
```

Run the existing setup helper with a new build directory:

```sh
python3 demo-ios/tools/run_device.py \
  --models "$IOS_MODELS_DIR" \
  --build-dir "$IOS_BUILD_DIR" \
  --device "$IOS_DEVICE_ID" \
  --team "$IOS_TEAM_ID" \
  --bundle-id "$IOS_BUNDLE_ID"
```

**Check:** the helper prints `App launched. Select an example; the app verifies the chosen model before inference.` and **LFM3 zetic runtime benchmark** opens on the iPhone. The helper performs model verification, signing, installation, and model copying for you.

## 5. Run an example and check the result

1. Tap **Text → text**.
2. Wait for the example to finish preparing and running.
3. Scroll to **Output**.

**Check:** a summary of the Darwin excerpt appears, together with **TTFT** and **decode tok/s**. You have completed the first run.

Try the other buttons when ready:

| Button | Expected result |
| --- | --- |
| Text → audio | A short spoken summary of the reference text |
| Image → text | A text summary whose sentence count follows the circles in the input image |
| Image → audio | A spoken summary whose length follows the circle count |
| Audio → text | A text answer to the spoken question about the reference text |
| Audio → audio | A spoken answer to that question |

The app chooses each sample's model automatically. For speech output, use **Play audio** and check **First PCM** and the audio realtime metric. Speech output does not have a decode tok/s measurement. Exact wording and timings can vary.

## If a step fails

| Problem | Next action |
| --- | --- |
| Hugging Face access error | Confirm repository approval and repeat `hf auth login` with the approved account. |
| Download is incomplete | See [download and recovery options](README.md#models-and-download). Keep the complete four-bundle layout from step 3. |
| Build directory already exists | Set `IOS_BUILD_DIR` to a new directory and repeat step 4. |
| Bundle ID is already installed | Use a different bundle ID your team can sign, then repeat step 4 with a new build directory. |
| Device/signing error | Unlock the phone, check Developer Mode and your Xcode development team, and verify the device identifier. |

For model details, custom SDK configuration, and measurement definitions, continue to the [technical reference](README.md).
