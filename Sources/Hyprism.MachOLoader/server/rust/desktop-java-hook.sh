#!/bin/bash
# Test hook (HytaleMobile, branch rust): with ~/HytaleMobile-rust-ref/rust-enabled present, the
# singleplayer server is the Rust one; otherwise this is the normal Java.
REAL="$(cd "$(dirname "$0")" && pwd)/java.real"
if [ -f "$HOME/HytaleMobile-rust-ref/rust-enabled" ] && printf '%s\n' "$@" | grep -q 'HytaleServer.jar'; then
  bind=127.0.0.1:5520; assets=; prev=
  for a in "$@"; do
    case "$a" in
      --bind=*) bind=${a#--bind=} ;;
      --assets=*) assets=${a#--assets=} ;;
      *) case "$prev" in --bind) bind=$a ;; --assets) assets=$a ;; esac ;;
    esac
    prev=$a
  done
  case "$bind" in localhost:*) bind=127.0.0.1:${bind#localhost:} ;; esac
  R=$HOME/HytaleMobile-rust-ref/ref
  LOG=$HOME/HytaleMobile-rust-ref/rust-mac.log
  { echo "=== $(date) cwd=$PWD"; printf '%q ' "$@" | sed -E 's/(token[= ])[^ ]+/\1<redacted>/g'; echo; } >> "$LOG"
  trap 'kill 0' EXIT TERM INT
  "$R/target/release/hytale-server" serve --universe "$PWD/universe" --assets-pack "$assets" \
      --assets-root "$R/Assets/extracted" --world-data "$R/testdata/Default" --bind "$bind" \
      --auth-mode authenticated --workers 4 2>&1 | while IFS= read -r l; do
    echo "$l"; echo "$l" >> "$LOG"
    case "$l" in *"serve listening"*) echo ">> Singleplayer Ready <<"; echo "[ready signal sent]" >> "$LOG" ;; esac
  done
  exit 0
fi
exec "$REAL" "$@"
