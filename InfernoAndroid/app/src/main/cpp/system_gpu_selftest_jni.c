/* SPDX-License-Identifier: GPL-3.0-or-later
 * Client side of the system GPU host self-test, loaded in the GPU lab's UI
 * process. It connects over loopback to :system_gpu exactly as a guest
 * broker would reach the forwarded port; it never loads EGL itself. */
#include <arpa/inet.h>
#include <jni.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "system/selftest.h"

typedef struct {
    JNIEnv *env;
    jobject listener;
    jmethodID method;
} Hold;
/* Same thread as nativeRun: the listener blocks until the window is captured. */
static int hold(void *user) {
    Hold *h = user;
    jboolean ok = (*h->env)->CallBooleanMethod(h->env, h->listener, h->method);
    if ((*h->env)->ExceptionCheck(h->env)) {
        (*h->env)->ExceptionClear(h->env);
        return 0;
    }
    return ok == JNI_TRUE;
}

JNIEXPORT jstring JNICALL Java_dev_embervm_android_SystemGpuSelfTest_nativeRun(JNIEnv *env, jclass cls, jint port,
                                                                              jbyteArray capability, jint frames,
                                                                              jobject listener) {
    (void)cls;
    char report[1024] = "passed=0\nstage=arguments\n";
    uint8_t key[48];
    if (port < 1 || port > 65535 || frames < 0 || frames > 600 || !capability ||
        (*env)->GetArrayLength(env, capability) != (jsize)sizeof(key))
        return (*env)->NewStringUTF(env, report);
    (*env)->GetByteArrayRegion(env, capability, 0, sizeof(key), (jbyte *)key);
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || connect(fd, (const struct sockaddr *)&address, sizeof(address))) {
        if (fd >= 0)
            close(fd);
        memset(key, 0, sizeof(key));
        snprintf(report, sizeof(report), "passed=0\nstage=connect\n");
        return (*env)->NewStringUTF(env, report);
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    Hold h = {env, listener, NULL};
    if (listener) {
        jclass type = (*env)->GetObjectClass(env, listener);
        h.method = type ? (*env)->GetMethodID(env, type, "presented", "()Z") : NULL;
        if (!h.method) {
            close(fd);
            memset(key, 0, sizeof(key));
            return NULL; /* NoSuchMethodError is pending */
        }
    }
    EgsSelfTest result;
    egs_selftest_run(fd, key, key + 32, (uint32_t)frames, listener ? hold : NULL, &h, &result);
    memset(key, 0, sizeof(key));
    if (!egs_selftest_format(&result, report, sizeof(report)))
        snprintf(report, sizeof(report), "passed=0\nstage=report\n");
    return (*env)->NewStringUTF(env, report);
}
