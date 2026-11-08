/* SPDX-License-Identifier: GPL-3.0-or-later
 * Freestanding AArch64 regression: real stacks, AAPCS64 regs, PAC-capable CPU.
 * No Android/iOS files are used. Executed by qemu-system-aarch64 virt/TCG.
 */
#include <stdint.h>
#include "coroutine-android-aarch64.h"

static QemuAndroidJmpBuf caller_env, fiber_env;
QemuAndroidJmpBuf abi_env;
static unsigned char fiber_stack[65536] __attribute__((aligned(16)));
static volatile unsigned iterations;
extern void enter_new_stack(void *, void (*)(void)) __attribute__((noreturn));
extern int test_registers(void);

static void print(const char *message) {
    register uint64_t r0 __asm__("x0")=4;
    register const char *r1 __asm__("x1")=message;
    __asm__ volatile("hlt #0xf000" : "+r"(r0) : "r"(r1) : "memory");
}
static void stop(int code) __attribute__((noreturn));
static void stop(int code) {
    uint64_t args[2]={0x20026,(uint64_t)code};
    register uint64_t r0 __asm__("x0")=0x20;
    register void *r1 __asm__("x1")=args;
    __asm__ volatile("hlt #0xf000" : "+r"(r0) : "r"(r1) : "memory");
    for(;;){}
}
static void fail(void) { print("FAIL native coroutine context\n");stop(1); }
static void fiber(void) {
    volatile uint64_t locals[32];
    for(unsigned i=0;i<32;i++)locals[i]=0x1234000000000000ULL+i;
    for(;;) {
        for(unsigned i=0;i<32;i++)if(locals[i]!=0x1234000000000000ULL+i)fail();
        iterations++;
        if(qemu_android_fiber_setjmp(fiber_env)==0)qemu_android_fiber_longjmp(caller_env,1);
    }
}
void test_main(void) {
    if(test_registers()!=0)fail();
    print("PASS x19-x30/SP, d8-d15, x18 remains thread-local\n");
    volatile int initial=1;
    int value=qemu_android_fiber_setjmp(caller_env);
    if(initial) { initial=0;qemu_android_fiber_longjmp(caller_env,0); }
    if(value!=1)fail();
    print("PASS zero longjmp value becomes one\n");
    if(qemu_android_fiber_setjmp(caller_env)==0)enter_new_stack(fiber_stack+sizeof(fiber_stack),fiber);
    while(iterations<100000) {
        if(qemu_android_fiber_setjmp(caller_env)==0)qemu_android_fiber_longjmp(fiber_env,3);
    }
    print("PASS 100000 switches to a separate stack with local-state checks\n");
    stop(0);
}
