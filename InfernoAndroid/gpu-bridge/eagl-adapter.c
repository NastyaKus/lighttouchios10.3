/* SPDX-License-Identifier: GPL-3.0-or-later
 * Public Objective-C runtime ABI; no Apple headers/implementation included. */
#include "eagl-adapter.h"
#ifdef EGPU_GUEST_FREESTANDING
#undef NULL
#include "../guest/darwin-abi.h"
extern void *dlopen(const char *,int);
extern void *dlsym(void *,const char *);
#else
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#endif
typedef void *Obj, *Class, *Sel, *Ivar;
struct Super { Obj receiver;Class superclass; };
static struct {
    Class (*get_class)(const char *);
    Class (*allocate_class)(Class,const char *,size_t);
    void (*register_class)(Class);
    unsigned char (*add_ivar)(Class,const char *,size_t,uint8_t,const char *);
    unsigned char (*add_method)(Class,Sel,void *,const char *);
    Sel (*selector)(const char *);
    Ivar (*ivar)(Class,const char *);
    ptrdiff_t (*offset)(Ivar);
    void *msg;
    void *super_msg;
    Class (*object_class)(Obj);
} runtime;
static Class context_class,drawable_class,nsobject;
static ptrdiff_t context_offset,drawable_offset;
static char *channel_path;
static int broker_capability=-1;
static uint32_t surface_width,surface_height;
static int active_contexts;
static const char *failure="not installed";
static _Thread_local Obj current_object;
static Sel sel(const char *s) { return runtime.selector(s); }
static Obj message(Obj object,const char *name) { return ((Obj (*)(Obj,Sel))runtime.msg)(object,sel(name)); }
static EgGlesClient **storage(Obj object) { return (EgGlesClient **)((uint8_t *)object+context_offset); }
EgGlesClient *egEAGLClient(void *object) {
    return object && context_class && runtime.object_class(object)==context_class?*storage(object):NULL;
}
static Obj init_api(Obj self,Sel cmd,unsigned long api) {
    (void)cmd;
    if(api!=2 || __atomic_exchange_n(&active_contexts,1,__ATOMIC_ACQ_REL)) { message(self,"release");return NULL; }
    struct Super super={self,nsobject};
    self=((Obj (*)(struct Super *,Sel))runtime.super_msg)(&super,sel("init"));
    if(!self){__atomic_store_n(&active_contexts,0,__ATOMIC_RELEASE);return NULL;}
    EgGlesClient *client=broker_capability>=0?egGlesOpenBroker(channel_path,broker_capability,surface_width,surface_height):
        egGlesOpen(channel_path,surface_width,surface_height);
    if(!client){__atomic_store_n(&active_contexts,0,__ATOMIC_RELEASE);message(self,"release");return NULL;}
    *storage(self)=client;
    egGlesMakeCurrent(NULL); /* EAGL init does not make a context current. */
    return self;
}
static Obj init_sharegroup(Obj self,Sel cmd,unsigned long api,Obj group) {
    if(group){message(self,"release");return NULL;}
    return init_api(self,cmd,api);
}
static unsigned long api(Obj self,Sel cmd) { (void)self;(void)cmd;return 2; }
static Obj current(Obj self,Sel cmd) { (void)self;(void)cmd;return current_object; }
static signed char set_current(Obj cls,Sel cmd,Obj object) {
    (void)cls;(void)cmd;
    if(object==current_object)return 1;
    EgGlesClient *client=egEAGLClient(object);
    if(object && !client)return 0;
    if(!egGlesMakeCurrent(client))return 0;
    if(object)message(object,"retain");
    Obj old=current_object;current_object=object;
    if(old)message(old,"release");
    return 1;
}
static Obj sharegroup(Obj self,Sel cmd) { (void)self;(void)cmd;return NULL; }
static signed char present(Obj self,Sel cmd,unsigned int target) {
    (void)cmd;return self==current_object && target==GL_RENDERBUFFER && egGlesPresent();
}
static signed char drawable(Obj self,Sel cmd,unsigned int target,Obj layer) {
    (void)cmd;
    if(self!=current_object || target!=GL_RENDERBUFFER || !layer || runtime.object_class(layer)!=drawable_class)return 0;
    uint32_t *size=(uint32_t *)((uint8_t *)layer+drawable_offset);
    glRenderbufferStorage(target,GL_RGBA8_OES,size[0],size[1]);return glGetError()==GL_NO_ERROR;
}
void *egEAGLCreateDiagnosticDrawable(uint32_t width,uint32_t height) {
    if(!drawable_class || !width || !height || width>512 || height>512)return NULL;
    Obj object=message((Obj)drawable_class,"alloc");object=message(object,"init");if(!object)return NULL;
    uint32_t *size=(uint32_t *)((uint8_t *)object+drawable_offset);size[0]=width;size[1]=height;return object;
}
static void dealloc(Obj self,Sel cmd) {
    EgGlesClient *client=*storage(self);
    if(client) {
        *storage(self)=NULL;egGlesClose(client);
        __atomic_store_n(&active_contexts,0,__ATOMIC_RELEASE);
    }
    struct Super super={self,nsobject};((void (*)(struct Super *,Sel))runtime.super_msg)(&super,cmd);
}
const char *egEAGLError(void) { return failure; }
int egEAGLInstallBroker(const char *path,int fd,uint32_t width,uint32_t height) {
    if(context_class || fd<3)return 0;
    if(!egEAGLInstall(path,width,height))return 0;
    broker_capability=fd;return 1;
}
int egEAGLInstall(const char *image,uint32_t width,uint32_t height) {
    if(context_class)return 1;
    if(!width || !height || width>512 || height>512){failure="invalid offscreen dimensions";return 0;}
    void *foundation=dlopen("/System/Library/Frameworks/Foundation.framework/Foundation",1);
    if(!foundation)foundation=dlopen("/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation",1);
    if(!foundation)foundation=dlopen("/System/Library/Frameworks/Foundation.framework/Versions/A/Foundation",1);
    void *objc=dlopen("/usr/lib/libobjc.A.dylib",1);
    if(!foundation || !objc){failure="Foundation / Objective-C runtime unavailable";return 0;}
#define RESOLVE(field,name) do { *(void **)(&runtime.field)=dlsym(objc,name);if(!runtime.field){failure="missing " name;return 0;} } while(0)
    RESOLVE(get_class,"objc_getClass");RESOLVE(allocate_class,"objc_allocateClassPair");
    RESOLVE(register_class,"objc_registerClassPair");RESOLVE(add_ivar,"class_addIvar");
    RESOLVE(add_method,"class_addMethod");RESOLVE(selector,"sel_registerName");
    RESOLVE(ivar,"class_getInstanceVariable");RESOLVE(offset,"ivar_getOffset");
    RESOLVE(msg,"objc_msgSend");RESOLVE(super_msg,"objc_msgSendSuper");RESOLVE(object_class,"object_getClass");
#undef RESOLVE
    if(runtime.get_class("EAGLContext")){failure="native EAGLContext already exists; no replacement attempted";return 0;}
    nsobject=runtime.get_class("NSObject");
    if(!nsobject){failure="NSObject unavailable";return 0;}
    if(image) {
        size_t n=0;while(image[n] && n<=1024)n++;
        if(n>1024){failure="channel path too long";return 0;}
        channel_path=malloc(n+1);if(!channel_path){failure="allocation failed";return 0;}
        memcpy(channel_path,image,n+1);
    }
    Class candidate=runtime.allocate_class(nsobject,"EAGLContext",0);
    if(!candidate || !runtime.add_ivar(candidate,"_emberContext",sizeof(void *),3,"^v")) {failure="cannot allocate offscreen class/ivar";return 0;}
#define METHOD(cls,name,imp,types) do { if(!runtime.add_method(cls,sel(name),(void *)(imp),types)){failure="cannot add " name;return 0;} } while(0)
    METHOD(candidate,"initWithAPI:",init_api,"@24@0:8Q16");
    METHOD(candidate,"initWithAPI:sharegroup:",init_sharegroup,"@32@0:8Q16@24");
    METHOD(candidate,"API",api,"Q16@0:8");METHOD(candidate,"sharegroup",sharegroup,"@16@0:8");
    METHOD(candidate,"presentRenderbuffer:",present,"B24@0:8I16");
    METHOD(candidate,"renderbufferStorage:fromDrawable:",drawable,"B32@0:8I16@24");
    METHOD(candidate,"dealloc",dealloc,"v16@0:8");
    Class meta=runtime.object_class(candidate);
    METHOD(meta,"currentContext",current,"@16@0:8");
    METHOD(meta,"setCurrentContext:",set_current,"B24@0:8@16");
#undef METHOD
    Class draw=runtime.allocate_class(nsobject,"EmberDiagnosticDrawable",0);
    if(!draw || !runtime.add_ivar(draw,"_emberSize",8,2,"[2I]")){failure="cannot allocate diagnostic drawable";return 0;}
    runtime.register_class(draw);drawable_class=draw;
    drawable_offset=runtime.offset(runtime.ivar(draw,"_emberSize"));
    runtime.register_class(candidate);context_class=candidate;
    context_offset=runtime.offset(runtime.ivar(candidate,"_emberContext"));
    surface_width=width;surface_height=height;failure="";return 1;
}
