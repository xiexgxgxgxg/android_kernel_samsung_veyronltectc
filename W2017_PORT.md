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
