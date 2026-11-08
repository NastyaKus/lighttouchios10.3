#ifndef EMBER_DARWIN_ABI_H
#define EMBER_DARWIN_ABI_H
/* SPDX-License-Identifier: GPL-3.0-or-later
 * Minimal public C/POSIX ABI declarations used by patcher-daemon.c on Darwin.
 * No SDK headers/libraries or Apple implementation are included. */
typedef unsigned long size_t;
typedef long ssize_t;
typedef long long off_t;
typedef int pid_t;
#define NULL ((void *)0)
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x0200
#define O_TRUNC 0x0400
#define SEEK_END 2
#define F_OK 0
#define WIFSIGNALED(s) (((s) & 0177) != 0 && ((s) & 0177) != 0177)
#define WTERMSIG(s) ((s) & 0177)
#define WIFEXITED(s) (((s) & 0177) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
extern void *malloc(size_t);
extern void free(void *);
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern int memcmp(const void *, const void *, size_t);
extern size_t strlen(const char *);
extern int open(const char *, int, ...);
extern int close(int), dup2(int, int), access(const char *, int), fsync(int);
extern void sync(void);
extern int unmount(const char *, int);
extern int mount(const char *, const char *, int, void *);
extern int mkdir(const char *, unsigned short), rename(const char *, const char *);
extern int dprintf(int, const char *, ...);
extern ssize_t read(int, void *, size_t);
extern ssize_t write(int, const void *, size_t);
extern int ftruncate(int, off_t);
extern off_t lseek(int, off_t, int);
extern pid_t fork(void), waitpid(pid_t, int *, int);
extern int execv(const char *, char *const []);
extern void _exit(int) __attribute__((noreturn));
extern unsigned int sleep(unsigned int);
extern int usleep(unsigned int);
extern int strncmp(const char *, const char *, size_t);
extern char *strstr(const char *, const char *);
extern int *__error(void);
#define errno (*__error())
#define EINTR 4

#endif
