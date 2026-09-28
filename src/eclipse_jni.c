/*
 * eclipseexec — the Java surface.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 *
 * Every native method is bound in one place, here, instead of being matched
 * against a mangled name at load time. Two things fall out of that:
 *
 *   - the exported symbol list in version.script stays tiny;
 *   - a broken binding fails JNI_OnLoad with a message that says which class
 *     and how many methods were involved, instead of throwing
 *     UnsatisfiedLinkError on the first call from a renderer thread.
 *
 * The one thing RegisterNatives cannot survive is the class being renamed by
 * R8, which is why jni_bindings/consumer-rules.pro keeps it — and why the
 * failure message below mentions that file by name.
 */

#define ECLIPSE_LOG_TAG "EclipseExec/jni"
#include "eclipseexec.h"
#include "eclipse_log.h"

#include <jni.h>
#include <stdbool.h>
#include <string.h>

#define ECLIPSE_JNI_CLASS "me/shadow/eclipselauncher/exec/EclipseExec"

/* ---- helpers ------------------------------------------------------------ */

static const char *take_jstring(JNIEnv *env, jstring value)
{
    if (value == NULL) {
        return NULL;
    }
    /* GetStringUTFChars returns NULL and raises OutOfMemoryError on failure;
     * the caller checks the pointer and lets the exception propagate. */
    return (*env)->GetStringUTFChars(env, value, NULL);
}

static void release_jstring(JNIEnv *env, jstring value, const char *chars)
{
    if (chars != NULL) {
        (*env)->ReleaseStringUTFChars(env, value, chars);
    }
}

/* ---- native methods ----------------------------------------------------- */

static jboolean native_prepare_egl(JNIEnv *env, jclass clazz, jstring jpath,
                                   jboolean use_bypass, jboolean force_gles, jint gles_major)
{
    (void)clazz;

    const char *path = take_jstring(env, jpath);
    if (path == NULL) {
        eclipseexec_set_error("prepareEgl: path is null");
        return JNI_FALSE;
    }

    bool ok = eclipseexec_prepare_egl(path, use_bypass == JNI_TRUE,
                                      force_gles == JNI_TRUE, (int)gles_major);
    release_jstring(env, jpath, path);
    return ok ? JNI_TRUE : JNI_FALSE;
}

static void native_set_display_params(JNIEnv *env, jclass clazz,
                                      jint width, jint height, jfloat hz)
{
    (void)env;
    (void)clazz;
    eclipseexec_set_display_params((int)width, (int)height, (float)hz);
}

static void native_set_native_library_dir(JNIEnv *env, jclass clazz, jstring jdir)
{
    (void)clazz;

    const char *dir = take_jstring(env, jdir);
    if (dir == NULL) {
        eclipseexec_set_error("setNativeLibraryDir: argument is null");
        return;
    }

    (void)eclipseexec_set_native_dir(dir);
    release_jstring(env, jdir, dir);
}

static void native_set_use_turnip(JNIEnv *env, jclass clazz, jboolean enable)
{
    (void)env;
    (void)clazz;
    eclipseexec_set_use_turnip(enable == JNI_TRUE);
}

static void native_preload_vulkan(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    eclipseexec_preload_vulkan();
}

static void native_set_affinity(JNIEnv *env, jclass clazz, jboolean enable)
{
    (void)env;
    (void)clazz;
    eclipseexec_set_affinity_enabled(enable == JNI_TRUE);
}

static jstring native_last_error(JNIEnv *env, jclass clazz)
{
    (void)clazz;
    const char *message = eclipseexec_last_error();
    return (*env)->NewStringUTF(env, message != NULL ? message : "");
}

static jint native_device_api_level(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    return (jint)eclipseexec_device_api_level();
}

/* The table is the contract with jni_bindings/.../EclipseExec.java. Names and
 * descriptors must match that file exactly. */
static const JNINativeMethod g_methods[] = {
    {"prepareEgl",         "(Ljava/lang/String;ZZI)Z", (void *)native_prepare_egl},
    {"setDisplayParams",   "(IIF)V",                   (void *)native_set_display_params},
    {"setNativeLibraryDir","(Ljava/lang/String;)V",    (void *)native_set_native_library_dir},
    {"setUseTurnip",       "(Z)V",                     (void *)native_set_use_turnip},
    {"preloadVulkan",      "()V",                      (void *)native_preload_vulkan},
    {"setUseBigCoreAffinity", "(Z)V",                  (void *)native_set_affinity},
    {"lastError",          "()Ljava/lang/String;",     (void *)native_last_error},
    {"deviceApiLevel",     "()I",                      (void *)native_device_api_level},
};

/* ---- loading ------------------------------------------------------------ */

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;

    JNIEnv *env = NULL;
    jint status = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (status != JNI_OK || env == NULL) {
        ECLIPSE_LOGE("GetEnv returned %d; cannot bind native methods", (int)status);
        return JNI_ERR;
    }

    jclass clazz = (*env)->FindClass(env, ECLIPSE_JNI_CLASS);
    if (clazz == NULL) {
        /* A missing class means either the class was not packaged or R8
         * renamed it. Clear the pending exception: returning JNI_ERR with one
         * still pending makes loadLibrary report something unrelated. */
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        ECLIPSE_LOGE("class %s not found. If the app is minified, keep it — "
                     "jni_bindings/consumer-rules.pro already contains the rule.",
                     ECLIPSE_JNI_CLASS);
        return JNI_ERR;
    }

    jint method_count = (jint)(sizeof(g_methods) / sizeof(g_methods[0]));
    if ((*env)->RegisterNatives(env, clazz, g_methods, method_count) != 0) {
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        ECLIPSE_LOGE("RegisterNatives failed for %s (%d methods) — a name or "
                     "descriptor in g_methods no longer matches the Java class",
                     ECLIPSE_JNI_CLASS, (int)method_count);
        (*env)->DeleteLocalRef(env, clazz);
        return JNI_ERR;
    }

    (*env)->DeleteLocalRef(env, clazz);
    ECLIPSE_LOGI("eclipseexec %d.%d.%d bound to %s",
                 ECLIPSEEXEC_VERSION_MAJOR, ECLIPSEEXEC_VERSION_MINOR,
                 ECLIPSEEXEC_VERSION_PATCH, ECLIPSE_JNI_CLASS);
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved)
{
    (void)vm;
    (void)reserved;
    /* No JNI calls here: the class is going away and the loader may already
     * have torn down the state we would touch. */
    ECLIPSE_LOGI("eclipseexec unloading");
}
