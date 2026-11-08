/* SPDX-License-Identifier: GPL-3.0-or-later
 * Explicit command producer for a diagnostic scratch namespace. This is not an
 * EAGL/Metal driver. It never mounts or patches an installed iOS volume. */
#include "protocol.h"
#ifdef EGPU_GUEST_FREESTANDING
#undef NULL
#include "../guest/darwin-abi.h"
extern void *dlopen(const char *,int);
extern void *dlsym(void *,const char *);
extern int dlclose(void *);
extern const char *dlerror(void);
#else
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#endif

static uint8_t *request,*reply,*scratch;
static uint8_t identity[64];
static uint32_t sequence;
static int disk=-1;
static void report_iosurface(void) {
#ifdef EGPU_GUEST_FREESTANDING
    void *cf=dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",1);
    if(!cf)cf=dlopen("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation",1);
    void *io=dlopen("/System/Library/Frameworks/IOSurface.framework/IOSurface",1);
    if(!io)io=dlopen("/System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface",1);
    if(!cf || !io){dprintf(1,"*** EGPU: IOSurface runtime unavailable\n");goto cleanup;}
    void *(*number)(void *,long,const void *)=dlsym(cf,"CFNumberCreate");
    void *(*dictionary)(void *,const void **,const void **,long,const void *,const void *)=dlsym(cf,"CFDictionaryCreate");
    void (*release)(const void *)=dlsym(cf,"CFRelease");
    const void *key_callbacks=dlsym(cf,"kCFTypeDictionaryKeyCallBacks");
    const void *value_callbacks=dlsym(cf,"kCFTypeDictionaryValueCallBacks");
    void *(*create)(void *)=dlsym(io,"IOSurfaceCreate");
    int (*lock)(void *,uint32_t,uint32_t *)=dlsym(io,"IOSurfaceLock");
    int (*unlock)(void *,uint32_t,uint32_t *)=dlsym(io,"IOSurfaceUnlock");
    void *(*base)(void *)=dlsym(io,"IOSurfaceGetBaseAddress");
    size_t (*width)(void *)=dlsym(io,"IOSurfaceGetWidth");
    size_t (*height)(void *)=dlsym(io,"IOSurfaceGetHeight");
    size_t (*stride)(void *)=dlsym(io,"IOSurfaceGetBytesPerRow");
    uint32_t (*identifier)(void *)=dlsym(io,"IOSurfaceGetID");
    if(!number || !dictionary || !release || !create || !lock || !unlock || !base ||
       !width || !height || !stride || !identifier || !key_callbacks || !value_callbacks) {
        dprintf(1,"*** EGPU: IOSurface public ABI symbols unavailable\n");goto cleanup;
    }
    const char *names[]={"kIOSurfaceWidth","kIOSurfaceHeight","kIOSurfaceBytesPerElement",
        "kIOSurfaceBytesPerRow","kIOSurfaceAllocSize","kIOSurfacePixelFormat"};
    const int32_t integers[]={64,64,4,256,16384,0x42475241};
    const void *keys[6]={0},*values[6]={0};int ready=1;
    for(int i=0;i<6;i++) {
        const void **key=dlsym(io,names[i]);keys[i]=key?*key:NULL;
        values[i]=number(NULL,3,&integers[i]);if(!keys[i] || !values[i])ready=0;
    }
    void *attributes=ready?dictionary(NULL,keys,values,6,key_callbacks,value_callbacks):NULL;
    void *surface=attributes?create(attributes):NULL;
    if(surface) {
        uint32_t seed=0;int locked=lock(surface,0,&seed),verified=0,unlocked=-1,written=0;
        if(!locked) {
            uint8_t *pixels=base(surface);
            if(pixels && width(surface)==64 && height(surface)==64 && stride(surface)>=256) {
                const uint8_t expected[4]={17,34,51,255};memcpy(pixels,expected,4);
                written=1;
            }
            unlocked=unlock(surface,0,&seed);
            if(written && !unlocked && !(locked=lock(surface,0,&seed))) {
                const volatile uint8_t *again=base(surface);
                verified=again && again[0]==17 && again[1]==34 && again[2]==51 && again[3]==255;
                unlocked=unlock(surface,0,&seed);
            }
        }
        dprintf(1,"*** EGPU: IOSurface id=%u width=%lu height=%lu stride=%lu lock=%d unlock=%d cpu_mapping_verified=%d\n",
            identifier(surface),(unsigned long)width(surface),(unsigned long)height(surface),
            (unsigned long)stride(surface),locked,unlocked,verified);
        release(surface);
    } else dprintf(1,"*** EGPU: IOSurfaceCreate returned NULL\n");
    if(attributes)release(attributes);
    for(int i=0;i<6;i++)if(values[i])release(values[i]);
cleanup:
    if(io)dlclose(io);
    if(cf)dlclose(cf);
#endif
}
static void report_guest_graphics(void) {
#ifdef EGPU_GUEST_FREESTANDING
    const char *paths[]={"/System/Library/Frameworks/IOKit.framework/IOKit",
        "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit",
        "/System/Library/PrivateFrameworks/IOKit.framework/IOKit","/usr/lib/libIOKit.dylib"};
    void *kit=NULL;
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]) && !kit;i++)kit=dlopen(paths[i],1);
    if(!kit){const char *error=dlerror();dprintf(1,"*** EGPU: IOKit unavailable: %s\n",error?error:"unknown loader error");return;}
    void *(*matching)(const char *)=dlsym(kit,"IOServiceMatching");
    int (*services)(uint32_t,void *,uint32_t *)=dlsym(kit,"IOServiceGetMatchingServices");
    uint32_t (*next)(uint32_t)=dlsym(kit,"IOIteratorNext");
    int (*release)(uint32_t)=dlsym(kit,"IOObjectRelease");
    if(matching && services && next && release) {
        const char *classes[]={"IOAccelerator","IOGPU","AGXAccelerator","AppleParavirtGPU","IOSurfaceRoot"};
        for(unsigned i=0;i<sizeof(classes)/sizeof(classes[0]);i++) {
            uint32_t iterator=0,count=0;void *query=matching(classes[i]);
            int status=query?services(0,query,&iterator):-1;
            if(!status){uint32_t object;while((object=next(iterator))){count++;release(object);}release(iterator);}
            dprintf(1,"*** EGPU: registry class=%s count=%u status=%d\n",classes[i],count,status);
        }
    }
    dlclose(kit);
#endif
}
static int read_page(int fd,uint32_t at,uint8_t *p) {
    return lseek(fd,at,0)==at && read(fd,p,EGPU_PAGE)==EGPU_PAGE;
}
static int marked(int fd) {
    return read_page(fd,0,scratch) && !memcmp(scratch,EGPU_DISK_MAGIC,16) &&
        egpu_u32(scratch+16)==EGPU_VERSION && egpu_u32(scratch+20)==EGPU_DISK_BYTES;
}
static int exchange(uint16_t opcode,const uint8_t *payload,uint32_t size,EgpuFrame *response) {
    if(!marked(disk) || memcmp(identity,scratch,64))return 0;
    if(egpu_encode(request,opcode,++sequence,payload,size))return 0;
    if(lseek(disk,EGPU_REQUEST_OFFSET,0)!=EGPU_REQUEST_OFFSET || write(disk,request,EGPU_PAGE)!=EGPU_PAGE)return 0;
    if(fsync(disk))return 0;
    for(int attempt=0;attempt<3000;attempt++) {
        if(read_page(disk,EGPU_REPLY_OFFSET,reply) && !egpu_decode(reply,EGPU_PAGE,response) &&
            response->seq==sequence && response->opcode==(opcode|EGPU_RESPONSE) && response->size>=4) {
            int status=egpu_u32(response->payload);
            if(status)dprintf(1,"*** EGPU: host error opcode=%u status=%u\n",opcode,status);
            return status==EGPU_OK;
        }
        usleep(10000);
    }
    dprintf(1,"*** EGPU: reply timeout opcode=%u seq=%u\n",opcode,sequence);return 0;
}
static int try_device(const char *name) {
    int fd=open(name,O_RDONLY);
    if(fd<0)return -1;
    int found=marked(fd);close(fd);if(!found)return -1;
    fd=open(name,O_RDWR);
    if(fd<0)return -1;
    if(!marked(fd)){close(fd);return -1;}
    memcpy(identity,scratch,64);dprintf(1,"*** EGPU: diagnostic namespace %s\n",name);return fd;
}
int main(int argc,char **argv) {
    dprintf(1,"*** EGPU: probe started (explicit commands, not an iOS graphics driver)\n");
    report_guest_graphics();
    if(argc==2 && !strncmp(argv[1],"--registry-only",16))return 0;
    report_iosurface();
    void *allocation=malloc(EGPU_PAGE*4+4095);
    if(!allocation)return 1;
    request=(uint8_t *)(((uintptr_t)allocation+4095)&~(uintptr_t)4095);
    reply=request+EGPU_PAGE;scratch=reply+EGPU_PAGE;
    if(argc==3 && !strncmp(argv[1],"--image",8))disk=try_device(argv[2]);
    else for(int attempt=0;attempt<60 && disk<0;attempt++) {
        for(int i=0;i<32 && disk<0;i++) {
            char path[20]="/dev/rdisk";int end=10;
            if(i>=10)path[end++]=(char)('0'+i/10);
            path[end++]=(char)('0'+i%10);path[end]=0;
            disk=try_device(path);
        }
        if(disk<0)usleep(1000000);
    }
    if(disk<0){dprintf(1,"*** EGPU: FAIL no marked diagnostic raw namespace (absent or denied)\n");return 1;}
    uint8_t payload[64]={0};EgpuFrame response;
#define SEND(op,n) do { if(!exchange(op,payload,n,&response))goto failed; } while(0)
    SEND(EGPU_HELLO,0);
    if(response.size<17 || response.payload[response.size-1]!=0 || egpu_u32(response.payload+4)!=EGPU_VERSION)goto failed;
    dprintf(1,"*** EGPU: backend %.*s\n",(int)response.size-16,response.payload+16);
    egpu_put32(payload,64);egpu_put32(payload+4,64);SEND(EGPU_CREATE,8);
    if(response.size!=8)goto failed;
    uint32_t handle=egpu_u32(response.payload+4);
    egpu_put32(payload,handle);payload[4]=0;payload[5]=0;payload[6]=255;payload[7]=255;
    SEND(EGPU_CLEAR,8);
    const float vertices[6]={-.75f,-.75f,.75f,-.75f,0,.75f};
    for(int i=0;i<6;i++){uint32_t value;memcpy(&value,&vertices[i],4);egpu_put32(payload+4+i*4,value);}
    payload[28]=255;payload[29]=0;payload[30]=0;payload[31]=255;
    SEND(EGPU_TRIANGLE,32);
    SEND(EGPU_FENCE,4);
    egpu_put32(payload+4,2);egpu_put32(payload+8,32);egpu_put32(payload+12,32);
    egpu_put32(payload+16,0);egpu_put32(payload+20,0);SEND(EGPU_READ_POINTS,24);
    const uint8_t expected[8]={255,0,0,255,0,0,255,255};
    if(response.size!=16 || egpu_u32(response.payload+4)!=2 || memcmp(response.payload+8,expected,8)) {
        dprintf(1,"*** EGPU: FAIL GPU readback differs\n");goto failed;
    }
    SEND(EGPU_DESTROY,4);
    dprintf(1,"*** EGPU: PASS guest commands -> host GL -> verified red triangle / blue corner\n");
    close(disk);free(allocation);return 0;
failed:
    dprintf(1,"*** EGPU: FAIL command exchange seq=%u\n",sequence);close(disk);free(allocation);return 1;
}
