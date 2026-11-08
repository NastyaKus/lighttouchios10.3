/* SPDX-License-Identifier: GPL-3.0-or-later
 * Apple's real EAGLContext, not the diagnostic replacement class from EGP1.
 * IMPs stay function pointers throughout (never stored through void **).
 * All encodings/cache identity are checked before any method is changed.
 * Hooks currently forward unchanged: full GLI/IOSurface backend is pending. */
#include "eagl-native.h"
#include "../../guest/darwin-abi.h"
#if !__has_feature(ptrauth_calls)
#error System EAGL interposition requires an arm64e compiler with ptrauth calls
#endif
extern void *dlopen(const char *,int),*dlsym(void *,const char *);
typedef void *Obj,*Sel,*Method;
typedef Obj (*Imp)(Obj,Sel,...);
// Objective-C intentionally erases the concrete method function type in IMP.
// The exact encodings below are checked first; each wrapper casts back to that
// method's actual ABI, including BOOL and NSUInteger returns.
#pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
static struct {
    Obj (*cls)(const char *),(*meta)(Obj);
    Sel (*sel)(const char *);
    Method (*method)(Obj,Sel);
    const char *(*encoding)(Method);
    Imp (*get)(Method),(*set)(Method,Imp);
} rt;
static const unsigned char cache_id[16]={0x11,0xad,0x39,0xc9,0xe3,0xc9,0x30,0x67,0xa3,0x37,0x72,0x87,0x74,0x4f,0xdc,0xb5};
static struct Hook {const char *name,*type;unsigned meta;Imp replacement,original;Method method;uint64_t calls;} hooks[EG_NATIVE_HOOKS];
static unsigned installed;
static const char *failure="not installed";
#define HIT(n) __atomic_add_fetch(&hooks[n].calls,1,__ATOMIC_RELAXED)
static Obj init_api(Obj self,Sel cmd,unsigned long api) {
    HIT(EG_NATIVE_INIT);return ((Obj(*)(Obj,Sel,unsigned long))hooks[EG_NATIVE_INIT].original)(self,cmd,api);
}
static Obj init_props(Obj self,Sel cmd,unsigned long api,Obj props) {
    HIT(EG_NATIVE_PROPERTIES);return ((Obj(*)(Obj,Sel,unsigned long,Obj))hooks[EG_NATIVE_PROPERTIES].original)(self,cmd,api,props);
}
static Obj common_init(Obj self,Sel cmd,unsigned long api,Obj props) {
    HIT(EG_NATIVE_COMMON);return ((Obj(*)(Obj,Sel,unsigned long,Obj))hooks[EG_NATIVE_COMMON].original)(self,cmd,api,props);
}
static unsigned long get_api(Obj self,Sel cmd) {
    HIT(EG_NATIVE_API);return ((unsigned long(*)(Obj,Sel))hooks[EG_NATIVE_API].original)(self,cmd);
}
static void *get_macro(Obj self,Sel cmd) {
    HIT(EG_NATIVE_MACRO);return ((void*(*)(Obj,Sel))hooks[EG_NATIVE_MACRO].original)(self,cmd);
}
static Obj get_current(Obj self,Sel cmd) {
    HIT(EG_NATIVE_CURRENT);return ((Obj(*)(Obj,Sel))hooks[EG_NATIVE_CURRENT].original)(self,cmd);
}
static unsigned char set_current(Obj self,Sel cmd,Obj current) {
    HIT(EG_NATIVE_SET_CURRENT);return ((unsigned char(*)(Obj,Sel,Obj))hooks[EG_NATIVE_SET_CURRENT].original)(self,cmd,current);
}
const char *egNativeEaglError(void){return failure;}
void egNativeEaglStats(EgNativeStats *out) {
    if(!out)return;
    out->installed=installed;
    for(unsigned i=0;i<EG_NATIVE_HOOKS;i++)out->calls[i]=__atomic_load_n(&hooks[i].calls,__ATOMIC_RELAXED);
}
int egNativeEaglInstall(void) {
    if(installed){failure="already installed";return 0;}
    int (*uuid)(unsigned char *)=(int(*)(unsigned char *))dlsym((void *)(long)-2,"_dyld_get_shared_cache_uuid");
    unsigned char actual[16];
    if(!uuid || !uuid(actual) || memcmp(actual,cache_id,16)){failure="requires exact 18A5351d stock System cache";return 0;}
    if(!dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES",2|4)){failure="native OpenGLES unavailable";return 0;}
    void *objc=dlopen("/usr/lib/libobjc.A.dylib",2|4);if(!objc){failure="Objective-C runtime unavailable";return 0;}
#define RESOLVE(member,type,name) do {rt.member=(type)dlsym(objc,name);if(!rt.member){failure="missing " name;return 0;}}while(0)
    RESOLVE(cls,Obj(*)(const char *),"objc_getClass");RESOLVE(meta,Obj(*)(Obj),"object_getClass");
    RESOLVE(sel,Sel(*)(const char *),"sel_registerName");RESOLVE(method,Method(*)(Obj,Sel),"class_getInstanceMethod");
    RESOLVE(encoding,const char *(*)(Method),"method_getTypeEncoding");
    RESOLVE(get,Imp(*)(Method),"method_getImplementation");RESOLVE(set,Imp(*)(Method,Imp),"method_setImplementation");
#undef RESOLVE
    Obj cls=rt.cls("EAGLContext");if(!cls){failure="Apple EAGLContext absent";return 0;}
    hooks[EG_NATIVE_INIT]=(struct Hook){"initWithAPI:","@24@0:8Q16",0,(Imp)init_api,0,0,0};
    hooks[EG_NATIVE_PROPERTIES]=(struct Hook){"initWithAPI:properties:","@32@0:8Q16@24",0,(Imp)init_props,0,0,0};
    hooks[EG_NATIVE_COMMON]=(struct Hook){"commonInitWithAPI:properties:","@32@0:8Q16@24",0,(Imp)common_init,0,0,0};
    hooks[EG_NATIVE_API]=(struct Hook){"API","Q16@0:8",0,(Imp)get_api,0,0,0};
    hooks[EG_NATIVE_MACRO]=(struct Hook){"getMacroContextPrivate","^{EAGLMacroContext=}16@0:8",0,(Imp)get_macro,0,0,0};
    hooks[EG_NATIVE_CURRENT]=(struct Hook){"currentContext","@16@0:8",1,(Imp)get_current,0,0,0};
    hooks[EG_NATIVE_SET_CURRENT]=(struct Hook){"setCurrentContext:","B24@0:8@16",1,(Imp)set_current,0,0,0};
    // Preflight everything. The original IMP is populated before exposing any
    // wrapper, so a concurrent call always has a valid unchanged destination.
    for(unsigned i=0;i<EG_NATIVE_HOOKS;i++) {
        struct Hook *h=&hooks[i];h->method=rt.method(h->meta?rt.meta(cls):cls,rt.sel(h->name));
        const char *type=h->method?rt.encoding(h->method):0;
        if(!type || strncmp(type,h->type,256)){failure="native method encoding mismatch";return 0;}
        h->original=rt.get(h->method);if(!h->original){failure="native method IMP absent";return 0;}
    }
    for(unsigned i=0;i<EG_NATIVE_HOOKS;i++) {
        Imp old=rt.set(hooks[i].method,hooks[i].replacement);
        if(old!=hooks[i].original) {
            // Somebody raced the preflight. Restore their method too; never
            // leave our wrappers partially installed or discard their IMP.
            rt.set(hooks[i].method,old);
            for(unsigned j=0;j<i;j++)if(rt.get(hooks[j].method)==hooks[j].replacement)rt.set(hooks[j].method,hooks[j].original);
            failure="method changed during installation";return 0;
        }
    }
    installed=1;failure="";return 1;
}
int egNativeEaglRemove(void) {
    if(!installed){failure="not installed";return 0;}
    for(unsigned i=0;i<EG_NATIVE_HOOKS;i++)if(rt.get(hooks[i].method)!=hooks[i].replacement) {
        failure="refusing to overwrite an external method change";return 0;
    }
    for(unsigned i=0;i<EG_NATIVE_HOOKS;i++)rt.set(hooks[i].method,hooks[i].original);
    installed=0;failure="";return 1;
}
