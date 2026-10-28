# XenonRecomp patches

Applied on top of hedge-dev/XenonRecomp `ddd128b` (the `tools/XenonRecomp` submodule), in order,
by `scripts/apply_xenonrecomp_patches.sh`.

| Patch | From | What |
|---|---|---|
| 0001-0008 | SpeedBreaker | NFS: Most Wanted instructions, arm64 FPCR width, secondary entry points, switch tables indexed with the low 32 bits, memory fences, `vpkd3d128`/`vupkd3d128`/`vcmpbfp`, a 49.875 MHz `mftb` timebase |
| 0009 | deadpool-decomp | Deadpool instructions: `bdzf`/`bdnzt` chains, update-form loads and stores, `addc`/`addme`/`subfze`/`eqv`, `fnmadd`, `frsqrte`, VMX128 packs, `lvehx` |
| 0010 | deadpool-decomp | XEX loader allocates the whole image for basic compression |
| 0011 | armb02 | Black Ops II instructions: `lhbrx`, `ldux`, `stvebx`, `vsubsbs`, `vsubuws`, `vmaxsb`, `vsrb`, `vsl`, `vcmpgtuw(.)`, `vrfip(128)`, `vpkshss128` |

Changes to XenonRecomp go in as new patch files here, never as edits that only live in the
submodule. Every new or changed instruction gets tests (`tests/gen_bo2_instr_tests.py`), and
`tests/run_instr_tests.sh` must report 0 failed checks.

To make a new patch: apply the existing ones, commit your change inside `tools/XenonRecomp`, then
`git -C tools/XenonRecomp format-patch -1 --start-number <next> -o ../../patches/xenonrecomp`.
