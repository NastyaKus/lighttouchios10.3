/* SPDX-License-Identifier: GPL-3.0-or-later
 * Own executable used to verify signed Mach-O dependency loading and GLI PAC
 * instruction roundtrip on the real iOS 14 kernel. Only the separately checked
 * versioned arm64e build can test JOP enforcement; the arm64 control cannot. */
#include <stdint.h>
#ifdef EMBER_NATIVE_ARM64E
#include <ptrauth.h>
#endif
#include "../../guest/darwin-abi.h"
extern void *dlsym(void *,const char *);
extern int egp2_selftest(void);
extern const uint32_t _mh_execute_header[];
struct Macro { void *context;void *call; };
static int callback(void *context,int number,float value) { return *(int *)context+number+(int)value; }
static void *sign_pointer(void *address) {
    __asm__ volatile("paciza %0" : "+r"(address));return address;
}
static int invoke(volatile struct Macro *table) {
    register void *context __asm__("x0")=table->context;
    register long number __asm__("x1")=5;
    register float real __asm__("s0")=3.0f;
    register void *function __asm__("x8")=table->call;
    __asm__ volatile("blraaz x8" : "+r"(context),"+r"(number),"+w"(real),"+r"(function) : :
        "x2","x3","x4","x5","x6","x7","x9","x10","x11","x12","x13","x14","x15","x16","x17","x30",
        "v1","v2","v3","v4","v5","v6","v7","v16","v17","v18","v19","v20","v21","v22","v23","v24","v25","v26","v27","v28","v29","v30","v31","cc","memory");
    return (int)(uintptr_t)context;
}
int main(void) {
    int crypto=egp2_selftest();
    if(crypto){dprintf(1,"*** EMBER-LOADER: FAIL wire_crypto=%d\n",crypto);return 1;}
    dprintf(1,"*** EMBER-LOADER: wire_crypto=verified SHA256_RFC4231_CRC\n");
    int (*loaded)(void)=dlsym((void *)(intptr_t)-2,"emberLoaderProbe");
    if(!loaded || loaded()!=14){dprintf(1,"*** EMBER-LOADER: FAIL dylib\n");return 1;}
    void *raw=(void *)callback;
#ifdef EMBER_NATIVE_ARM64E
    raw=ptrauth_strip(raw,ptrauth_key_function_pointer);
#endif
    int value=70;volatile struct Macro table={&value,sign_pointer(raw)};
    dprintf(1,"*** EMBER-LOADER: image_cpusubtype=0x%x signature_changed=%d\n",
        _mh_execute_header[2],table.call!=raw);
    int result=invoke(&table);
    if(result!=78){dprintf(1,"*** EMBER-LOADER: FAIL PAC result=%d\n",result);return 1;}
#ifdef EMBER_NATIVE_ARM64E
    if(_mh_execute_header[2]!=0x80000002u) {
        dprintf(1,"*** EMBER-LOADER: FAIL native_arm64e_header\n");return 1;
    }
    if(table.call==raw) {
        // Inferno's apple-gxf CPU defaults to pauth-noop=true under TCG.
        // An invalid address fault here would not prove PAC rejection.
        dprintf(1,"*** EMBER-LOADER: PASS dependency=loaded versioned_arm64e_loaded PAC_enforcement=unavailable\n");return 0;
    }
    // A no-JOP arm64 process can pass the positive opcode test. Flip a PAC
    // bit, keeping the address intact: this child MUST die on authentication.
    pid_t child=fork();if(child<0)return 1;
    if(!child){table.call=(void *)((uintptr_t)table.call^((uintptr_t)1<<54));invoke(&table);_exit(0);}
    int status;while(waitpid(child,&status,0)<0){if(errno!=EINTR)return 1;}
    if(!WIFSIGNALED(status) || (WTERMSIG(status)!=11 && WTERMSIG(status)!=10)) {
        dprintf(1,"*** EMBER-LOADER: FAIL corrupted_PAC_accepted wait_status=0x%x\n",status);return 1;
    }
    dprintf(1,"*** EMBER-LOADER: corrupted_PAC_rejected signal=%d\n",WTERMSIG(status));
    dprintf(1,"*** EMBER-LOADER: PASS dependency=loaded native_arm64e_abi=0 PAC_IA0_verified\n");return 0;
#else
    dprintf(1,"*** EMBER-LOADER: PASS dependency=loaded PAC_instruction_roundtrip=int_float_context\n");return 0;
#endif
}
