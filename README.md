# eclipseexec

[![CI](https://github.com/ShadowMaybe/eclipseexec/actions/workflows/ci.yml/badge.svg)](https://github.com/ShadowMaybe/eclipseexec/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ShadowMaybe/eclipseexec?include_prereleases&label=release)](https://github.com/ShadowMaybe/eclipseexec/releases)
[![Licence: MIT](https://img.shields.io/badge/licence-MIT-blue.svg)](LICENSE)

The native bootstrap layer for **Eclipse Launcher: Minecraft Java Edition for
Android**.

`eclipseexec` is the piece that runs before the game does. It chooses and loads
the OpenGL ES or Vulkan driver the launcher wants, publishes the display
parameters the renderer must see, and pins the render thread to a big CPU core.
Everything above it — the SDL fork, the LWJGL hooks, the launcher UI — reads
that state instead of talking to the Android linker itself.

| | |
|---|---|
| library | `libeclipseexec.so` (all ABIs) plus `libeclipsehook.so` (arm64-v8a) |
| language | C11 |
| public header | [`include/eclipseexec.h`](include/eclipseexec.h) |
| Java binding | `me.shadow.eclipselauncher.exec.EclipseExec` |
| minSdk | 21 (the namespace bypass itself needs API 24) |
| licence | MIT — see [`LICENSE`](LICENSE) |

## What it does

- **`eclipseexec_prepare_egl(path, bypass, force_gles, gles_major)`** — loads a
  GL driver, either through the normal `dlopen` or through a private linker
  namespace when the driver lives where Android will not look for it.
- **`eclipseexec_acq_egl_handle()` / `eclipseexec_acq_vulkan_handle()`** — what
  SDL and the LWJGL `ndlopen` hook call instead of `dlopen`. Each returns a
  reference of its own, so a caller that `dlclose()`s it cannot take the driver
  away from the rest of the process. These are the two functions the rest of
  the launcher links against.
- **`eclipseexec_renderspec`** — the struct SDL's EGL setup reads to decide
  between desktop GL and GLES and which GLES major version to request.
- **`eclipseexec_preload_vulkan()`** — pulls the Turnip driver in ahead of time
  when the launcher has selected it, so the game does not pay for it on first
  frame.
- **`eclipseexec_make_bigcore_affine()`** — moves the calling thread onto the
  highest-frequency CPU in the device, once, if the launcher enabled it.

## Getting the binaries

Everything published is on the **[Releases page](https://github.com/ShadowMaybe/eclipseexec/releases)**
— not as workflow artifacts, which have a storage quota and expire:

| asset | contents |
|---|---|
| `eclipseexec-<tag>-<abi>.zip` | `libeclipseexec.so`, `libeclipsehook.so` (arm64-v8a only), `eclipseexec.h` |
| `eclipseexec-<tag>.aar` | the same libraries for all four ABIs, plus the Java class and the R8 keep rules |
| `SHA256SUMS.txt` | checksums for both |

Grab the zip for one architecture, or the AAR if you are an Android app.

## Building

### In GitHub Actions

Builds happen on GitHub, not on developer machines:

- **[`ci.yml`](.github/workflows/ci.yml)** runs on every push and pull request:
  host unit tests and the JNI binding check, a native build of all four ABIs
  with a pinned NDK, an export-surface check on the resulting `.so` files, and
  the Gradle AAR. It uploads nothing — it only decides whether the tree is
  healthy.
- **[`release.yml`](.github/workflows/release.yml)** runs when you push a
  version tag, builds everything again, and publishes the assets above to the
  Releases page. A manual dispatch builds and verifies the same steps without
  publishing, so a tag is never the first time the release path is exercised.

Cutting a release:

```sh
git tag v0.1.0
git push origin v0.1.0
```

### Locally

The same scripts CI runs:

```sh
# native libraries, one or more ABIs (needs an Android NDK)
ANDROID_NDK=/path/to/ndk tools/build_native.sh arm64-v8a

# unit tests + the Java/C binding check — no device, no emulator
sh tests/run_tests.sh

# the AAR (needs JDK 17+ and an Android SDK)
./gradlew :jni_bindings:assembleRelease
```

`tools/build_native.sh` reads `ANDROID_NDK`, `CMAKE` and `OUT` from the
environment; everything else it picks up from the toolchain file.

## Layout

```
include/eclipseexec.h      public C API
src/eclipse_exec.c         state, error buffer, EGL acquisition
src/eclipse_jni.c          JNI_OnLoad, RegisterNatives, the Java entry points
src/eclipse_vulkan.c       system and Turnip Vulkan loader
src/eclipse_affinity.c     big-core affinity
src/eclipse_namespace.c    private linker namespace (the driver bypass)
src/eclipse_namespace.h    the four functions that isolate it
src/eclipse_elf.c          minimal ELF symbol lookup and DT_SONAME patching
src/eclipse_maps.c         /proc/self/maps lookup
src/eclipse_arm64.c        arm64 fallback route to __loader_dlopen
src/eclipse_hook.c         libeclipsehook.so interposer (arm64-v8a only)
src/eclipse_log.h          logging macros
jni_bindings/              Gradle module: the Java class + keep rules
tests/                     host unit tests, runnable without a device
tools/                     build, export check, JNI check, release packaging
.github/workflows/         CI and the release pipeline
```

## Tests

`sh tests/run_tests.sh` builds and runs the host tests. They cover the parts
that behave the same on Linux and Android — configuration and error reporting,
the `prepareEgl` call order, the big-core affinity policy, `/proc/self/maps`
lookup and the `DT_SONAME` patching — plus `tools/check_jni_bindings.py`, which
fails the build if the Java signatures and the `g_methods[]` table in
`eclipse_jni.c` drift apart.

The pieces that need bionic (the linker namespace, the Turnip path, JNI
registration) are exercised by the real NDK build in CI, including a check that
the libraries export exactly the intended symbols and nothing else.

## Using it from Java

```java
import me.shadow.eclipselauncher.exec.EclipseExec;

static boolean setupRenderer(String glLibrary, boolean useNamespaceBypass) {
    System.loadLibrary("eclipseexec");
    EclipseExec.setNativeLibraryDir(applicationInfo.nativeLibraryDir);
    if (!EclipseExec.prepareEgl(glLibrary, useNamespaceBypass, /* forceGles */ true, 3)) {
        Log.e("Eclipse", EclipseExec.lastError());
        return false;
    }
    EclipseExec.setUseBigCoreAffinity(true);
    return true;
}
```

`lastError()` returns the reason the last failing call failed, so the launcher
can show it instead of a bare "renderer failed".

## Licence

MIT, © 2026 Shadow.
