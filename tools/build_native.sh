#!/bin/sh
#
# eclipseexec — build the native libraries for one or more Android ABIs.
#
# Used by .github/workflows/ci.yml and release.yml, and runnable locally with
# the same three variables. Nothing here is CI-specific on purpose: if the
# script only works on a runner, the runners are lying about what they build.
#
#   ANDROID_NDK  NDK root.          Default: $ANDROID_NDK_HOME, then $ANDROID_HOME/ndk/<pinned>.
#   CMAKE        cmake executable.  Default: cmake from PATH (the SDK's own, if its bin dir is first).
#   OUT          output directory.  Default: build/native
#
# Usage: tools/build_native.sh [abi ...]
set -eu

NDK_VERSION=${NDK_VERSION:-28.2.13676358}
ANDROID_NDK=${ANDROID_NDK:-${ANDROID_NDK_HOME:-${ANDROID_HOME:+$ANDROID_HOME/ndk/$NDK_VERSION}}}
if [ -z "${ANDROID_NDK:-}" ] || [ ! -f "$ANDROID_NDK/build/cmake/android.toolchain.cmake" ]; then
    echo "tools/build_native.sh: no NDK found. Set ANDROID_NDK to the NDK root." >&2
    echo "  (looked for ANDROID_NDK, ANDROID_NDK_HOME, ANDROID_HOME/ndk/$NDK_VERSION)" >&2
    exit 2
fi

CMAKE=${CMAKE:-cmake}
OUT=${OUT:-build/native}
PLATFORM=${ANDROID_PLATFORM:-android-21}
BUILD_TYPE=${BUILD_TYPE:-Release}
GENERATOR=${GENERATOR:-Ninja}

if [ "$#" -eq 0 ]; then
    set -- arm64-v8a armeabi-v7a x86 x86_64
fi

echo "NDK:     $ANDROID_NDK"
echo "cmake:   $($CMAKE --version | head -1)"
echo "ABIs:    $*"
echo "output:  $OUT"
echo

for abi in "$@"; do
    echo "--- $abi ---"
    "$CMAKE" -S . -B "$OUT/$abi" \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" \
        -DANDROID_PLATFORM="$PLATFORM" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -G "$GENERATOR"
    "$CMAKE" --build "$OUT/$abi"
    echo
done

echo "built:"
for abi in "$@"; do
    ls -l "$OUT/$abi"/*.so
done
