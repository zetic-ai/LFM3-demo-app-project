#!/usr/bin/env bash
set -euo pipefail
GUI_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
PYTHON="$GUI_DIR/.venv/bin/python"
if [[ ! -x "$PYTHON" ]]; then
  echo "Run iq9/gui/setup.sh first." >&2
  exit 1
fi
if [[ "${1:-}" == "--server-only" ]]; then
  shift
  exec "$PYTHON" "$GUI_DIR/server.py" "$@"
fi
GUI_URL=$("$PYTHON" - "$GUI_DIR" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1]);c=p/'config.json';c=c if c.exists() else p/'config.example.json'
print('http://127.0.0.1:'+str(json.loads(c.read_text()).get('port',8765)))
PY
)
if curl -fsS "$GUI_URL/api/info" >/dev/null 2>&1; then
  xdg-open "$GUI_URL"
  exit 0
fi
"$PYTHON" "$GUI_DIR/server.py" "$@" &
GUI_PID=$!
trap 'kill "$GUI_PID" 2>/dev/null || true; wait "$GUI_PID" 2>/dev/null || true' EXIT INT TERM
for i in {1..100}; do
  if curl -fsS "$GUI_URL/api/info" >/dev/null 2>&1; then break; fi
  if ! kill -0 "$GUI_PID" 2>/dev/null; then wait "$GUI_PID"; exit 1; fi
  sleep .1
done
if command -v chromium >/dev/null; then
  chromium --user-data-dir="$HOME/.local/share/lfm3-iq9/chromium" --app="$GUI_URL" --new-window
elif command -v chromium-browser >/dev/null; then
  chromium-browser --user-data-dir="$HOME/.local/share/lfm3-iq9/chromium" --app="$GUI_URL" --new-window
elif command -v firefox >/dev/null; then
  mkdir -p "$HOME/snap/firefox/common/lfm3-iq9-profile"
  firefox --no-remote --profile "$HOME/snap/firefox/common/lfm3-iq9-profile" --new-window "$GUI_URL"
else
  echo "Open $GUI_URL in your browser. Press Ctrl+C to stop the server."
  wait "$GUI_PID"
fi
kill "$GUI_PID" 2>/dev/null || true
wait "$GUI_PID" 2>/dev/null || true
