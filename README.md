# LFM3 on ZETIC Engine

Run text, image, audio, and D1 decision examples with the bundled SDK.

**Choose your platform and follow its numbered setup guide.** Models are downloaded separately; request access to [ZETIC-ai/LFM3](https://huggingface.co/ZETIC-ai/LFM3) before downloading.

## Android

**[Start the Android guide →](android/README.md)**

Default download example: **Galaxy S26 Ultra (`v81`)**. The guide also lists the target for Galaxy S25 and Xiaomi.

1. Prepare your computer and connect the phone.
2. Build and install the app.
3. Download the three demo models and copy them to the phone.
4. Tap a sample and check its output and timings.

**Finished when:** text appears under **Output** with **TTFT / TPS**, or a D1 example shows a selected answer and option probabilities. [See example screens](android/README.md#5-run-the-first-example).

## IQ9

**[Start the IQ9 guide →](iq9/README.md)**

Target: **Dragonwing IQ9 (`v73`)**. Build with GNU `g++` and `make`.

1. Prepare the IQ9 and build the two CLI programs.
2. Download the three demo models.
3. Run a text sample, then the D1 examples.

**Finished when:** the terminal prints an answer and timings, or a D1 selected answer and option probabilities.

## iOS

Follow the existing [iOS setup guide](demo-ios/README.md).

## Models used by the Android and IQ9 examples

| Model package | Examples |
| --- | --- |
| `lfm3-omni-2b` | Six generation examples: text, image, or audio input → text or audio output |
| `d1-2b` | Ticket decisions from text |
| `d1-omni-2b` | Cat-count decisions from an image; topic decisions from natural speech |

The SDK supports additional compatible model packages. For API usage, see the SDK header links at the end of the [Android](android/README.md#sdk-api-reference) or [IQ9](iq9/README.md#sdk-api-reference) guide.

Sample inputs and attribution: [samples/README.md](samples/README.md). SDKs and dependencies are subject to their respective licenses; see [LICENSES/](LICENSES/).
