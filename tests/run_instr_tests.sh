#!/usr/bin/env bash
# Instruction tests for our XenonRecomp patches (adapted from the Deadpool project's
# tests/run_instr_tests.sh, which follows SpeedBreaker's).
#
#   tests/run_instr_tests.sh [test-name ...]
#
# Assembles tests/instr/*.s with LLVM, recompiles them with XenonRecomp's test mode, builds
# XenonTests natively and runs it. Any line the test binary prints is a failed check.
# Needs LLVM with the PowerPC target and lld (LLVM=<bin dir>, LLD=<ld.lld> to override).
# Run scripts/apply_xenonrecomp_patches.sh first.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -z "${LLVM:-}" ]; then
  if [ -x /opt/homebrew/opt/llvm/bin/llvm-mc ]; then LLVM=/opt/homebrew/opt/llvm/bin
  else LLVM="$(dirname "$(command -v llvm-mc)")"; fi
fi
if [ -z "${LLD:-}" ]; then
  if [ -x "$LLVM/ld.lld" ]; then LLD="$LLVM/ld.lld"
  elif command -v ld.lld > /dev/null; then LLD="$(command -v ld.lld)"
  else LLD=/opt/homebrew/bin/ld.lld; fi
fi
for t in "$LLVM/clang" "$LLVM/llvm-mc" "$LLVM/llvm-objdump" "$LLD"; do
  [ -x "$t" ] || { echo "missing $t"; exit 1; }
done
XR="$ROOT/tools/XenonRecomp"
WORK="$ROOT/build/instr-tests"
rm -rf "$WORK" && mkdir -p "$WORK/bin"

cmake -Wno-deprecated -S "$XR" -B "$XR/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$LLVM/clang" -DCMAKE_CXX_COMPILER="$LLVM/clang++" > /dev/null
ninja -C "$XR/build" XenonRecomp > "$WORK.build.log" 2>&1 || { cat "$WORK.build.log"; echo "XenonRecomp build failed"; exit 1; }

cp "$ROOT"/tests/instr/*.s "$WORK"/
if [ $# -gt 0 ]; then
  for f in "$WORK"/*.s; do
    keep=0; for t in "$@"; do [ "$(basename "$f" .s)" = "$t" ] && keep=1; done
    [ $keep = 1 ] || rm "$f"
  done
fi

for s in "$WORK"/*.s; do
  n=$(basename "$s" .s)
  # LLVM wants %-prefixed register names; the tests use bare ones. `#` starts a comment for LLVM.
  perl -pe 'next if /^\s*#/; s/\b(r|v|f|cr)(\d+)\b/%$1$2/g unless /:\s*$/' "$s" > "$WORK/bin/$n.llvm.s"
  if ! "$LLVM"/llvm-mc -triple=powerpc-unknown-unknown -mcpu=pwr7 -mattr=+altivec \
       -filetype=obj -o "$WORK/bin/$n.obj" "$WORK/bin/$n.llvm.s" 2> "$WORK/bin/$n.asm.log"; then
    echo "ASSEMBLE FAILED: $n ($(head -1 "$WORK/bin/$n.asm.log"))"; rm -f "$s"; continue
  fi
  # XenonUtils' ELF loader wants a linked 32-bit big-endian image at a low address.
  "$LLD" -o "$WORK/bin/$n.o" -e 0 --image-base=0 -Ttext=0x10000 "$WORK/bin/$n.obj"
  "$LLVM"/llvm-objdump -d "$WORK/bin/$n.o" > "$WORK/bin/$n.dis"
done

rm -f "$XR"/XenonTests/*.cpp
"$XR"/build/XenonRecomp/XenonRecomp "$WORK/bin" "$XR/XenonTests" | grep -v "^Recompiling" || true

cmake -Wno-deprecated -S "$XR" -B "$XR/build-tests" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$LLVM/clang" -DCMAKE_CXX_COMPILER="$LLVM/clang++" \
  "-DCMAKE_CXX_FLAGS=-include cstdlib" > /dev/null
ninja -C "$XR/build-tests" XenonTests 2>&1 | grep -E "error|FAILED" | head -20 || true
echo "---- running ----"
"$XR/build-tests/XenonTests/XenonTests" | tee "$WORK/results.txt"
echo "---- $(wc -l < "$WORK/results.txt" | tr -d ' ') failed checks ----"
