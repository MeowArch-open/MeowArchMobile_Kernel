# Checkpoint validation

## Source identity

- Pristine upstream commit: `72d3fcf802c45d00b300f25b848a93c3a2bd7c7e`.
- Pristine upstream tree: `b9e9f22cee46d79f64ee1d888a80ca0d04dd8df6`.
- Archived patched commit: `13c62c8115f60347320b2faa543a9838f0b63de4`.
- Archived patched tree: `1871f732bbdf403a72a9c36a47f4d7dbb07e17f3`.
- All five recovered files matched the installed MSM module's DWARF source
  checksums; expected/recovered MD5 and SHA256 values are in `metadata.json`.
- The original development branch, untracked `fwstage/`, and `work/` were left
  untouched. The checkpoint was created in a separate worktree.

## Checks performed

1. **Real `git am` replay: passed.**

   In a separate clean worktree at the pristine commit, apply all six patches in
   `series` with `git am --whitespace=nowarn`. The resulting Git tree was exactly
   `1871f732bbdf403a72a9c36a47f4d7dbb07e17f3`; comparison with the archived snapshot
   had no differences and the replay worktree was clean.

2. **Independent index replay: passed.**

   ```sh
   python3 verify-series.py /path/to/linux
   ```

   All patch hashes passed, all six patches applied to the exact pristine base,
   and the output tree matched. The real index, HEAD and worktree were unchanged.

3. **Verifier boundary tests: five checks.**

   ```sh
   python3 test-verifier.py /path/to/linux
   ```

   Success plus four fail-closed cases: tampered patch, duplicate series entry,
   incorrect resulting tree ID, missing base commit. The test checks preservation
   of the actual kernel index, HEAD and worktree status in every case.

4. **Config normalization: passed, byte-identical.**

   ```sh
   cp /path/to/patch-bundle/config/zorn.config /path/to/snapshot/.config
   make -C /path/to/snapshot ARCH=arm64 LLVM=1 olddefconfig
   cmp /path/to/snapshot/.config /path/to/patch-bundle/config/zorn.config
   ```

   No warning/error was found in this run. Config SHA256 remained
   `b0e7494f6c31fec304547d701a0545ea941dca386a9aa43168156a6ea07bc8aa`.
   In the local fully committed snapshot checkout, `make ARCH=arm64 LLVM=1 -s
   kernelrelease` returned `7.3.0-rc5-gpuwifi-00006-g13c62c8115f6`. Other clone/tag
   availability can affect the suffix; always use your actual build's release.

5. **GRUB template syntax and policy: passed.**

   ```sh
   grub-script-check runtime/grub.cfg.in
   ```

   Exactly recovery/normal/test; default normal. Normal and test both include
   `drm_display_helper.dsc_pps_bpp_double=1`; neither includes `drm.debug=0x1e`.

6. **Publication scope check:** only source patches, config, public provenance,
   runtime templates and verification scripts. No firmware payloads, private
   inputs, tokens, raw session history, full device logs or binary artifacts.
   Targeted source changes pass `git diff --check`; `.patch` whitespace is
   preserved verbatim rather than rewritten because context lines are significant.

## Not claimed

- No new full `Image modules` build or device flash was done for this archive.
  The earlier deployed artifacts were compiled; recovered source correspondence
  and new patch replay were checked independently here.
- A fresh build will not automatically reproduce the old `-dirty` release,
  binary build timestamp or SHA256. Do not mix its modules with old artifacts.
- User-reported normal operation is not an automated full hardware test or a
  diagnosed Waydroid panic fix. The old terminal panic stack was unavailable.
- Diagnostics and experimental static-HBB/single-slice settings are deliberately
  preserved, not represented as generally valid upstream fixes.
- Existing production manifest/builder pins are not changed by this publication.
