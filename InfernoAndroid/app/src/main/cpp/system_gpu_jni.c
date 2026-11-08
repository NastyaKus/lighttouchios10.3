/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real ES3 executor used by the persistent EGP2 service. One EGL owner worker;
 * no QEMU lock, no diagnostic timeout and no synthetic probe instructions. */
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include <android/native_window_jni.h>
#endif
#include "system/host.h"
typedef struct {
    EgsHost *host;
    pthread_t owner;
} Host;
static Host *active;
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static EGLNativeWindowType requested_window;
static uint64_t generation;
static EGLNativeWindowType acquire(void *unused, uint64_t *stamp) {
    (void)unused;
    pthread_mutex_lock(&window_lock);
    EGLNativeWindowType w = requested_window;
    *stamp = generation;
#ifdef __ANDROID__
    if (w)
        ANativeWindow_acquire(w);
#endif
    pthread_mutex_unlock(&window_lock);
    return w;
}
static void release(void *unused, EGLNativeWindowType w) {
    (void)unused;
#ifdef __ANDROID__
    if (w)
        ANativeWindow_release(w);
#else
    (void)w;
#endif
}
static void exception(JNIEnv *env, const char *text) {
    if (!(*env)->ExceptionCheck(env)) {
        jclass type = (*env)->FindClass(env, "java/lang/IllegalStateException");
        if (type)
            (*env)->ThrowNew(env, type, text);
    }
}
static Host *checked(JNIEnv *env, jlong token) {
    if (!active || token != 1 || !pthread_equal(active->owner, pthread_self())) {
        exception(env, "GPU backend handle/thread mismatch");
        return NULL;
    }
    return active;
}
JNIEXPORT jlong JNICALL Java_dev_embervm_android_SystemGpuNativeBackend_nativeOpen(JNIEnv *env, jclass cls) {
    (void)cls;
    if (active) {
        exception(env, "GPU backend already exists in this process");
        return 0;
    }
    Host *h = calloc(1, sizeof(*h));
    if (!h) {
        exception(env, "GPU host allocation");
        return 0;
    }
    char error[192] = {0};
    EgPresentOptions options = {.require_window = 1, .acquire_window = acquire, .release_window = release};
    h->host = egs_host_open(&options, error);
    if (!h->host) {
        free(h);
        exception(env, error);
        return 0;
    }
    h->owner = pthread_self();
    active = h;
    return 1;
}
JNIEXPORT jobjectArray JNICALL Java_dev_embervm_android_SystemGpuNativeBackend_nativeInfo(JNIEnv *env, jclass cls,
                                                                                          jlong token) {
    (void)cls;
    Host *h = checked(env, token);
    if (!h)
        return NULL;
    jclass string = (*env)->FindClass(env, "java/lang/String");
    if (!string)
        return NULL;
    jobjectArray out = (*env)->NewObjectArray(env, 2, string, NULL);
    if (!out)
        return NULL;
    char number[16];
    snprintf(number, sizeof(number), "%u", egs_host_texture_limit(h->host));
    jstring limit = (*env)->NewStringUTF(env, number), renderer = (*env)->NewStringUTF(env, egs_host_renderer(h->host));
    if (!limit || !renderer)
        return NULL;
    (*env)->SetObjectArrayElement(env, out, 0, limit);
    (*env)->SetObjectArrayElement(env, out, 1, renderer);
    (*env)->DeleteLocalRef(env, limit);
    (*env)->DeleteLocalRef(env, renderer);
    return out;
}
JNIEXPORT void JNICALL Java_dev_embervm_android_SystemGpuNativeBackend_nativeCreate(JNIEnv *env, jclass cls,
                                                                                    jlong token, jlong id, jlong share,
                                                                                    jint w, jint height) {
    (void)cls;
    Host *h = checked(env, token);
    if (!h)
        return;
    if (id < 1 || id > UINT32_MAX || share < 0 || share > UINT32_MAX || w < 1 || height < 1 ||
        !egs_host_create(h->host, (uint32_t)id, (uint32_t)share, (uint32_t)w, (uint32_t)height))
        exception(env, egs_host_error(h->host));
}
JNIEXPORT void JNICALL Java_dev_embervm_android_SystemGpuNativeBackend_nativeDestroy(JNIEnv *env, jclass cls,
                                                                                     jlong token, jlong id) {
    (void)cls;
    Host *h = checked(env, token);
    if (!h)
        return;
    if (id < 1 || id > UINT32_MAX || !egs_host_destroy(h->host, (uint32_t)id))
        exception(env, egs_host_error(h->host));
}
JNIEXPORT jbyteArray JNICALL Java_dev_embervm_android_SystemGpuNativeBackend_nativeCall(JNIEnv *env, jclass cls,
                                                                                        jlong token, jlong id,
                                                                                        jbyteArray command,
                                                                                        jbyteArray upload) {
    (void)cls;
    Host *h = checked(env, token);
    if (!h)
        return NULL;
    if (!command || id < 1 || id > UINT32_MAX) {
        exception(env, "Invalid native GPU command");
        return NULL;
    }
    jsize n = (*env)->GetArrayLength(env, command), bytes = upload ? (*env)->GetArrayLength(env, upload) : 0;
    if (n < 4 || bytes < 0 || (uint32_t)n > EGS_MAX_COMMAND_BYTES || (uint32_t)bytes > EGS_ARRAY_LIMIT) {
        exception(env, "GPU command/upload bound");
        return NULL;
    }
    uint8_t *p = malloc((size_t)n), *data = bytes ? malloc((size_t)bytes) : NULL;
    if (!p || (bytes && !data)) {
        free(p);
        free(data);
        exception(env, "GPU command staging allocation");
        return NULL;
    }
    (*env)->GetByteArrayRegion(env, command, 0, n, (jbyte *)p);
    if (bytes)
        (*env)->GetByteArrayRegion(env, upload, 0, bytes, (jbyte *)data);
    if ((*env)->ExceptionCheck(env)) {
        free(p);
        free(data);
        return NULL;
    }
    EgsReply r = {0};
    int ok = egs_host_call(h->host, (uint32_t)id, p, (uint32_t)n, data, (uint32_t)bytes, &r);
    free(p);
    free(data);
    if (!ok) {
        egs_reply_close(&r);
        exception(env, egs_host_error(h->host));
        return NULL;
    }
    jbyteArray out = (*env)->NewByteArray(env, (jsize)r.size);
    if (out && r.size)
        (*env)->SetByteArrayRegion(env, out, 0, (jsize)r.size, (const jbyte *)r.data);
    egs_reply_close(&r);
    return out;
}
JNIEXPORT jint JNICALL Java_dev_embervm_android_SystemGpuNativeBackend_nativePresent(JNIEnv *env, jclass cls,
                                                                                     jlong token, jlong id,
                                                                                     jlong surface, jlong serial) {
    (void)cls;
    Host *h = checked(env, token);
    if (!h)
        return 2;
    int status = 2;
    if (id < 1 || id > UINT32_MAX || surface < 1 || serial < 1 ||
        !egs_host_present(h->host, (uint32_t)id, (uint64_t)surface, (uint64_t)serial, &status))
        exception(env, egs_host_error(h->host));
    return status;
}
JNIEXPORT void JNICALL Java_dev_embervm_android_SystemGpuNativeBackend_nativeClose(JNIEnv *env, jclass cls,
                                                                                   jlong token) {
    (void)cls;
    Host *h = checked(env, token);
    if (!h)
        return;
    egs_host_close(h->host);
    free(h);
    active = NULL;
}
JNIEXPORT void JNICALL Java_dev_embervm_android_SystemGpuNativeBackend_nativeSurface(JNIEnv *env, jclass cls,
                                                                                     jobject surface, jlong stamp) {
    (void)cls;
    if (stamp < 0) {
        exception(env, "Invalid GPU Surface generation");
        return;
    }
    EGLNativeWindowType w = (EGLNativeWindowType)0;
#ifdef __ANDROID__
    if (surface)
        w = ANativeWindow_fromSurface(env, surface);
#else
    (void)surface;
#endif
    pthread_mutex_lock(&window_lock);
    EGLNativeWindowType old = requested_window;
    if ((uint64_t)stamp >= generation) {
        requested_window = w;
        generation = (uint64_t)stamp;
    } else
        old = w;
    pthread_mutex_unlock(&window_lock);
    release(NULL, old);
}
