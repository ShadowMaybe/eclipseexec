#!/bin/sh
#
# eclipseexec — build and run the host unit tests.
#
# Needs nothing but a C compiler: no Android SDK, no emulator. The Android-only
# files (JNI, the namespace, Turnip) are covered by the real NDK build in CI.
#
# Usage: tests/run_tests.sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
OUT="$ROOT/tests/build"
CC=${CC:-cc}

mkdir -p "$OUT"

CFLAGS="-std=gnu11 -Wall -Wextra -Wshadow -fPIC -D_GNU_SOURCE"
INCLUDES="-I$ROOT/include -I$ROOT/src -I$ROOT/tests"

# The fixture: a real shared object with a known SONAME and an exported
# symbol, so the maps/ELF tests have something true to check against.
# shellcheck disable=SC2086  # $CFLAGS holds a list of flags and must word-split
$CC $CFLAGS -shared -Wl,-soname,libfixture.so \
    -o "$OUT/libfixture.so" "$ROOT/tests/fixture.c"

# The library under test, minus the files that need a device.
# shellcheck disable=SC2086  # likewise: $CFLAGS and $INCLUDES are word lists
$CC $CFLAGS $INCLUDES -o "$OUT/test_eclipseexec" \
    "$ROOT/src/eclipse_exec.c" \
    "$ROOT/src/eclipse_maps.c" \
    "$ROOT/src/eclipse_elf.c" \
    "$ROOT/src/eclipse_affinity.c" \
    "$ROOT/tests/stubs_namespace.c" \
    "$ROOT/tests/test_eclipseexec.c" \
    -ldl -lpthread

"$OUT/test_eclipseexec" "$OUT/libfixture.so"

# The Java class and the native registration table are two halves of one
# contract; this checks they still agree.
if command -v python3 >/dev/null 2>&1; then
    python3 "$ROOT/tools/check_jni_bindings.py"
else
    echo "python3 not found: skipped the JNI binding check" >&2
fi
