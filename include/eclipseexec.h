/*
 * eclipseexec — native graphics bootstrap for the Eclipse launcher.
 *
 * Copyright (c) 2026 Shadow.
 * SPDX-License-Identifier: MIT
 *
 * This header is the whole contract between the launcher and the linker. Every
 * consumer — the SDL fork, the LWJGL ndlopen hook, the launcher's own renderer
 * setup — reads the globals here instead of touching Android's loader itself.
 */
#ifndef ECLIPSEEXEC_H
#define ECLIPSEEXEC_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ECLIPSEEXEC_VERSION_MAJOR 0
#define ECLIPSEEXEC_VERSION_MINOR 1
#define ECLIPSEEXEC_VERSION_PATCH 0

/**
 * Render parameters published by the launcher and read by the GL/Vulkan layer.
 *
 * SDL's EGL setup reads this struct to decide between desktop GL and GLES and
 * which GLES major version to ask for; the windowing code reads the display
 * geometry when it has to pick a swap interval.
 */
typedef struct eclipseexec_renderspec {
    int force_gles_context;     /* non-zero: request a GLES context, not desktop GL */
    int override_major_version; /* GLES major version to request; 0 means "let the driver pick" */
    int disp_width;
    int disp_height;
    float disp_hz;
} eclipseexec_renderspec_t;

/** The current render parameters. Written before the game thread starts. */
extern eclipseexec_renderspec_t eclipseexec_renderspec;

/** The launcher's native library directory, or NULL if it was never set. */
extern const char *eclipseexec_native_dir;

/*
 * Configuration. The launcher calls these from Java, on the main thread,
 * before the game starts.
 */

/**
 * Record where the GPU drivers live. Required before any call that uses the
 * namespace bypass.
 *
 * Returns false and sets eclipseexec_last_error() on a null or empty path.
 */
bool eclipseexec_set_native_dir(const char *dir);

/** Publish the display geometry. Values are used as-is; zero is passed through. */
void eclipseexec_set_display_params(int width, int height, float hz);

/**
 * Load a GL driver now and record how the context should be created.
 *
 * @param path          absolute path to the driver, or a name dlopen can find.
 * @param use_bypass    load through a private linker namespace instead of
 *                      dlopen(). Needs eclipseexec_set_native_dir() first.
 * @param force_gles    request a GLES context rather than desktop GL.
 * @param gles_major    GLES major version to request; 0 for the default.
 * @return true if the driver loaded. On failure eclipseexec_last_error() says why.
 */
bool eclipseexec_prepare_egl(const char *path, bool use_bypass, bool force_gles, int gles_major);

/** Select the Turnip driver. Takes effect on the next preload or acquisition. */
void eclipseexec_set_use_turnip(bool enable);

/** Load the Turnip driver up front. A no-op when Turnip was not selected. */
void eclipseexec_preload_vulkan(void);

/**
 * Turn the big-core affinity helper on or default it off. It is on by default:
 * calling eclipseexec_make_bigcore_affine() is itself the request.
 */
void eclipseexec_set_affinity_enabled(bool enable);

/*
 * Consumption. These are the functions other native code links against.
 */

/** Handle to the GL driver selected by eclipseexec_prepare_egl(). */
void *eclipseexec_acq_egl_handle(void);

/** Handle to the Vulkan loader, or to Turnip when it was selected and loaded. */
void *eclipseexec_acq_vulkan_handle(void);

/** Move the calling thread onto the fastest CPU in the device. Once per thread. */
void eclipseexec_make_bigcore_affine(void);

/** Whether eclipseexec_make_bigcore_affine() will act when called. */
bool eclipseexec_affinity_enabled(void);

/*
 * Diagnostics.
 */

/**
 * Human-readable reason the last failing call failed, or "" if none.
 *
 * Thread-local: it describes what happened on the calling thread, so a
 * failure on another thread is neither visible here nor able to overwrite
 * this one. The message is replaced by the next failure on this thread —
 * read it straight after the call that failed.
 */
const char *eclipseexec_last_error(void);

/** Record a failure. Called from anywhere in the library. */
void eclipseexec_set_error(const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 1, 2)))
#endif
        ;

/** Clear the recorded error. */
void eclipseexec_clear_error(void);

/** Android SDK_INT of the running device, read from system properties. */
int eclipseexec_device_api_level(void);

#ifdef __cplusplus
}
#endif

#endif /* ECLIPSEEXEC_H */
