# W2017 kernel — port notes

> **STATUS: WORK IN PROGRESS.** This is the *stock* Samsung Android 6.0.1 kernel
> source for the W2017. It is not patched for Android 11 (LineageOS 18.1) yet and
> will not build a bootable 18.1 kernel as-is.

## What this is

Fork of the official Samsung msm8996 source release for the SM-W2017
(build `W2017KEU1APJ7`, Linux 3.18.20), kept because it is the only tree that
contains the W2017-specific dual-panel logic.

* defconfig: `arch/arm64/configs/veyronlte_ctc_defconfig`
* board DTS: `arch/arm64/boot/dts/samsung/msm8996-sec-veyronlte-ctc-r0{0,1,2,5}.dts`
  (plus `.dtb` and decompiled `.dtb.reverse.dts` in the same directory)
* build script: `build_msm8996_kernel.sh veyronlte_ctc`
  (`SIGN_MODEL=SM-W2017_CHN_CTC_ROOT0`)

## Dual-panel switching — do not lose this in any rebase

| Piece | Where |
|---|---|
| Folder hall driver | `drivers/input/keyboard/folder_hall_ic.c`, DTS node `folder_hall` (PM8994 GPIO 7) |
| Hall → display plumbing | `drivers/video/msm/mdss/mdss_fb.c` (`support_hall_ic`, `hall_ic_status_pending`), `mdss_dsi_panel.c`, `samsung/ss_dsi_panel_common.c` (`samsung_display_hall_ic_status()`, `select_panel_gpio`) |
| Panel select GPIO / delay | DTS: `samsung,mdss_dsi_lcd_sel_gpio = <&tlmm 1 0>`, `samsung,mdss_dsi_lcd_flip_delay_ms = <500>` |
| Panel driver | `drivers/video/msm/mdss/samsung/S6E3FA5_AMS420MS01/` |

The switch is driven entirely in-kernel: Android's display stack never sees a
panel change. Any kernel used for this device must keep this code path.

## Path to LineageOS 18.1

Two options:

1. **Patch this tree** for Android 11 userspace (binder, sdcardfs or FUSE,
   SELinux, quota, wifi driver, uid_sys_stats, etc.). Most faithful to the
   hardware, most work.
2. **Start from `LineageOS/android_kernel_samsung_msm8996`** (which already has
   the 18.1-era patches) and port `veyronlte` DTS + the mdss panel/hall changes
   on top. Usually faster, but the mdss code differs between releases and needs
   care.

Either way: keep `veyronlte_ctc_defconfig`, keep the panel driver, keep the hall
plumbing.


## Why a dedicated kernel

Other msm8996 trees (including LineageOS' and the S7 ones) do not carry the
`veyronlte` board files, the S6E3FA5_AMS420MS01 panel driver or the `folder_hall`
plumbing. Starting from a foreign tree means re-porting all of that *and*
re-validating the display path, which is more work and a much worse failure mode
(a wrong panel select can leave the phone dark while everything else "works").
Base = this tree; borrow patches, not boards.

## Android 11 (LineageOS 18.1) config gaps in the stock defconfig

Already present: `CONFIG_ANDROID_BINDER_IPC`, `CONFIG_ASHMEM`, `CONFIG_ION`,
`CONFIG_SW_SYNC`, `CONFIG_DM_VERITY`, `CONFIG_SECURITY_SELINUX`,
`CONFIG_FUSE_FS`, `CONFIG_CGROUP_FREEZER/CPUACCT/SCHED`, `CONFIG_NAMESPACES`,
`CONFIG_NETFILTER_XT_MATCH_QTAGUID`, `CONFIG_EXT4_FS_SECURITY`.

Missing / needs work:

| Config | Impact |
|---|---|
| `CONFIG_USB_CONFIGFS` (+ `USB_CONFIGFS_F_FS`, `_F_MTP`) | not enabled, but `drivers/usb/gadget/configfs.c` **exists** in this tree — Android 11 adb/MTP needs it (stock uses `USB_G_ANDROID` legacy gadget) |
| `CONFIG_QUOTA`, `CONFIG_QUOTACTL`, `CONFIG_QFMT_V2` | ext4 project quota / storage stats |
| `CONFIG_MEMCG` | LMKD memory pressure (alternative: keep vmpressure and patch lmkd) |
| `CONFIG_CGROUP_BLKIO` | blkio cgroup (minor) |
| `CONFIG_SDCARDFS` | absent; Android 11 can use FUSE (`FUSE_FS=y`) instead |
| `CONFIG_PSI` | not available on 3.18; needs memcg/vmpressure fallback |
| `CONFIG_ENCRYPTED_KEYS` | FBE only; W2017 uses FDE, so not blocking |

## Patch source

`ivanmeler/android_kernel_samsung_msm8996`, branch `lineage-18.1` (and
`lineage-18.1-caf`) — same Samsung 3.18.20 baseline, already patched for 18.1 on
msm8996. Cherry-pick the Android-11 enabling commits (gadget configfs, quota,
memcg, sdcardfs/FUSE, SELinux bits) onto this tree instead of switching base.

---

# 3.18.20 -> 3.18.140 stable bump (clean restart, 2026-11-17)

The earlier attempt (`wip-318140`, now deleted, archived as tag
`archive/wip-318140`) left the tree half-merged: headers from one release,
sources from another, so the build failed with whack-a-mole errors and nothing
could be trusted.

This restart does it properly, from the untouched `W2017KEU1APJ7` baseline.

## Reference sources (content is never taken from a third-party port tree)

| role | source |
|---|---|
| merge base | `linux-3.18.20` from kernel.org |
| merge target | `linux-3.18.140` from kernel.org |
| ours | this tree (Samsung W2017 source) |
| architecture reference only | `LineageOS/android_kernel_xiaomi_msm8996` lineage-18.1 |
| local copies | `/data/w2017/ref/linux-3.18.20`, `.../linux-3.18.140`, `.../android_kernel_xiaomi_msm8996` |

## Method

For every file that 3.18.140 changed:

* `ours == base` (Samsung never touched it) -> copy the 3.18.140 file verbatim
* `ours != base` -> `git merge-file ours base theirs`

Result on the first pass:

| outcome | count |
|---|---|
| taken verbatim from 3.18.140 | 3482 |
| clean 3-way merge | 410 |
| conflicts needing a decision | 88 |
| skipped (file absent in our tree) | 35 |

## Resolution rules

* **Upstream fixed a real defect** (error paths, locking, bounds checks) ->
  take the upstream side.
* **Samsung added board/vendor functionality** -> keep our side.
* **Both touched the same lines** -> merge by hand, keeping the vendor feature
  and the upstream fix.

Do not resolve whole files by side without reading the hunks: picking "ours"
blindly dropped the `crypto` error checks, the `crypto/shash` scatterlist fix,
the `input` key-release-on-remove fix and `direct_complete` reset.

## Tooling

`/data/twrp/w2_audit/pick_conflict.py <file> <ours|theirs> [all|N]` resolves
conflict blocks in place; `--list` shows the block offsets.

## Status

88 conflicts found, 17 resolved in the first pass (arch/arm64, mmc, ufs,
crypto, pm, input, dvb), **71 files still carry conflict markers**.  Each is a
small, local decision; the list is in the commit messages of the batch commits.
