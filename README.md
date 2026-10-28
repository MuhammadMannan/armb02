# armb02

A native Apple Silicon (ARM64 + Metal) port of the Xbox 360 version of Call of Duty: Black Ops II, built by statically recompiling the game executable with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

**No game data is included or ever will be.** You need your own Xbox 360 disc image, title update and DLC. The recompiled C++ output is derived from the game executable, so it is generated locally into `ppc/` and is ignored by git as well.

## Plan

1. **Repo setup and executable analysis** (in progress): extract the executable, apply the title update, run XenonAnalyse, and fill in the recompiler config.
2. **Recompile and boot to the main menu**: generate C++, then write the runtime (kernel/XAM imports, file system, XMA audio, Xenos to Metal graphics).
3. **Playable campaign from start to finish**: fix crashes mission by mission, unlock 60+ fps and add graphics settings.
4. **Later**: multiplayer, zombies, DLC and online play, including whether Plutonium interop is possible.

## Layout

| Path | What it is |
|-|-|
| `tools/XenonRecomp` | Submodule pinned to a known-good XenonRecomp commit |
| `tools/bo2scan` | Finds the addresses XenonRecomp's config needs |
| `config/bo2_sp.toml` | XenonRecomp config for the campaign executable |
| `scripts/extract_game.sh` | Copies the executable out of your ISO into `private/` |
| `private/` | Your game files (git-ignored) |
| `ppc/` | Generated C++ (git-ignored) |

## Step 1 on macOS

```sh
brew install cmake ninja llvm extract-xiso
git clone --recursive https://github.com/MuhammadMannan/armb02.git && cd armb02

# Build the host tools with Homebrew Clang (18 or later).
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang" \
  -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++"
cmake --build build

# Copy the executable (and the title update's default.xexp, if you have it extracted).
scripts/extract_game.sh ~/Games/BO2.iso ~/Games/TU/default.xexp

# Apply the title update, then find the register save/restore helpers and longjmp/setjmp candidates.
build/tools/bo2scan/bo2scan private/default.xex private/default.xexp private/default_patched.xex

# List the jump tables.
build/tools/XenonRecomp/XenonAnalyse/XenonAnalyse private/default_patched.xex config/bo2_sp_switch_tables.toml

# After pasting the bo2scan addresses into config/bo2_sp.toml, recompile.
build/tools/XenonRecomp/XenonRecomp/XenonRecomp config/bo2_sp.toml tools/XenonRecomp/XenonUtils/ppc_context.h
```

Check every address `bo2scan` reports in a disassembler (Ghidra with the XEX loader, or IDA) before trusting it. XenonAnalyse's jump table detection was written for Sonic Unleashed, so compare its count with the `mtctr r0` count from `bo2scan`. A large gap means BO2 uses patterns it does not recognise yet. Expect a few rounds of fixing jump tables and function boundaries before XenonRecomp runs cleanly.

The title update ships as an STFS package, so `default.xexp` has to be pulled out of it first.

## License

The code in this repository will be released under an open source license once the project is public. XenonRecomp is MIT licensed.
