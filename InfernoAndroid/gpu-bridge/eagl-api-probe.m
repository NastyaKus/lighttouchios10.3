/* SPDX-License-Identifier: GPL-3.0-or-later
 * Public method declarations only, deliberately no Apple SDK required. */
#include "eagl-adapter.h"
#ifdef EGPU_BROKER_PROBE
#include "broker-client-probe.h"
#endif
#ifdef EGPU_PRESENT_PROBE
#include "present-probe.h"
#endif
#undef NULL
#include "../guest/darwin-abi.h"
typedef _Bool BOOL;
typedef unsigned long NSUInteger;
@interface NSObject
+ (id)alloc;
- (id)retain;
- (void)release;
@end
@interface EAGLContext:NSObject
- (id)initWithAPI:(NSUInteger)api;
- (id)initWithAPI:(NSUInteger)api sharegroup:(id)group;
+ (BOOL)setCurrentContext:(EAGLContext *)context;
+ (EAGLContext *)currentContext;
- (NSUInteger)API;
- (id)sharegroup;
- (BOOL)presentRenderbuffer:(unsigned int)target;
- (BOOL)renderbufferStorage:(unsigned int)target fromDrawable:(id)layer;
@end
extern Class objc_getClass(const char *);
typedef void *pthread_t;
extern int pthread_create(pthread_t *,const void *,void *(*)(void *),void *);
extern int pthread_join(pthread_t,void **);
static Class context_class;
static EAGLContext *context;
#ifdef EGPU_PRESENT_PROBE
static int storage(void *opaque,uint32_t w,uint32_t h) {
    (void)opaque;id drawable=egEAGLCreateDiagnosticDrawable(w,h);
    if(!drawable)return 0;
    BOOL ok=[context renderbufferStorage:GL_RENDERBUFFER fromDrawable:drawable];[drawable release];return ok;
}
static int present_frame(void *opaque) { (void)opaque;return [context presentRenderbuffer:GL_RENDERBUFFER]; }
#endif
static void *check_other_thread(void *opaque) {
    int *ok=opaque;
    *ok=![context_class currentContext] && ![context_class setCurrentContext:context] && ![context_class currentContext];
    return NULL;
}
EgGlesClient *egProbeOpen(const char *image,uint32_t width,uint32_t height) {
#ifdef EGPU_BROKER_PROBE
    (void)image;
    if(!egBrokerClientIdentity() || !egEAGLInstallBroker(egBrokerSocket(),3,width,height)) {
        dprintf(1,"*** EGPU-EAGL: FAIL broker install %s\n",egEAGLError());return NULL;
    }
#else
    if(!egEAGLInstall(image,width,height)){dprintf(1,"*** EGPU-EAGL: FAIL install %s\n",egEAGLError());return NULL;}
#endif
    context_class=objc_getClass("EAGLContext");
    EAGLContext *unsupported=[[context_class alloc]initWithAPI:3];
    if(unsupported){[unsupported release];return NULL;}
    context=[[context_class alloc]initWithAPI:2 sharegroup:(id)0];
    if(!context || [context API]!=2 || [context_class currentContext] || ![context_class setCurrentContext:context])return NULL;
    dprintf(1,"*** EGPU-EAGL: created API=2 offscreen context via Objective-C selectors\n");
    return egEAGLClient(context);
}
int egProbeExtraChecks(EgGlesClient *client) {
    if(egEAGLClient(context)!=client || [context_class currentContext]!=context || [context sharegroup])return 0;
    if([context presentRenderbuffer:0x8d41] || [context renderbufferStorage:0x8d41 fromDrawable:(id)0])return 0;
    if(![context_class setCurrentContext:(id)0] || [context_class currentContext])return 0;
    if(![context_class setCurrentContext:context] || [context_class currentContext]!=context)return 0;
    pthread_t thread;int ok=0;
    if(pthread_create(&thread,NULL,check_other_thread,&ok) || pthread_join(thread,NULL) || !ok)return 0;
    if([context_class currentContext]!=context)return 0;
    dprintf(1,"*** EGPU-EAGL: PASS API=2 current/detach/reattach; TLS ownership verified; unsupported API=3 and nil drawable return failure\n");
#ifdef EGPU_PRESENT_PROBE
    return egProbePresentation(client,NULL,storage,present_frame);
#else
    return 1;
#endif
}
void egProbeClose(EgGlesClient *client) {
    (void)client;[context_class setCurrentContext:(id)0];[context release];context=(id)0;
}
