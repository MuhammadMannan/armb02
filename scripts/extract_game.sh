#!/usr/bin/env bash
# Copies the game executable (and optionally the title update patch) out of
# the player's own Xbox 360 disc image into private/, which git ignores.
#
#   scripts/extract_game.sh <path/to/BO2.iso> [path/to/default.xexp]
#
# Requires extract-xiso (macOS: `brew install extract-xiso`).

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <BO2 Xbox 360 ISO> [title update default.xexp]" >&2
    exit 1
fi

iso="$1"
xexp="${2:-}"
root="$(cd "$(dirname "$0")/.." && pwd)"
private="$root/private"

if ! command -v extract-xiso >/dev/null 2>&1; then
    echo "extract-xiso not found. Install it with: brew install extract-xiso" >&2
    exit 1
fi

mkdir -p "$private/iso" "$root/ppc"

echo "Extracting $iso into private/iso ..."
extract-xiso -x "$iso" -d "$private/iso"

echo
echo "Executables on the disc:"
find "$private/iso" -maxdepth 2 -iname '*.xex' -print | sed "s|$private/||"

if [[ -f "$private/iso/default.xex" ]]; then
    cp "$private/iso/default.xex" "$private/default.xex"
    echo "Copied default.xex to private/default.xex"
else
    echo "No default.xex at the disc root. Copy the campaign executable to private/default.xex by hand." >&2
fi

if [[ -n "$xexp" ]]; then
    cp "$xexp" "$private/default.xexp"
    echo "Copied title update patch to private/default.xexp"
else
    echo "No title update given. Remove patch_file_path from config/bo2_sp.toml or add private/default.xexp later."
fi
