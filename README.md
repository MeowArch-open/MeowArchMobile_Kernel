# zorn / SM8650: Linux v7.3-rc5 source checkpoint

This branch archives the complete local kernel patch series separately from the
pristine Linux tree. It contains **patches and build/runtime inputs, not a kernel
source checkout**. No firmware, private device data, crash journals, or built
kernel/module binaries are included.

## Repository and references

All three references belong to
[MeowArchMobile_Kernel](https://github.com/MeowArch-open/MeowArchMobile_Kernel):

| Purpose | Reference | Commit |
| --- | --- | --- |
| Pristine upstream base | tag `v7.3-rc5-meowarch-base` | `72d3fcf802c45d00b300f25b848a93c3a2bd7c7e` |
| Patched source checkpoint | tag `v7.3-rc5-meowarch1`, branch `v7.3-rc5-zorn-tested` | `13c62c8115f60347320b2faa543a9838f0b63de4` |
| This independent patch archive | branch `patches/zorn-v7.3-rc5` | use the commit of this checkout |

The base is **Linux v7.3-rc5**, not a final v7.3.0 release. Its tree must remain
`b9e9f22cee46d79f64ee1d888a80ca0d04dd8df6`. The upstream annotated tag object is
`5a956dde5526a634dca7ccad27c051ebcc306089`.

Applying all six patches in `series` must produce tree:

```text
1871f732bbdf403a72a9c36a47f4d7dbb07e17f3
```

`metadata.json` records the complete provenance and artifact identities.
The original local development branch and its history are not rewritten.

### Where older patches live

Historically the organization kept subsystem patches/overlays in
`MeowArchMobile_sm8650AB_Display`, `MeowArchMobile_sm8650AB_Audio`,
`MeowArchMobile_sm8650AB_Touch`, and other component repositories. Those are not
an ordered, complete v7.3-rc5 stack. In particular, the old Display patch mixes
pre-images, and core driver/gpio/pinctrl changes are absent from that layering.
**Do not combine those historical overlays with this full series.**

This dedicated patch-only branch uses the existing Kernel repository instead of
introducing another repository or mixing packaging files into the pristine tag.

## What is preserved

1. The complete zorn local port: DSC/DPU/DSI/panel, touch, IPA/QMI, battery/power,
   AudioReach/TDM/MI2S/SIA91XX, and core probe/GPIO/pinctrl changes.
2. CAMSS diagnostic instrumentation.
3. The DSC all-slices migration and TDM ops conflict resolution from the clean
   v7.3-rc5 port.
4. The tested static-HBB override.
5. Recovered display source edits, including the **single-slice packet override**
   present in the deployed driver, plus its framebuffer/layout/clock/DSC logs.

Patch 1's historical subject says "on 7.0.12"; that is an old subject retained
from the port. These patches apply to the v7.3-rc5 base above, not to 7.0.12.
Patch 6 supersedes the host packet choice from patch 3, preserving the tested
`slice_per_pkt=1` / `pkt_per_line=2` state. None of the HBB or packet experiments
is claimed to be the display-corruption fix.

**This is a faithful source checkpoint, not an upstream-ready or log-free
production cleanup.** Diagnostic comments include hypotheses that were later
disproved; they are retained for byte-identical source provenance. In particular,
`zorn-dppsprobe` prints the local DSC config, not the final on-wire PPS payload.
The actual display fix was delivering the PPS parameter to the built-in helper.

Five uncommitted source files had been restored out of the original working tree
after building the deployed `msm.ko`. Their edits were recovered locally and all
five matched the source MD5s in that module's DWARF line tables. Across 677 source
paths referenced by the installed module, the other checked files matched the
committed tree. The recovery commit does not include the later, undeployed
`zorn-dppspath` probes. This proves source correspondence at that boundary, not a
claim that a fresh build has the old binary hash or release string.

## Verify and apply

Requires Git and Python 3. Start with separate patch and kernel directories:

```sh
git clone --single-branch --branch patches/zorn-v7.3-rc5 \
  https://github.com/MeowArch-open/MeowArchMobile_Kernel.git zorn-patches
git clone --depth 1 --branch v7.3-rc5-meowarch-base \
  https://github.com/MeowArch-open/MeowArchMobile_Kernel.git linux-zorn

python3 zorn-patches/verify-series.py linux-zorn
```

The verifier checks `SHA256SUMS`, replays every patch against the exact base in a
**temporary Git index**, and compares the resulting tree ID. It leaves your real
index, worktree, HEAD and refs alone. It may add replayed objects to Git's object
store. Checksums establish integrity, not authenticity of an untrusted download.

Then apply to a clean checkout (Git author/committer identity must be configured):

```sh
PATCHES=$(cd zorn-patches && pwd)
cd linux-zorn
test "$(git rev-parse HEAD)" = 72d3fcf802c45d00b300f25b848a93c3a2bd7c7e
test -z "$(git status --porcelain)"
git switch -c zorn-v7.3-rc5-replay
while IFS= read -r patch; do
  git am "$PATCHES/$patch" || exit 1
done < "$PATCHES/series"
test "$(git rev-parse HEAD^{tree})" = 1871f732bbdf403a72a9c36a47f4d7dbb07e17f3
```

`git am` replay can have different commit IDs because of committer metadata;
**tree identity**, not a regenerated commit ID, is the invariant. Alternatively,
clone the already patched `v7.3-rc5-meowarch1` tag and skip applying the series.

## Build inputs

Use the archived full `config/zorn.config`, **`ARCH=arm64 LLVM=1`**, and Clang/LLD
22.1.8 (the toolchain used for the deployed artifacts).

The config embeds three GPU firmware files from `fwstage/`. Obtain these
separately from your authorized firmware source; they are not part of this
patch archive. The files under `linux-zorn/fwstage/` must match
`config/firmware-inputs.sha256`:

```sh
# From linux-zorn; PATCHES is the absolute path above.
(cd fwstage && sha256sum -c "$PATCHES/config/firmware-inputs.sha256")
cp "$PATCHES/config/zorn.config" .config
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j6 Image modules
make ARCH=arm64 LLVM=1 -s kernelrelease
# Stage modules from THIS build, never from an older -dirty directory.
make ARCH=arm64 LLVM=1 modules_install \
  INSTALL_MOD_PATH="$PWD/modroot" INSTALL_MOD_STRIP=1
```

`CONFIG_LOCALVERSION_AUTO=y` derives a release suffix from the actual checkout.
The archived deployed release was
`7.3.0-rc5-gpuwifi-00005-g2a1f6733fb86-dirty`; a committed/replayed checkout may
produce a different release. **Do not rename module directories or force module
vermagic to pretend a new Image is the old build.** Always deploy Image and its
matching modules together, and regenerate dependencies for that release.

`qcomtee` is enabled as an in-tree module by this config; this archive does not
rebuild or replace Qualcomm's QTEE firmware. If a rootfs still loads a fixed
`/usr/local/lib/zorn/qcomtee.ko`, synchronize it from the newly built release or
use the matching release's `modprobe qcomtee` path. Never retain an older fixed
copy across a kernel release change.

## Runtime requirements (part of the checkpoint)

Install `runtime/zorn-dsc422.conf` as the display policy, preserving any other
required platform module ordering. With this config:

```text
CONFIG_DRM_DISPLAY_HELPER=y
```

Therefore `options drm_display_helper dsc_pps_bpp_double=1` in modprobe.d alone is
**ineffective**. Normal/test must also receive:

```text
drm_display_helper.dsc_pps_bpp_double=1
```

Without it, local DPU registers can still show native 4:2:2 correctly while the
panel receives the wrong PPS bits-per-pixel field. The successful test added
only this kernel command-line parameter to the previous diagnostic boot.

`runtime/grub.cfg.in` is a three-entry template (recovery, normal, test; default
normal). It deliberately omits `drm.debug=0x1e`. Substitute the ESP filesystem
UUID and root PARTUUID, retain the matching zorn recovery/normal DTBs, and keep
all GRUB copies used by your ESP consistent. DT sources belong to
`MeowArchMobile_common_rootfs/profiles/zorn/boot/`; they are not replaced here.
This is the **boot -> Mu-UEFI -> ESP -> Arch** chain, not an Android boot image.

Verify on actual Arch, not an Android chroot:

```sh
uname -r
cat /proc/cmdline
cat /sys/module/drm_display_helper/parameters/dsc_pps_bpp_double  # Y
systemctl --failed
df -h /
```

The snapshot retains verbose probes, especially per-frame `zorn-dppsprobe`.
Omitting `drm.debug` does **not** remove these `pr_info` messages. Monitor/bound
journal usage and avoid the obsolete `zorn-display-probe` service that appends
large display logs. Do not remove display-kick as an incidental cleanup.

The device had zero unreserved free filesystem blocks; old module trees/logs
were cleaned and the qcomtee copy matched before the user reported normal
operation. Space repair and this checkpoint are **not proof of the earlier
Waydroid panic's root cause**; no final panic stack was recovered.

## Existing image-builder integration

The currently published manifest pins a baked 7.2.7 kernel. Its builder skips
historical component patches when it sees the Xiaomi panel source. This archival
publication **does not change those production pins or silently migrate the
pipeline**. Do not point that old builder at the clean base alone: use a verified
full-series apply path, or the patched source tag, and carry the PPS boot argument
and exact config into the builder together. Kernel config/GRUB generation belongs
to `MeowArchMobile_Builder`; rootfs assembly copies subsystem runtime policy.

## Validation boundary

The deployed Image/modules were built earlier and the user now reports normal
operation. For this publication, verify source checksums, six-patch replay/tree
identity, config normalization and GRUB syntax. This is **not a newly flashed
kernel, a bit-for-bit rebuilt binary release, or a new Waydroid stress test**.
See `VALIDATION.md` for commands and actual results.
