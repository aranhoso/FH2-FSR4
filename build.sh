#!/bin/sh
# Builds dxgi.dll (x86_64 Windows) with Zig, and optionally a release zip.
#   ZIG=/path/to/zig ./build.sh            -> build/dxgi.dll
#   ZIG=/path/to/zig ./build.sh release    -> also dist/FH2-FSR4-<version>.zip
# The AMD FSR API headers and DLLs come from the official FSR SDK v2.3.0 (FSR Upscaling 4.1.1).
set -e
cd "$(dirname "$0")"
VERSION=0.3.0
SDK_TAG=v2.3.0
ZIG=${ZIG:-zig}

# Build dependencies: Zig 0.15.2 (compiler), git + network (one-time sparse clone of the
# FSR SDK headers into third_party/), and for `release` mode curl + unzip (signed AMD DLLs;
# zip is optional, tar.exe is used as fallback).
if ! command -v "$ZIG" >/dev/null 2>&1; then
    echo "zig not found at '$ZIG'. Install Zig 0.15.2 (https://ziglang.org/download/) and rerun:" >&2
    echo "  ZIG=/path/to/zig ./build.sh" >&2
    exit 1
fi

# FSR API headers: sparse checkout of the official SDK repository.
FFX=third_party/FidelityFX-SDK
if [ ! -f "$FFX/Kits/FidelityFX/upscalers/include/ffx_upscale.h" ]; then
    rm -rf "$FFX"
    git -c core.longpaths=true clone -q --depth 1 --branch "$SDK_TAG" --filter=blob:none --sparse \
        https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git "$FFX"
    git -C "$FFX" sparse-checkout set --no-cone Kits/FidelityFX/api/include/ Kits/FidelityFX/upscalers/include/ \
        Kits/FidelityFX/docs/license.md
fi

mkdir -p build
"$ZIG" cc -target x86_64-windows-gnu -shared -O2 -Wall -Wextra -Wno-unused-parameter -Werror \
    -I"$FFX/Kits/FidelityFX/api/include" -I"$FFX/Kits/FidelityFX/upscalers/include" \
    -o build/dxgi.dll src/proxy.c src/hooks.c src/fsr.c src/dxgi.def -luser32 -lkernel32
echo "built build/dxgi.dll"

if [ "$1" = "release" ]; then
    # Signed AMD DLLs from the SDK's prebuilt package.
    PKG=third_party/FidelityFX-Samples-$SDK_TAG-prebuilt
    if [ ! -f "$PKG/amd_fidelityfx_upscaler_dx12.dll" ]; then
        mkdir -p "$PKG"
        curl -sSL -o "$PKG.zip" \
            "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/$SDK_TAG/FidelityFX-Samples-$SDK_TAG-prebuilt.zip"
        unzip -q -j -o "$PKG.zip" \
            "Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/amd_fidelityfx_loader_dx12.dll" \
            "Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/amd_fidelityfx_upscaler_dx12.dll" -d "$PKG"
    fi
    OUT=dist/FH2-FSR4-$VERSION
    rm -rf "$OUT" "$OUT.zip"
    mkdir -p "$OUT"
    cp build/dxgi.dll "$PKG/amd_fidelityfx_loader_dx12.dll" "$PKG/amd_fidelityfx_upscaler_dx12.dll" "$OUT/"
    # Ship the PDB alongside the DLL so crash dumps resolve mod symbols (proxy.lib is link-time only).
    [ -f build/dxgi.pdb ] && cp build/dxgi.pdb "$OUT/"
    cp "$FFX/Kits/FidelityFX/docs/license.md" "$OUT/LICENSE-AMD-FSR.txt"
    cp LICENSE "$OUT/LICENSE.txt"
    cp release/README.txt "$OUT/"
    if command -v zip >/dev/null 2>&1; then
        (cd dist && zip -q -r "FH2-FSR4-$VERSION.zip" "FH2-FSR4-$VERSION")
    else
        # Windows' bsdtar writes zip archives with -a.
        (cd dist && "${SYSTEMROOT:-C:/Windows}/System32/tar.exe" -a -cf "FH2-FSR4-$VERSION.zip" "FH2-FSR4-$VERSION")
    fi
    echo "packaged dist/FH2-FSR4-$VERSION.zip"
fi
