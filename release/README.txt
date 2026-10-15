FH2-FSR4 - AMD FSR 4 Native AA for Forza Horizon 2 on Azimuth

INSTALLATION
1. Copy the three DLLs in this folder into your Azimuth folder (next to azimuth.exe):
     dxgi.dll
     amd_fidelityfx_loader_dx12.dll
     amd_fidelityfx_upscaler_dx12.dll
2. In the Azimuth launcher, turn anti-aliasing off.
   (Or edit data\azimuth.ini and set antialiasing=off)
3. Play.

KEYS
  F11  toggle FSR on/off
  F9   motion-vector debug overlay
  F10  overlay scale

UNINSTALL
  Delete the three DLLs.

Requirements: Azimuth 0.9.9, Forza Horizon 2 1.0.0.64, AMD RX 9000 GPU (RX 7000 should work).
Log: fh2_fsr.log in the Azimuth folder.

The amd_fidelityfx_* DLLs belong to AMD (FSR SDK 2.3.0) and are redistributed unmodified
under LICENSE-AMD-FSR.txt. dxgi.dll is under the MIT license (LICENSE.txt).
Unofficial mod, not affiliated with AMD, Microsoft, Playground Games or Almost There Games.
