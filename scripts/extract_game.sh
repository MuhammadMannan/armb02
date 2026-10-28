#!/usr/bin/env bash
# Copies the game executables (and the title update patches) out of the
# player's own Xbox 360 disc image and title update package into private/,
# which git ignores.
#
#   scripts/extract_game.sh <BO2.iso> [title update package, e.g. tu00000001_00000000]
#
# Only needs python3.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <BO2 Xbox 360 ISO> [title update package]" >&2
    exit 1
fi

iso="$1"
tu="${2:-}"
root="$(cd "$(dirname "$0")/.." && pwd)"
private="$root/private"
mkdir -p "$private" "$root/ppc"

for xex in default.xex default_mp.xex; do
    python3 "$root/scripts/xiso.py" extract "$iso" "$xex" "$private/$xex"
done

if [[ -n "$tu" ]]; then
    for xexp in default.xexp default_mp.xexp; do
        python3 "$root/scripts/stfs.py" extract "$tu" "$xexp" "$private/$xexp"
    done
else
    echo "No title update given. Remove patch_file_path from config/bo2_sp.toml or add private/default.xexp later."
fi
