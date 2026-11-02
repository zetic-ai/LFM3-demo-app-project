Sample scenarios shared by the Android app (copied into the APK assets by Gradle) and the IQ9 CLI (`--sample <name>`).
Each folder has `turns.txt` (one user turn per block, blocks separated by `---`) and `expected.txt`.
turns.txt keys: `out=text|audio|both` (reply modalities, first block), `image=<file>` (512x512 P6 PPM, path relative to the folder),
`audio=<wav>`, `text=<prompt>`, `text_file=<file>` (appended to `text` after a blank line).

The circle and OCR images are synthetic. The Darwin excerpt is public domain (Project Gutenberg #1228); the Kennedy inaugural-address audio is a US government public-domain work. The voice question is synthesized for this demo.
