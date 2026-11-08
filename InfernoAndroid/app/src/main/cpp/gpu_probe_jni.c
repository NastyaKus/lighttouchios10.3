/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <jni.h>
#include <stdatomic.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#ifdef __ANDROID__
#include <android/native_window_jni.h>
#endif
#include "file-host.h"

/* One invocation per dedicated process. Cancellation is also observed before
 * the first poll; a stuck vendor GL call is contained by killing :gpu. */
static atomic_int cancelled;
static atomic_int require_window;
static pthread_mutex_t presentation_lock=PTHREAD_MUTEX_INITIALIZER;
static EGLNativeWindowType requested_window;
static uint64_t requested_generation;
static EgPresentStats last_presentation;
static EGLNativeWindowType window_snapshot(void *opaque,uint64_t *generation) {
    (void)opaque;pthread_mutex_lock(&presentation_lock);
    EGLNativeWindowType window=requested_window;*generation=requested_generation;
#ifdef __ANDROID__
    if(window)ANativeWindow_acquire(window);
#endif
    pthread_mutex_unlock(&presentation_lock);return window;
}
static void window_release(void *opaque,EGLNativeWindowType window) {
    (void)opaque;
#ifdef __ANDROID__
    if(window)ANativeWindow_release(window);
#else
    (void)window;
#endif
}
static void presentation_update(void *opaque,const EgPresentStats *stats) {
    (void)opaque;pthread_mutex_lock(&presentation_lock);last_presentation=*stats;pthread_mutex_unlock(&presentation_lock);
}
JNIEXPORT void JNICALL Java_dev_embervm_android_GpuHostNative_nativePresentationMode(JNIEnv *env,jclass cls,jboolean required) {
    (void)env;(void)cls;atomic_store(&require_window,required);
}
JNIEXPORT void JNICALL Java_dev_embervm_android_GpuHostNative_nativeSetSurface(JNIEnv *env,jclass cls,jobject surface,jlong generation) {
    (void)cls;EGLNativeWindowType window=(EGLNativeWindowType)0;
#ifdef __ANDROID__
    if(surface)window=ANativeWindow_fromSurface(env,surface);
#else
    (void)env;(void)surface;
#endif
    pthread_mutex_lock(&presentation_lock);
    EGLNativeWindowType old=requested_window;
    if((uint64_t)generation>=requested_generation){requested_window=window;requested_generation=generation;}
    else {old=window;}
    pthread_mutex_unlock(&presentation_lock);window_release(NULL,old);
}
JNIEXPORT jlongArray JNICALL Java_dev_embervm_android_GpuHostNative_nativePresentationStats(JNIEnv *env,jclass cls) {
    (void)cls;pthread_mutex_lock(&presentation_lock);EgPresentStats s=last_presentation;pthread_mutex_unlock(&presentation_lock);
    jlong v[14]={(jlong)s.frames,(jlong)s.window_frames,(jlong)s.generation,(jlong)s.surface_creates,(jlong)s.surface_losses,(jlong)s.last_serial,
        s.pixels[0],s.pixels[1],s.pixels[2],s.pixels[3],s.width,s.height,(jlong)s.sample_pixels,(jlong)s.cpu_upload_bytes};
    jlongArray out=(*env)->NewLongArray(env,14);if(out)(*env)->SetLongArrayRegion(env,out,0,14,v);return out;
}
static int is_cancelled(void *unused) { (void)unused;return atomic_load(&cancelled); }
JNIEXPORT void JNICALL Java_dev_embervm_android_GpuHostNative_nativeCancel(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;atomic_store(&cancelled,1);
}
JNIEXPORT jobjectArray JNICALL Java_dev_embervm_android_GpuHostNative_nativeRun(
        JNIEnv *env,jclass cls,jstring image,jstring log,jint timeout) {
    (void)cls;
    const char *path=(*env)->GetStringUTFChars(env,image,NULL);
    if(!path)return NULL;
    const char *output=(*env)->GetStringUTFChars(env,log,NULL);
    if(!output){(*env)->ReleaseStringUTFChars(env,image,path);return NULL;}
    EgpuHostResult r={0};
    int fd=open(output,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)snprintf(r.error,sizeof(r.error),"Cannot create new GPU diagnostic log");
    else {
        fflush(stdout);fflush(stderr);
        if(dup2(fd,STDOUT_FILENO)<0 || dup2(fd,STDERR_FILENO)<0)
            snprintf(r.error,sizeof(r.error),"Cannot redirect GPU diagnostic output");
        else {
            EgPresentOptions options={.require_window=atomic_load(&require_window),.acquire_window=window_snapshot,
                .release_window=window_release,.publish=presentation_update};
            egpu_file_host_ex(path,(uint32_t)timeout,is_cancelled,NULL,&r,&options);
        }
        fflush(stdout);fflush(stderr);close(fd);
    }
    (*env)->ReleaseStringUTFChars(env,image,path);(*env)->ReleaseStringUTFChars(env,log,output);
    jclass string=(*env)->FindClass(env,"java/lang/String");if(!string)return NULL;
    jobjectArray array=(*env)->NewObjectArray(env,12,string,NULL);if(!array)return NULL;
    uint64_t counters[]={r.commands,r.draws,r.pixels_read,r.api_calls,r.api_batches,r.api_draws,r.shader_compiles};
    const char *text[]={r.success?"1":"0",r.vendor,r.renderer,r.version,r.error};
    for(int i=0;i<12;i++) {
        char number[32];const char *value;
        if(i<5)value=text[i];
        else {snprintf(number,sizeof(number),"%llu",(unsigned long long)counters[i-5]);value=number;}
        jstring item=(*env)->NewStringUTF(env,value);if(!item)return NULL;
        (*env)->SetObjectArrayElement(env,array,i,item);(*env)->DeleteLocalRef(env,item);
        if((*env)->ExceptionCheck(env))return NULL;
    }
    return array;
}
