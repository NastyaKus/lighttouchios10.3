/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real ES3 resource/presentation regression, not iOS compositor proof. */
#include "surface.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static EgsSurfacePool pool;
static EgsPresenter presenter;
int main(void) {
    EmberGL owner={0};assert(ember_gl_open(&owner,(EGLNativeWindowType)0,true,1,1));
    printf("Surface backend: %s / %s / %s\n",owner.vendor,owner.renderer,owner.version);
    assert(egs_pool_open(&pool,16u*1024u*1024u));
    assert(egs_surface_create(&pool,1,100,828,1792,EGS_RGBA));
    assert(!egs_surface_create(&pool,2,100,828,1792,EGS_RGBA));
    assert(!egs_surface_get(&pool,2,100));
    assert(egs_surface_create(&pool,1,101,828,1792,EGS_BGRA));
    assert(!egs_surface_create(&pool,1,102,828,1792,EGS_RGBA)); /* Explicit byte budget, no silent eviction. */
    uint8_t pixel[4];assert(egs_surface_read(&pool,1,100,0,0,1,1,pixel,4));
    assert(!memcmp(pixel,"\0\0\0\0",4));
    const uint8_t padded[24]={3,2,1,255,6,5,4,255,99,99,99,99,99,99,99,99,9,8,7,255,12,11,10,255};
    const uint8_t tight[16]={3,2,1,255,6,5,4,255,9,8,7,255,12,11,10,255};
    assert(egs_surface_upload(&pool,1,101,0,1,31,77,2,2,16,padded,sizeof(padded)));
    assert(pool.cpu_uploaded==16);uint8_t result[16];
    assert(egs_surface_read(&pool,1,101,31,77,2,2,result,16));assert(!memcmp(result,tight,16));
    assert(egs_surface_read(&pool,1,101,30,77,1,1,pixel,4));assert(!memcmp(pixel,"\0\0\0\0",4));
    assert(!egs_surface_upload(&pool,1,101,0,2,31,77,2,2,16,padded,sizeof(padded))); /* Stale producer. */
    assert(!egs_surface_upload(&pool,1,101,1,2,827,77,2,2,16,padded,sizeof(padded)));
    assert(!egs_surface_upload(&pool,1,101,1,2,31,77,2,2,16,padded,sizeof(padded)-1));
    assert(!egs_surface_upload(&pool,2,101,1,2,31,77,2,2,16,padded,sizeof(padded)));
    assert(pool.cpu_uploaded==16);
    assert(egs_surface_upload(&pool,1,100,0,1,31,77,2,2,16,padded,sizeof(padded)));
    assert(egs_surface_read(&pool,1,100,31,77,2,2,result,16));assert(!memcmp(result,tight,16)); /* RGBA stays RGBA. */
    assert(pool.cpu_uploaded==32);
    const EgsSurface *surface=egs_surface_get(&pool,1,100);assert(surface);
    glBindFramebuffer(GL_FRAMEBUFFER,surface->framebuffer);const GLfloat color[]={0.25f,0.5f,0.75f,1};
    glClearBufferfv(GL_COLOR,0,color);assert(egs_surface_gpu_written(&pool,1,100,1,2));
    assert(!egs_surface_gpu_written(&pool,1,100,1,3));
    uint64_t read_before=pool.cpu_read,uploaded_before=pool.cpu_uploaded;
    assert(egs_present(&presenter,&owner,&pool,1,100,1)==0);
    assert(egs_present(&presenter,&owner,&pool,1,100,2)==0);
    assert(eglGetCurrentContext()==owner.context);
    assert(presenter.stats.frames==2 && presenter.stats.surface_creates==1 && presenter.stats.sample_pixels==0);
    assert(pool.cpu_read==read_before && pool.cpu_uploaded==uploaded_before && presenter.stats.cpu_upload_bytes==0);
    /* Test-only read of the actual presented pbuffer, outside presenter/pool. */
    assert(eglMakeCurrent(owner.display,presenter.surface,presenter.surface,presenter.context));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,0);glReadPixels(414,896,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    assert(pixel[0]>=63 && pixel[0]<=64 && pixel[1]>=127 && pixel[1]<=128 && pixel[2]>=191 && pixel[2]<=192 && pixel[3]==255);
    assert(eglMakeCurrent(owner.display,owner.surface,owner.surface,owner.context));
    assert(egs_present(&presenter,&owner,&pool,1,100,2)==2); /* Replay cannot become a new displayed frame. */
    assert(egs_present(&presenter,&owner,&pool,2,100,3)==2);
    assert(egs_surface_destroy(&pool,1,100));assert(!egs_surface_get(&pool,1,100));
    assert(!egs_surface_destroy(&pool,1,100));
    assert(egs_surface_create(&pool,1,102,414,896,EGS_RGBA));
    assert(egs_present(&presenter,&owner,&pool,1,102,3)==0);assert(presenter.stats.surface_creates==2);
    egs_present_close(&presenter,&owner);egs_pool_close(&pool);assert(pool.bytes==0 && !pool.owner);
    ember_gl_close(&owner);
    puts("PASS system surfaces: full panel, RGBA/BGRA dirty rows, ownership, revision, byte budget, GPU-only presentation, resize, cleanup");return 0;
}
