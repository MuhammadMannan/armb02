# Third-party notices

armb02 is free software under the GNU General Public License, version 3 or (at your option) any
later version ([`COPYING`](COPYING)). It contains no game data: every player supplies their own
Xbox 360 disc, title update and DLC. The license texts below are in [`LICENSES/`](LICENSES).

| Project | What it does here | License | Text |
|---|---|---|---|
| **XenonRecomp** (and XenonUtils): Copyright (c) 2025 hedge-dev and contributors | Recompiles the game's PowerPC code into C++. A submodule at `tools/XenonRecomp`, built with the patches in `patches/xenonrecomp/`. | MIT | [`LICENSES/XenonRecomp-MIT.md`](LICENSES/XenonRecomp-MIT.md) |
| **SpeedBreaker**: Copyright (C) 2026 project(u) and SpeedBreaker contributors | XenonRecomp patches `0001`-`0008` (instructions, arm64 FPCR, secondary entry points, switch indexing, memory fences, `vpkd3d128`/`vupkd3d128`/`vcmpbfp`, `mftb`). | GPL-3.0-or-later | [`LICENSES/GPL-3.0.txt`](LICENSES/GPL-3.0.txt) |
| **deadpool-decomp** (the author's Deadpool port, built on SpeedBreaker) | XenonRecomp patches `0009`-`0010` and the instruction test harness (`tests/run_instr_tests.sh`, `tests/instr/deadpool_*.s`). | GPL-3.0-or-later | [`LICENSES/GPL-3.0.txt`](LICENSES/GPL-3.0.txt) |
| **Xenia**: Copyright Ben Vanik and Xenia contributors | Reference semantics for several patched instructions. | BSD-3-Clause | [`LICENSES/Xenia-BSD-3-Clause.txt`](LICENSES/Xenia-BSD-3-Clause.txt) |
| **SIMDe**: Copyright (c) 2017 Evan Nemerson and contributors | Vector instructions in the recompiled code (through XenonRecomp). | MIT | [`LICENSES/SIMDe-MIT.txt`](LICENSES/SIMDe-MIT.txt) |
| **libmspack**: (C) 2003-2023 Stuart Caie | LZX decompression of the XEX (through XenonRecomp). | LGPL-2.1 | [`LICENSES/libmspack-LGPL-2.1.txt`](LICENSES/libmspack-LGPL-2.1.txt) |
| **tiny-AES-c** | XEX decryption (through XenonRecomp). | Unlicense | [`LICENSES/tiny-AES-c-Unlicense.txt`](LICENSES/tiny-AES-c-Unlicense.txt) |
| **TinySHA1**: Copyright (c) 2012-22 Saurav Mohapatra | SHA-1 in XenonRecomp's XEX patcher. | ISC-style | [`LICENSES/TinySHA1-ISC.txt`](LICENSES/TinySHA1-ISC.txt) |
| **toml++**: Copyright (c) Mark Gillard | Reads the recompiler config (through XenonRecomp). | MIT | [`LICENSES/tomlplusplus-MIT.txt`](LICENSES/tomlplusplus-MIT.txt) |
