# FH2-FSR4

**AMD FSR 4 (FSR Upscaling 4.1.1) for Forza Horizon 2 running on [Azimuth](https://almost-there-games.itch.io/azimuth).**

This mod replaces the game's MSAA with FSR 4's temporal anti-aliasing. At native resolution it is Native AA (no upscaling); when Azimuth renders below the display resolution (`resolution=` in `data\azimuth.ini`), the mod replaces Azimuth's final bilinear stretch with FSR upscaling to the display resolution. The result is no jaggies, stable fences, wires and foliage, and a much cleaner image in motion.

It is an unofficial mod, not affiliated with AMD, Microsoft, Playground Games or Almost There Games.

## Requirements

| | |
|---|---|
| Azimuth | 1.0.x (render-pass hashes taken from Azimuth 1.0.3 / 1.0.4) |
| Forza Horizon 2 | build 1.0.0.64 (your own copy, set up in Azimuth) |
| GPU | AMD Radeon RX 9000 (RDNA 4). RX 7000 (RDNA 3) should also work, since AMD enabled FSR 4.1 for it in SDK 2.3.0 |
| Driver | A recent AMD Software: Adrenalin |

So far it has only been tested on an **RX 9070 XT**, on Windows 11 at 2560x1440 with HDR. On other GPUs, AMD's loader picks a different provider, which has not been tested yet.

## Installation

1. Download the `.zip` from the [Releases](../../releases) page.
2. Copy the three DLLs into your **Azimuth** folder, next to `azimuth.exe`:
   - `dxgi.dll` (the mod)
   - `amd_fidelityfx_loader_dx12.dll` (AMD)
   - `amd_fidelityfx_upscaler_dx12.dll` (AMD, FSR 4.1.1)
3. In the Azimuth launcher, **turn anti-aliasing off**. FSR becomes the anti-aliasing. You can also edit `data\azimuth.ini` and set `antialiasing=off`.
4. Play as usual.

To uninstall, delete the three DLLs.

It works alongside [FH2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions), which lives in `data\bin` and does not conflict with this mod.

## Keys

| Key | Action |
|---|---|
| **F11** | Toggle FSR on/off (handy for comparisons) |

The key is polled from a dedicated thread, so F11 works even when the game is launched from Steam.

## FSR upscaling

Azimuth 1.x renders the game at the render resolution (`resolution=` / `aspect=` in `data\azimuth.ini`) into the HDR scene buffer and stretches/tonemaps it onto the display-sized back buffer itself. When the render size is smaller than the display size, this mod upscales the HDR scene with FSR 4 right after the scene resolve and redirects the game's scene-colour texture bindings to the upscaled image, so the game's own present pass tonemaps the upscaled frame into the back buffer (pillarboxing and the HUD are handled by the game at display resolution and stay crisp).

1. Set e.g. `resolution=1080` with `aspect=21:9` (match your display's aspect) in `data\azimuth.ini` on a 3440x1440 display (keep `antialiasing=off`).
2. Launch as usual. `fh2_fsr.log` reports `upscale mode: render 2580x1080 < display 3440x1440` and `FSR frame N (upscale, render 2580x1080 -> out 3440x1440)`.

When render size == display size, the mod automatically stays in Native AA mode. Works the same with HDR on or off — the scene buffer is linear HDR either way; only the game's final tonemap differs.

Optional `fh2_fsr.ini` next to the DLLs:

```ini
[fsr]
mode=auto        ; auto | native | upscale
hdr=auto         ; auto | on | off  (FSR HDR flag on the scene input; auto = on)
sharpness=-1     ; -1 = off, 0..1 = FSR sharpening strength (1 = max)
debug_blink=0    ; 1 = alternate 30 frames FSR / 30 frames the game's own stretch (visibility test)
```

Motion vectors, depth and camera jitter are taken from the game's own passes at render resolution, so they scale automatically with the render size.

The log is written to `fh2_fsr.log` in the Azimuth folder.

## How it works

FH2 never had TAA, and FSR needs four things the game doesn't provide out of the box. The mod is a `dxgi.dll` proxy that hooks Azimuth's D3D12 vtables and builds each of them:

- **Sub-pixel jitter:** offsets the viewport of the scene passes, only while the scene depth buffer is bound, following FSR's official jitter sequence.
- **Full-resolution motion vectors:** FH2's motion blur already computes the reprojection of the camera and of each car (the matrix is picked by each object's stencil value), but at half resolution and with a clamped vector. The mod runs a variant of that shader at full resolution, without the clamp, using the game's own bindings.
- **HDR colour and depth:** the mod takes the targets of the scene resolve, before tonemapping and the HUD.
- **FSR dispatch:** FSR runs inside the game's own command list right after the scene resolve. In Native AA mode its output is copied back into the scene colour in place; in upscale mode (render < display) the output is the scene upscaled to the display-fitted size and the game's scene-colour SRV descriptors are redirected to it. Either way the mod then restores the game's descriptor heaps, root signatures, tables and PSO.

The game's passes are identified by the DXBC checksums of their pixel shaders (the Azimuth 1.x builds), and the resolve is also validated by its render-target formats (R11G11B10 colour + D32 depth).

## Azimuth 1.x notes

On Azimuth 1.x the mod deliberately does **not** hook `Present`/`ResizeBuffers` on the swap chain: the Steam overlay hooks `Present` too, and with both on the vtable the game crashed at startup when launched from Steam. Azimuth 1.x may also own several swap chains, so Present is not a reliable frame boundary. Frame counting and the jitter state are driven from the scene resolve instead. When the game stops its velocity pass (pause/photo mode), the mod falls back to zero motion vectors with a history reset.

## Known limitations

- The recognised shaders are for **FH2 1.0.0.64**. Storm Island and Fast & Furious may need adjustments.
- Spinning wheels and swaying foliage don't have their own motion vectors.
- There is no reactive mask, so particles and transparencies may leave a slight trail.
- The camera parameters passed to FSR (near, far and FOV) are approximations.

## Building

Requires [Zig 0.15.2](https://ziglang.org/download/), Git and a POSIX shell (Git Bash works on Windows).

```sh
ZIG=/path/to/zig ./build.sh           # build/dxgi.dll
ZIG=/path/to/zig ./build.sh release   # plus dist/FH2-FSR4-<version>.zip
```

The script downloads the FSR SDK v2.3.0 headers from AMD's official repository. In `release` mode it also downloads AMD's signed DLLs.

## Licenses

- The code in this repository is under the MIT license (see [LICENSE](LICENSE)).
- The FSR DLLs belong to AMD and are redistributed unmodified under the FSR SDK license, which ships in the release zip as `LICENSE-AMD-FSR.txt`.
