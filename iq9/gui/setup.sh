#!/usr/bin/env bash
set -euo pipefail
GUI_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
if ! command -v ffmpeg >/dev/null || ! command -v g++ >/dev/null; then
  echo "Install prerequisites: sudo apt-get install g++ make python3-venv ffmpeg curl"
  exit 1
fi
python3 -m venv "$GUI_DIR/.venv"
"$GUI_DIR/.venv/bin/pip" install --index-url https://pypi.org/simple -r "$GUI_DIR/requirements.txt"
make -C "$GUI_DIR/.." out/sdk_worker CXX=g++
if [[ ! -f "$GUI_DIR/config.json" ]]; then cp "$GUI_DIR/config.example.json" "$GUI_DIR/config.json"; fi
mkdir -p "$HOME/.local/share/applications"
cat > "$HOME/.local/share/applications/lfm3-iq9.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=LFM3 ZETIC playground
Comment=Compare ZETIC and llama.cpp on Dragonwing IQ9
Exec="$GUI_DIR/run.sh"
Icon=$GUI_DIR/static/zetic.png
Terminal=false
Categories=Development;Education;
DESKTOP
echo "Ready. Edit $GUI_DIR/config.json to match your local model paths, then run $GUI_DIR/run.sh"
