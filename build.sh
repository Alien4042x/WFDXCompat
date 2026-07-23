#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TOOLCHAIN=${TOOLCHAIN:-}
if [ -n "$TOOLCHAIN" ]; then
    CC="$TOOLCHAIN/bin/x86_64-w64-mingw32-clang"
else
    CC=$(command -v x86_64-w64-mingw32-clang || true)
fi
if [ -z "$CC" ] || [ ! -x "$CC" ]; then
    echo "x86_64-w64-mingw32-clang is required; set TOOLCHAIN or add it to PATH" >&2
    exit 1
fi
WFDXCOMPAT_CFLAGS=${WFDXCOMPAT_CFLAGS:-}
OUT="$ROOT/build/x86_64-windows"
STAGE="$ROOT/lib/wfdxcompat/x86_64-windows"
WINEBUILD=${WINEBUILD:-}

if [ -z "$WINEBUILD" ]; then
    WINEBUILD=$(command -v winebuild || true)
    if [ -z "$WINEBUILD" ] && [ -x "$ROOT/../install-archs-i386_x86_64/bin/winebuild" ]; then
        WINEBUILD="$ROOT/../install-archs-i386_x86_64/bin/winebuild"
    fi
fi
if [ -z "$WINEBUILD" ] || [ ! -x "$WINEBUILD" ]; then
    echo "winebuild is required to mark staged PE files as Wine builtins" >&2
    exit 1
fi

mkdir -p "$OUT" "$STAGE"
"$ROOT/tools/generate_forwarders.sh" "$ROOT/exports/d3d12.exports" \
    wfdxbackend-d3d12 "$OUT/d3d12.def"
"$CC" -shared -O2 -Wall -Wextra -Werror -I"$ROOT/include" \
    $WFDXCOMPAT_CFLAGS \
    "$ROOT/src/frontend/module.c" "$ROOT/src/frontend/proxy.c" \
    "$ROOT/src/frontend/nv12_resource.c" "$ROOT/src/frontend/resource_notifier.c" \
    "$ROOT/src/frontend/command_list_proxy.c" \
    "$ROOT/src/core/notifier_registry.c" \
    "$OUT/d3d12.def" -o "$OUT/d3d12.dll"
"$WINEBUILD" --builtin "$OUT/d3d12.dll"
"$WINEBUILD" --dll --fake-module -m64 -F wfdxbackend-d3d12.dll \
    -o "$OUT/wfdxbackend-d3d12.dll"
rm -f "$OUT/d3d11.dll" "$OUT/wfdxbackend-d3d11.dll" \
    "$OUT/wfdxcompat-core.dll" "$OUT/libwfdxcompat-core.a"
rm -f "$STAGE/d3d11.dll" "$STAGE/wfdxbackend-d3d11.dll" \
    "$STAGE/wfdxcompat-core.dll" "$STAGE/.DS_Store"
cp -f "$OUT/d3d12.dll" "$OUT/wfdxbackend-d3d12.dll" "$STAGE/"
echo "staged: $STAGE"
