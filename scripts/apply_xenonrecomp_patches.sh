#!/usr/bin/env bash
# Applies patches/xenonrecomp/*.patch to the tools/XenonRecomp submodule, in order.
#
#   scripts/apply_xenonrecomp_patches.sh
#
# The submodule is pinned to hedge-dev/XenonRecomp ddd128b. Run this once after
# `git submodule update --init --recursive`; it refuses to run on a submodule that
# has already moved off that commit.

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
xr="$root/tools/XenonRecomp"
base=ddd128bcca99fe8bfbb99bea583c972351fa6ace

if [[ "$(git -C "$xr" rev-parse HEAD)" != "$base" ]]; then
    echo "tools/XenonRecomp is not at $base. Already patched? To start over:" >&2
    echo "  git -C tools/XenonRecomp checkout $base" >&2
    exit 1
fi

git -C "$xr" -c user.name="armb02" -c user.email="armb02@localhost" am --3way --committer-date-is-author-date \
    "$root"/patches/xenonrecomp/*.patch
echo "Applied $(ls "$root"/patches/xenonrecomp/*.patch | wc -l | tr -d ' ') patches to tools/XenonRecomp"
