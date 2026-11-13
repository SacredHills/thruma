#!/usr/bin/env bash
# Install Thruma.vst3 from the Linux .tar.gz package.
#   ./install.sh            -> per-user install into ~/.vst3 (no root needed)
#   sudo ./install.sh --system -> system-wide install into /usr/lib/vst3
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(find "$HERE" -maxdepth 4 -type d -name 'Thruma.vst3' | head -n1)"
if [[ -z "$SRC" ]]; then echo "Thruma.vst3 not found next to this script" >&2; exit 1; fi

if [[ "${1:-}" == "--system" ]]; then DEST="/usr/lib/vst3"; else DEST="$HOME/.vst3"; fi
mkdir -p "$DEST"
rm -rf "$DEST/Thruma.vst3"
cp -R "$SRC" "$DEST/"
echo "Installed to $DEST/Thruma.vst3 - rescan plug-ins in your DAW."
