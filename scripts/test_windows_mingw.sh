#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN_PREFIX="${TOOLCHAIN_PREFIX:-x86_64-w64-mingw32}"
CC="${CC:-${TOOLCHAIN_PREFIX}-gcc}"
AR="${AR:-${TOOLCHAIN_PREFIX}-ar}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/windows-mingw}"

for tool in "$CC" "$AR"; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "missing tool: $tool" >&2
		exit 1
	fi
done

COMMON_FLAGS=( -O2 -g -Wall -Wextra -std=c11 -I"$ROOT/include" )

mkdir -p "$BUILD_DIR/test"

"$CC" "${COMMON_FLAGS[@]}" -c "$ROOT/src/quicksand+windows.c" -o "$BUILD_DIR/quicksand.o"
"$CC" "${COMMON_FLAGS[@]}" -c "$ROOT/src/time+windows.c" -o "$BUILD_DIR/time.o"
"$CC" "${COMMON_FLAGS[@]}" -c "$ROOT/src/timestamp+x86_64+windows.s" -o "$BUILD_DIR/timestamp.o"

"$AR" rcs "$BUILD_DIR/libquicksand.a" "$BUILD_DIR/quicksand.o" "$BUILD_DIR/time.o" "$BUILD_DIR/timestamp.o"
"$CC" -shared -o "$BUILD_DIR/libquicksand.dll" \
	"$BUILD_DIR/quicksand.o" "$BUILD_DIR/time.o" "$BUILD_DIR/timestamp.o" \
	-Wl,--out-implib,"$BUILD_DIR/libquicksand.dll.a"

for t in test_copy test_basic test_pub test_sub; do
	"$CC" "${COMMON_FLAGS[@]}" "$ROOT/test/${t}.c" "$BUILD_DIR/libquicksand.a" -o "$BUILD_DIR/test/${t}.exe"
done

echo "Windows MinGW build succeeded: $BUILD_DIR"
ls -1 "$BUILD_DIR" "$BUILD_DIR/test"
