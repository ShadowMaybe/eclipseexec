package me.shadow.eclipselauncher.exec;

/**
 * Native graphics bootstrap for Eclipse Launcher — the Java half of
 * {@code libeclipseexec.so}.
 *
 * <p>Everything here configures the process <em>before</em> the game starts:
 * which OpenGL/Vulkan driver gets loaded, how the GL context is created, and
 * where the render thread is allowed to run. The native side publishes that
 * state; the SDL fork and the LWJGL hooks read it.</p>
 *
 * <h2>Typical use</h2>
 *
 * <pre>{@code
 * EclipseExec.setNativeLibraryDir(applicationInfo.nativeLibraryDir);
 *
 * if (!EclipseExec.prepareEgl(glLibraryPath, true, true, 3)) {
 *     Log.e("Eclipse", EclipseExec.lastError());
 *     return false;
 * }
 *
 * EclipseExec.setUseBigCoreAffinity(true);
 * }</pre>
 *
 * <h2>Ordering</h2>
 *
 * <ul>
 *   <li>{@link #setNativeLibraryDir(String)} must come before any call that
 *       uses the namespace bypass, including {@link #setUseTurnip(boolean)}
 *       followed by {@link #preloadVulkan()}.</li>
 *   <li>{@link #prepareEgl(String, boolean, boolean, int)} must succeed before
 *       the renderer asks for {@code eclipseexec_acq_egl_handle()}
 *       indirectly through SDL.</li>
 *   <li>The rest can be called in any order.</li>
 * </ul>
 *
 * <h2>Failure</h2>
 *
 * <p>Methods that can fail return a status and record a reason readable with
 * {@link #lastError()}. The message is per-thread and describes the most
 * recent failure on <em>this</em> thread — so read it immediately, before
 * doing anything else. It is also written to logcat under the
 * {@code EclipseExec} tag, which is usually the faster place to look.</p>
 *
 * <h2>Threading</h2>
 *
 * <p>Configuration is expected from the thread that sets the game up, before
 * the renderer starts. The native side locks what it publishes, so a later
 * reader on another thread sees a complete value; it does not promise that
 * two threads configuring simultaneously will both win.</p>
 *
 * <p>Loading: this class loads {@code libeclipseexec.so} in a static
 * initializer, so {@link UnsatisfiedLinkError} at first use means the library
 * is missing from the APK — or that R8 renamed this class, in which case the
 * fix is already in {@code consumer-rules.pro}.</p>
 */
public final class EclipseExec {

    static {
        System.loadLibrary("eclipseexec");
    }

    private EclipseExec() {
        // Static methods only.
    }

    /**
     * Loads the GL driver now and records how the context should be created.
     *
     * @param path              absolute path to the driver, or a name {@code dlopen}
     *                          can find.
     * @param useNamespaceBypass load through a private linker namespace instead of
     *                          a plain {@code dlopen()}. Required for drivers that
     *                          live outside the app's own library directory (Mesa,
     *                          gl4es, a Turnip build). Needs
     *                          {@link #setNativeLibraryDir(String)} first, and
     *                          Android 7.0 (API 24) — older releases have no
     *                          linker namespaces, and the call fails with
     *                          {@link #lastError()} saying so.
     * @param forceGles         {@code true} to request a GLES context rather than
     *                          desktop GL.
     * @param glesMajorVersion  GLES major version to request, or {@code 0} to let
     *                          the driver choose.
     * @return {@code true} if the driver loaded; otherwise {@link #lastError()}
     *         says why not.
     */
    public static native boolean prepareEgl(String path, boolean useNamespaceBypass,
                                            boolean forceGles, int glesMajorVersion);

    /**
     * Publishes the display geometry the renderer should assume.
     *
     * <p>Called once the surface size and refresh rate are known. Values are
     * stored as given; zero is passed through rather than corrected.</p>
     *
     * @param width          framebuffer width in pixels.
     * @param height         framebuffer height in pixels.
     * @param refreshRateHz  refresh rate; {@code 0} when the platform does not
     *                       report one.
     */
    public static native void setDisplayParams(int width, int height, float refreshRateHz);

    /**
     * Records where the GPU drivers live.
     *
     * <p>Normally {@code applicationInfo.nativeLibraryDir}. Required before the
     * namespace bypass can be used, because it becomes the search path of the
     * private linker namespace.</p>
     *
     * @param nativeLibraryDir absolute directory path; must not be null or empty.
     */
    public static native void setNativeLibraryDir(String nativeLibraryDir);

    /**
     * Selects the Turnip (Freedreno) driver instead of the system Vulkan
     * driver.
     *
     * <p>Takes effect on the next {@link #preloadVulkan()} or the next native
     * call to {@code eclipseexec_acq_vulkan_handle()}. On arm64-v8a builds;
     * other architectures ignore it and use the system loader.</p>
     *
     * @param enable {@code true} to select Turnip.
     */
    public static native void setUseTurnip(boolean enable);

    /**
     * Loads the selected Vulkan driver up front.
     *
     * <p>A no-op when Turnip was not selected. Doing this before the game
     * starts moves driver loading off the first frame; failing only logs, since
     * Mesa will fall back to the system driver on its own.</p>
     */
    public static native void preloadVulkan();

    /**
     * Turns the big-core affinity helper on or off.
     *
     * <p>On by default: calling it is itself the request. When enabled, the
     * first call from each thread pins that thread to the fastest CPU in the
     * device, once, and later calls do nothing.</p>
     *
     * @param enable {@code false} to leave thread placement alone.
     */
    public static native void setUseBigCoreAffinity(boolean enable);

    /**
     * Human-readable reason the last failing call on this thread failed.
     *
     * <p>The buffer is thread-local: it reports what happened on <em>this</em>
     * thread, so a failure on another thread never shows up here and never
     * overwrites this one. It is replaced when this thread fails again.</p>
     *
     * @return the message, or an empty string if nothing has failed on this
     *         thread yet. Read it immediately after the failure.
     */
    public static native String lastError();

    /**
     * The device's {@code SDK_INT}.
     *
     * @return the API level, or {@code 0} if it could not be determined. Used
     *         internally to skip features the platform does not have.
     */
    public static native int deviceApiLevel();

    /*
     * The JNI entry points behind the methods above live in src/eclipse_jni.c
     * and are bound in JNI_OnLoad with RegisterNatives(). Every name and
     * signature must match that table exactly — tools/check_jni_bindings.py
     * fails the build if they drift apart.
     *
     * The handle accessors (eclipseexec_acq_egl_handle and
     * eclipseexec_acq_vulkan_handle) are deliberately absent here: their
     * callers are the SDL fork and the LWJGL hooks, which are native code and
     * link against the C API in include/eclipseexec.h.
     */
}
