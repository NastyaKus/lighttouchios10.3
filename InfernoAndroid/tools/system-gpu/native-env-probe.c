/* SPDX-License-Identifier: GPL-3.0-or-later
 * Inspect Apple's real framework/runtime availability. Never registers our
 * EAGL class, patches a method, draws a test triangle or changes an APFS disk. */
#include "../../guest/darwin-abi.h"
#include "../../gpu-bridge/system/eagl-native.h"
extern void *dlopen(const char *,int);
extern void *dlsym(void *,const char *);
extern const char *dlerror(void);
typedef void *Obj;
typedef void *Sel;
int main(void) {
    int (*cache_uuid)(unsigned char *)=dlsym((void *)(long)-2,"_dyld_get_shared_cache_uuid");
    if(cache_uuid) {
        unsigned char uuid[16];if(cache_uuid(uuid)) {
            char text[33];const char hex[]="0123456789abcdef";
            for(int i=0;i<16;i++){text[i*2]=hex[uuid[i]>>4];text[i*2+1]=hex[uuid[i]&15];}text[32]=0;
            dprintf(1,"*** EMBER-ENV: runtime_cache_uuid=%s\n",text);
        }
    }
    const char *paths[]={
        "/System/Library/Frameworks/Foundation.framework/Foundation",
        "/usr/local/lib/liblinkguard.dylib",
        "/System/Library/PrivateFrameworks/StudyLog.framework/StudyLog",
        "/System/Library/Frameworks/QuartzCore.framework/QuartzCore",
        "/System/Library/Frameworks/OpenGLES.framework/OpenGLES",
        "/System/Library/Frameworks/IOSurface.framework/IOSurface",
        "/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer"
    };
    int available=0,gles=0;
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++) {
        dlerror();void *handle=dlopen(paths[i],2|4);
        if(handle){available++;if(i==4)gles=1;dprintf(1,"*** EMBER-ENV: framework=available path=%s\n",paths[i]);}
        else {const char *error=dlerror();dprintf(1,"*** EMBER-ENV: framework=missing path=%s error=%.1024s\n",paths[i],error?error:"no dlerror");}
    }
    if(gles) {
        void *objc=dlopen("/usr/lib/libobjc.A.dylib",2|4);
        if(!objc)return 1;
        Obj (*getclass)(const char *)=dlsym(objc,"objc_getClass");
        Sel (*selector)(const char *)=dlsym(objc,"sel_registerName");
        void *msg=dlsym(objc,"objc_msgSend");
        void *(*method)(Obj,Sel)=dlsym(objc,"class_getInstanceMethod");
        const char *(*encoding)(void *)=dlsym(objc,"method_getTypeEncoding");
        if(!objc || !getclass || !selector || !msg || !method || !encoding)return 1;
        Obj cls=getclass("EAGLContext");dprintf(1,"*** EMBER-ENV: native_EAGLContext=%s\n",cls?"present":"absent");
        if(cls) {
            const char *names[]={"initWithAPI:","initWithAPI:properties:","getMacroContextPrivate","API","setParameter:to:"};
            for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
                void *m=method(cls,selector(names[i]));const char *type=m?encoding(m):NULL;
                dprintf(1,"*** EMBER-ENV: EAGL_method=%s encoding=%s\n",names[i],type?type:"MISSING");
            }
            Obj context=((Obj(*)(Obj,Sel))msg)(cls,selector("alloc"));
            dprintf(1,"*** EMBER-ENV: before_native_init_API3\n");
            context=((Obj(*)(Obj,Sel,unsigned long))msg)(context,selector("initWithAPI:"),3);
            dprintf(1,"*** EMBER-ENV: native_init_API3=%s\n",context?"object":"nil");
            if(context) {
                unsigned long api=((unsigned long(*)(Obj,Sel))msg)(context,selector("API"));
                void *macro=((void*(*)(Obj,Sel))msg)(context,selector("getMacroContextPrivate"));
                dprintf(1,"*** EMBER-ENV: native_API=%lu macro=%s\n",api,macro?"present":"nil");
                ((void(*)(Obj,Sel))msg)(context,selector("release"));
            }
            int (*install)(void)=(int(*)(void))dlsym((void *)(long)-2,"egNativeEaglInstall");
            int (*remove)(void)=(int(*)(void))dlsym((void *)(long)-2,"egNativeEaglRemove");
            void (*stats)(EgNativeStats *)=(void(*)(EgNativeStats *))dlsym((void *)(long)-2,"egNativeEaglStats");
            const char *(*error)(void)=(const char*(*)(void))dlsym((void *)(long)-2,"egNativeEaglError");
            if(install || remove || stats || error) {
                if(!install || !remove || !stats || !error)return 1;
                typedef Obj (*Imp)(Obj,Sel,...);
                Imp (*get)(void *)=(Imp(*)(void *))dlsym(objc,"method_getImplementation");
                void *init_method=method(cls,selector("initWithAPI:"));
                Imp original=get(init_method);
                if(!install()){dprintf(1,"*** EMBER-ENV: FAIL native_hook=%s\n",error());return 1;}
                if(get(init_method)==original || install())return 1;
                context=((Obj(*)(Obj,Sel))msg)(cls,selector("alloc"));
                context=((Obj(*)(Obj,Sel,unsigned long))msg)(context,selector("initWithAPI:"),3);
                if(context)((void(*)(Obj,Sel))msg)(context,selector("release"));
                Obj current=((Obj(*)(Obj,Sel))msg)(cls,selector("currentContext"));
                if(current || !((unsigned char(*)(Obj,Sel,Obj))msg)(cls,selector("setCurrentContext:"),NULL))return 1;
                EgNativeStats before,after;stats(&before);
                if(!before.installed || before.calls[EG_NATIVE_INIT]!=1 || !before.calls[EG_NATIVE_COMMON] ||
                   !before.calls[EG_NATIVE_CURRENT] || !before.calls[EG_NATIVE_SET_CURRENT])return 1;
                if(!remove() || get(init_method)!=original || remove())return 1;
                ((Obj(*)(Obj,Sel))msg)(cls,selector("currentContext"));stats(&after);
                if(after.installed || after.calls[EG_NATIVE_CURRENT]!=before.calls[EG_NATIVE_CURRENT])return 1;
                dprintf(1,"*** EMBER-EAGL: PASS native_class interpose forward restore\n");
            }
        }
    }
    dprintf(1,"*** EMBER-ENV: done frameworks=%d/7 system_gpu_verified=0\n",available);return 0;
}
