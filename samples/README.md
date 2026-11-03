Sample scenarios shared by the Android app (copied into the APK assets by Gradle) and the IQ9 CLI (`--sample <name>`).
Each folder has `turns.txt` (one user turn per block, blocks separated by `---`) and `expected.txt`.
turns.txt keys: `out=text|audio|both` (reply modalities, first block), `image=<file>` (512x512 P6 PPM, path relative to the folder),
`audio=<wav>`, `text=<prompt>`, `text_file=<file>` (appended to `text` after a blank line).

The circle and OCR images are synthetic. The Darwin excerpt is public domain (Project Gutenberg #1228); the Kennedy inaugural-address audio is a US government public-domain work. The voice question is synthesized for this demo.

D1 examples: `d1_text/ticket.txt` is a synthetic customer ticket; `d1_image/cats.ppm` is a COCO photo of two cats (credit and CC BY-SA license in `../LICENSES/coco-cats.md`); `d1_audio` uses the existing natural JFK speech `asr/jfk.wav`. Android and `iq9/out/d1_example` exercise the SDK `Decision` API (probabilities and decision time), rather than text or speech generation.
