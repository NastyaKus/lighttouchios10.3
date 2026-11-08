/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "file-host.h"
#include "session.h"
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

static uint64_t now_ms(void) { struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000; }
static void error(EgpuHostResult *r,const char *stage) { snprintf(r->error,sizeof(r->error),"%s: %s",stage,strerror(errno)); }
int egpu_file_host(const char *path,uint32_t timeout_ms,int (*cancelled)(void *),void *opaque,EgpuHostResult *result) {
    return egpu_file_host_ex(path,timeout_ms,cancelled,opaque,result,NULL);
}
int egpu_file_host_ex(const char *path,uint32_t timeout_ms,int (*cancelled)(void *),void *opaque,EgpuHostResult *result,const EgPresentOptions *options) {
    memset(result,0,sizeof(*result));
    if(!path || !timeout_ms || timeout_ms>300000){snprintf(result->error,sizeof(result->error),"Invalid path/timeout");return 1;}
    int fd=open(path,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(fd<0){error(result,"create diagnostic image (must not exist)");return 1;}
    uint8_t identity[EGPU_PAGE]={0},request[EGPU_PAGE],reply[EGPU_PAGE];
    memcpy(identity,EGPU_DISK_MAGIC,16);egpu_put32(identity+16,EGPU_VERSION);egpu_put32(identity+20,EGPU_DISK_BYTES);
    int random=open("/dev/urandom",O_RDONLY);
    if(random<0){error(result,"random");close(fd);return 1;}
    int random_ok=read(random,identity+24,16)==16;close(random);
    if(!random_ok){snprintf(result->error,sizeof(result->error),"Short random read");close(fd);return 1;}
    if(ftruncate(fd,EGPU_DISK_BYTES) || pwrite(fd,identity,EGPU_PAGE,0)!=EGPU_PAGE || fsync(fd)) {
        error(result,"initialize image");close(fd);return 1;
    }
    EgpuExecutor executor={0};EgpuSession session={0};
    if(egpu_executor_open(&executor)) {
        snprintf(result->error,sizeof(result->error),"EGL/GL initialization failed: %.190s",executor.context.error);
        egpu_executor_close(&executor);close(fd);return 1;
    }
    snprintf(result->vendor,sizeof(result->vendor),"%s",executor.context.vendor);
    if(options)executor.presenter.options=*options;
    snprintf(result->renderer,sizeof(result->renderer),"%s",executor.context.renderer);
    snprintf(result->version,sizeof(result->version),"%s",executor.context.version);
    printf("READY %s / %s\n",result->vendor,result->renderer);fflush(stdout);
    uint64_t deadline=now_ms()+timeout_ms;
    while(now_ms()<deadline) {
        if(cancelled && cancelled(opaque)){snprintf(result->error,sizeof(result->error),"Cancelled");break;}
        uint8_t current[64];
        if(pread(fd,current,64,0)!=64 || memcmp(current,identity,64)) {
            snprintf(result->error,sizeof(result->error),"Diagnostic channel identity changed");break;
        }
        if(pread(fd,request,EGPU_PAGE,EGPU_REQUEST_OFFSET)==EGPU_PAGE &&
            (!session.last_seq || memcmp(request,session.request,EGPU_PAGE))) {
            EgpuFrame frame;
            if(!egpu_decode(request,EGPU_PAGE,&frame) && !egpu_session_process(&executor,&session,request,reply)) {
                if(pwrite(fd,reply,EGPU_PAGE,EGPU_REPLY_OFFSET)!=EGPU_PAGE || fsync(fd)){error(result,"write reply");break;}
                EgpuFrame response;egpu_decode(reply,EGPU_PAGE,&response);
                printf("COMMAND seq=%u opcode=%u status=%u\n",frame.seq,frame.opcode,egpu_u32(response.payload));fflush(stdout);
                if(frame.opcode==EGPU_GLES_PRESENT && executor.presenter.error[0]) {
                    snprintf(result->error,sizeof(result->error),"Presentation: %.190s",executor.presenter.error);
                    printf("PRESENT_ERROR %s\n",result->error);fflush(stdout);
                }
                if(frame.opcode==EGPU_DESTROY && !egpu_u32(response.payload)){result->success=!result->error[0];break;}
            }
        }
        struct timespec delay={0,10000000};nanosleep(&delay,NULL);
    }
    if(!result->success && !result->error[0])snprintf(result->error,sizeof(result->error),"Guest command timeout");
    result->commands=executor.commands;result->draws=executor.draws;result->pixels_read=executor.pixels_read;
    result->api_calls=executor.gles.calls;result->api_batches=executor.gles.batches;
    result->api_draws=executor.gles.draws;result->shader_compiles=executor.gles.shader_compiles;
    result->presentation=executor.presenter.stats;
    printf("PRESENT_RESULT frames=%llu window_frames=%llu sample_pixels=%llu cpu_upload_bytes=%llu\n",
        (unsigned long long)result->presentation.frames,(unsigned long long)result->presentation.window_frames,
        (unsigned long long)result->presentation.sample_pixels,(unsigned long long)result->presentation.cpu_upload_bytes);
    printf("API_RESULT calls=%llu batches=%llu draws=%llu shader_compiles=%llu\n",
        (unsigned long long)result->api_calls,(unsigned long long)result->api_batches,
        (unsigned long long)result->api_draws,(unsigned long long)result->shader_compiles);
    printf("RESULT success=%d commands=%llu draws=%llu readback_pixels=%llu\n",result->success,
        (unsigned long long)result->commands,(unsigned long long)result->draws,(unsigned long long)result->pixels_read);
    egpu_executor_close(&executor);close(fd);return result->success?0:1;
}
