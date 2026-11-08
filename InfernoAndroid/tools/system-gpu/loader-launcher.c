/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bounded test bundle on the disposable restore ramdisk. Never mounts APFS.
 * Default: our two files; explicit system control: caller-owned backboardd.
 * Root HFS ramdisk is remounted in memory, never an installed system disk. */
#include <stdint.h>
#include "../../guest/darwin-abi.h"
extern unsigned char *CC_SHA256(const void *,unsigned int,unsigned char *);
#ifdef EMBER_SYSTEM_EXECUTABLE
#define TARGET "/usr/libexec/backboardd"
#else
#define TARGET "/usr/lib/ember/probe"
#endif
#ifdef EMBER_STOCK_SYSTEM
extern int ember_stock_setup(int (*)(char *const []));
#endif
#ifdef EMBER_STOCK_CHROOT
extern int ember_stock_stage_chroot(void),ember_stock_enter_chroot(void);
static int enter_stock;
#endif
#ifdef EMBER_SYSTEM_EXECUTABLE
extern void *dlsym(void *,const char *);
// Public Darwin arm64 ABI, XNU 7195 sys/signal.h and proc_info.h. WNOWAIT
// preserves the zombie long enough to query its reason before waitpid reaps it.
struct ExitSigInfo {int signo,error,code,pid;unsigned uid;int status;void *address;uint64_t value;long band;unsigned long reserved[7];};
_Static_assert(sizeof(struct ExitSigInfo)==104,"Darwin arm64 siginfo ABI");
static int peek_exit(pid_t pid) {
    int (*waitid_fn)(int,unsigned,struct ExitSigInfo *,int)=(int(*)(int,unsigned,struct ExitSigInfo *,int))dlsym((void *)(long)-2,"waitid");
    if(!waitid_fn)return -1;
    struct ExitSigInfo info={0};int code=waitid_fn(1,(unsigned)pid,&info,0x25);
    if(code)return errno==EINTR?0:-1;
    return info.pid==pid;
}
static void report_exit_reason(pid_t pid) {
    int (*info)(int,int,uint64_t,void *,int)=(int(*)(int,int,uint64_t,void *,int))dlsym((void *)(long)-2,"proc_pidinfo");
    unsigned char bytes[24]={0};int size=info?info(pid,25,0,bytes,sizeof(bytes)):-1;
    if(size==sizeof(bytes)) {
        uint32_t space;uint64_t code,flags;
        memcpy(&space,bytes,4);memcpy(&code,bytes+4,8);memcpy(&flags,bytes+12,8);
        dprintf(1,"*** EMBER-BB-LOADER: exit_reason namespace=%u code=%llu flags=0x%llx\n",space,(unsigned long long)code,(unsigned long long)flags);
    } else dprintf(1,"*** EMBER-BB-LOADER: exit_reason_unavailable size=%d errno=%d\n",size,errno);
}
#endif
static int run(char *const args[]) {
    pid_t pid=fork();if(pid<0)return -1;
    if(!pid){
#ifdef EMBER_STOCK_CHROOT
        if(enter_stock && !ember_stock_enter_chroot())_exit(126);
#endif
        execv(args[0],args);_exit(127);
    }
    int status;
#ifdef EMBER_SYSTEM_EXECUTABLE
    if(!strncmp(args[0],TARGET,128)) {
        // Real backboardd needs the installed launchd environment. Bound this
        // dependency-loading experiment; a stopped test is not a healthy UI.
        int exited=0,stopped=0;
        for(int i=0;i<200;i++) {
            int done=peek_exit(pid);
            if(done==1){exited=1;break;}if(done<0)break;
            usleep(100000);
        }
        if(!exited) {
            extern int kill(int,int);kill(pid,15);stopped=1;
            for(int i=0;i<20;i++) {
                if(peek_exit(pid)==1){exited=1;break;}usleep(100000);
            }
            if(!exited)kill(pid,9);
        }
        report_exit_reason(pid);
        while(waitpid(pid,&status,0)<0){if(errno!=EINTR)return -1;}
        if(stopped)dprintf(1,"*** EMBER-BB-LOADER: controlled_stop wait_status=0x%x\n",status);
    } else
#endif
    while(waitpid(pid,&status,0)<0){if(errno!=EINTR)return -1;}
    if(WIFSIGNALED(status))dprintf(1,"*** EMBER-LOADER: child_signal=%d wait_status=0x%x program=%s\n",WTERMSIG(status),status,args[0]);
    return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}
static uint32_t word(const unsigned char *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int exact(int fd,unsigned char *p,uint32_t n) {
    uint32_t at=0;while(at<n){ssize_t k=read(fd,p+at,n-at);if(k<0&&errno==EINTR)continue;if(k<=0)return 0;at+=(uint32_t)k;}return 1;
}
static int put(int input,const char *name,uint32_t size) {
    unsigned char *bytes=malloc(size),expected[32],actual[32];if(!bytes)return 0;
    if(!exact(input,bytes,size)){free(bytes);return 0;}
    CC_SHA256(bytes,size,expected);
    int fd=open(name,O_CREAT|O_TRUNC|O_WRONLY,0755);if(fd<0){free(bytes);return 0;}
    uint32_t at=0;int ok=1;
    while(at<size){ssize_t n=write(fd,bytes+at,size-at);if(n<0&&errno==EINTR)continue;if(n<=0){ok=0;break;}at+=(uint32_t)n;}
    if(fsync(fd))ok=0;if(close(fd))ok=0;
    fd=open(name,O_RDONLY);
    if(fd<0)ok=0;else {if(!exact(fd,bytes,size))ok=0;close(fd);}
    CC_SHA256(bytes,size,actual);if(memcmp(expected,actual,32))ok=0;
    if(ok) {
        char text[65];const char hex[]="0123456789abcdef";
        for(unsigned i=0;i<32;i++){text[i*2]=hex[actual[i]>>4];text[i*2+1]=hex[actual[i]&15];}text[64]=0;
        dprintf(1,"*** EMBER-LOADER: file_readback=%s bytes=%u sha256=%s\n",name,size,text);
    }
    free(bytes);return ok;
}
int main(void) {
    char *mount_args[]={"/sbin/mount","-u","-w","/",NULL};
    int result=run(mount_args);if(result){dprintf(1,"*** EMBER-LOADER: FAIL remount=%d\n",result);return 1;}
    mkdir("/usr/lib/ember",0755);
    int input=open("/usr/standalone/firmware/nfrestore/firmware/jcop-prod/JCOP-11.04-012.2-P.bin",O_RDONLY);
    unsigned char header[16];
    if(input<0 || !exact(input,header,sizeof(header)) || memcmp(header,"EMBLOAD1",8) ||
       !word(header+8) || word(header+8)>2*1024*1024 || !word(header+12) || word(header+12)>2*1024*1024)goto fail;
    if(!put(input,TARGET,word(header+8)) || !put(input,"/usr/lib/ember/libEmberSystemGLES.dylib",word(header+12)))goto fail;
    close(input);
#ifdef EMBER_STOCK_SYSTEM
    if(!ember_stock_setup(run)){dprintf(1,"*** EMBER-LOADER: FAIL stock_environment\n");return 1;}
#endif
#ifdef EMBER_STOCK_CHROOT
    if(!ember_stock_stage_chroot())return 1;
    enter_stock=1;
#endif
    char *args[]={TARGET,NULL};result=run(args);
#ifdef EMBER_STOCK_SYSTEM
    int unmounted=unmount("/mnt9",0);
    dprintf(1,"*** EMBER-STOCK: unmount=%d errno=%d\n",unmounted,unmounted?errno:0);
#endif
    if(!result){dprintf(1,"*** EMBER-LOADER: child_exit=0\n");return 0;}
    dprintf(1,"*** EMBER-LOADER: FAIL child_exit=%d\n",result);return 1;
fail:
    dprintf(1,"*** EMBER-LOADER: FAIL bundle errno=%d\n",errno);if(input>=0)close(input);return 1;
}
