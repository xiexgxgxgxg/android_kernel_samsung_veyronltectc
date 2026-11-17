# W2017 kernel — port notes

> **STATUS: WORK IN PROGRESS.** This is the *stock* Samsung Android 6.0.1 kernel
> source for the W2017. It boots TWRP and now builds out-of-tree for LineageOS
> 18.1, but the Android 11 kernel features (configfs USB, quota, memcg, F2FS)
> are still missing, so it is not expected to boot 18.1 yet.

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

## Milestone: the 3.18.140 kernel builds

`Linux version 3.18.140-g42f3dc2e9-dirty ... #4 SMP PREEMPT`, `Image.gz`
10218403 bytes, md5 `0705d9d524331e3648ecfeebd7466796`.

Fixing the last mile required these deliberate, documented deviations:

| area | change | why |
|---|---|---|
| `arch/arm64/include/asm/asm-uaccess.h` | added from 3.18.140 | new file the merge skipped |
| `net/netfilter/{xt_TCPMSS,xt_RATEEST,xt_dscp,xt_hl}.c`, `include/uapi/linux/netfilter*/{xt_mark,xt_connmark,ipt_ttl,ipt_ecn,ipt_REJECT}.h` | added from 3.18.140 | upstream 3.18.140 renamed `xt_*` sources to upper case; the merged Makefile points at the new names |
| `CONFIG_RKP_CFP`, `CONFIG_RKP_CFP_JOPP` | **disabled** | Samsung's post-link instrumentation (`scripts/rkp_cfp/instrument.py`) needs a cross `gdb`, which this build host does not have. Reversible once `aarch64-linux-android-gdb` exists. |
| `scripts/fips_crypto_utils.c` | added `string.h` + two forward declarations | Samsung's host-side FIPS utility, kept working |
| `fs/proc/base.c` | `smaps_simple` node removed | needs `mem_size_stats.swap_pss`, which 3.18.140 does not have; Samsung-ROM-only convenience |
| `net/ipv4/tcp_input.c` | re-added `sysctl_tcp_default_init_rwnd` | Samsung sysctl still referenced by `sysctl_net_ipv4.c` |
| `drivers/staging/android/ion/ion.c`, `drivers/usb/dwc3/gadget.c`, `drivers/usb/dwc3/core.h`, `drivers/usb/gadget/function/{f_acm,f_fs}.c` | taken whole from the Samsung baseline | heavily CAF-forked; mixing an upstream file with the vendor headers produced inconsistent code |
| `fs/proc/task_mmu.c`, `net/ipv4/tcp_input.c`, `net/ipv4/tcp_output.c`, `drivers/usb/core/hub.c` | taken whole from 3.18.140 | generic code; the Samsung forks there were cosmetic |

Lesson learned (cost me a full rebuild cycle each time): for a file whose
conflict blocks are resolved by side, the *non-conflicting* regions still come
from whichever side the automatic merge chose. Picking per hunk inside a
vendor-forked file leaves the file internally inconsistent. Decide per file:
either take the whole file from one side, or merge it by hand end to end.

## Milestone: the 3.18.140 kernel boots TWRP (2026-11-18)

```
Linux version 3.18.140-g2d09fb3de-dirty #31 SMP PREEMPT aarch64
TWRP 3.7.0_9-veyronltectc-wip   ro.product.device = veyronltectc
```

Confirmed on the device, over adb:

| item | state |
|---|---|
| UFS | `sda` 62 GB, all partitions present; `sdb`, `sdc` too |
| microSD | `mmcblk0` + `mmcblk0p1` |
| display | `/dev/graphics/fb0`, mode `U:1080x1920p-60` |
| input | `event0`..`event8` |
| userspace | `recovery` and `adbd` running |
| persistent log | `sdlog: raw mmc ok, capacity 1943552 sectors, region 1927168` and `SDLG` on the card |

### What made the difference

`drivers/mmc/` had been merged per hunk, so Samsung's `sdhci-msm` was sitting on
top of an upstream mmc core.  Taking the whole directory from the Samsung
baseline (the same rule already applied to ion, dwc3 and the UFS stack) gave a
working host.  `mmc_first_sd_card()` is re-added on top for veyron_sdlog.

### Method notes that wasted hours tonight

* **Reaching an initcall proves only that the previous one returned.**  It says
  nothing about whether that probe succeeded, so "initcall N reached" must not
  be reported as "driver N works".
* **Any probe that performs block I/O during the initcall sweep perturbs the
  boot** enough to hide the real behaviour.  A persdata writer that was active
  from level 6 onward invalidated five bisection runs before this was noticed.
* **A one-bit channel needs an unambiguous read-out.**  "2 s bite vs 90 s bite"
  is fine; "1 s bite vs the watchdog's natural 20 s" is not, because the two
  windows overlap.
* The bootloader watchdog can be used to keep the board alive indefinitely
  (`0x9830008 = 0` at the top of `start_kernel`), which is what proved the
  reset is watchdog driven and not `CONFIG_PANIC_TIMEOUT=5`.

## Milestone: the kernel builds out-of-tree for LineageOS 18.1 (2026-11-18)

The LineageOS build system compiles the kernel with `O=<out>/KERNEL_OBJ`, which
this Samsung tree had never supported. These changes make that path work
without changing the TWRP configuration:

| problem | fix |
|---|---|
| `$(srctree)/.config` was tracked, Kbuild refuses an out-of-tree build | `.config` is no longer tracked; `veyronlte_ctc_defconfig` is the source of truth |
| `scripts/gcc-wrapper.py` was Python 2 | ported to Python 3 |
| host tools need `-fcommon`; Samsung modpost needs the mismatch check relaxed | board config passes both |
| host tools could not find a linker in the hermetic PATH | `-fuse-ld=lld` in HOSTCFLAGS and in `kaslr_fips_hmac.sh` |
| firmware `.gen.S` were committed with relative `.incbin` paths | generated into the output dir from the source tree, generated files untracked |
| `kernel/time/hz.bc` wanted `include/config/hz.h` | dependency dropped; it only echoes `CONFIG_HZ` |
| RKP CFP/JOPP instrumentation needs Python 2 and a cross gdb | disabled |
| `headers_install` missed many UAPI files | display, media and IPA UAPI headers restored from the 3.18 msm8996 baselines |
| defconfig did not match the verified `.config` | `SEC_RESTRICT_*`, Samsung MTP/SideSync, DUN and NCM-MTU switches synced; `VEYRON_SDLOG` enabled |

The Android 11 userspace gaps (`CONFIG_USB_CONFIGFS`, quota, memcg, F2FS) are
still open and are the next step before a Lineage boot test.
