FH2-FSR4 - AMD FSR 4 (Native AA + upscaling) for Forza Horizon 2 on Azimuth

INSTALLATION
1. Copy the three DLLs in this folder into your Azimuth folder (next to azimuth.exe):
     dxgi.dll
     amd_fidelityfx_loader_dx12.dll
     amd_fidelityfx_upscaler_dx12.dll
2. In the Azimuth launcher, turn anti-aliasing off.
   (Or edit data\azimuth.ini and set antialiasing=off)
3. Play.

NATIVE AA (default)
  With resolution at the display height, FSR 4 Native AA replaces MSAA at native
  resolution. Log: "FSR frame N (native, ...)".

UPSCALING
  Set resolution= lower than your display height in data\azimuth.ini (keep the
  display's aspect, e.g. resolution=1080 aspect=21:9 on a 3440x1440 screen).
  The mod then upscales the HDR scene with FSR 4 to the display resolution and the
  game tonemaps the upscaled frame itself. Log: "upscale mode" and
  "FSR frame N (upscale, render WxH -> out WxH)". Works with HDR on or off.

KEYS
  F11  toggle FSR on/off

OPTIONAL fh2_fsr.ini next to the DLLs:
  [fsr]
  mode=auto        ; auto | native | upscale
  hdr=auto         ; auto | on | off (FSR HDR flag on the scene input; auto = on)
  sharpness=-1     ; -1 = off, 0..1 = sharpening strength (1 = max)
  debug_blink=0    ; 1 = alternate 30 frames FSR / 30 frames the game's own stretch

UNINSTALL
  Delete the three DLLs.

Requirements: Azimuth 1.0.x, Forza Horizon 2 1.0.0.64, AMD RX 9000 GPU (RX 7000 should work).
Log: fh2_fsr.log in the Azimuth folder.

The amd_fidelityfx_* DLLs belong to AMD (FSR SDK 2.3.0) and are redistributed unmodified
under LICENSE-AMD-FSR.txt. dxgi.dll is under the MIT license (LICENSE.txt).
Unofficial mod, not affiliated with AMD, Microsoft, Playground Games or Almost There Games.
