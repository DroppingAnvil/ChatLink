/*
 * ChatLink - link a TI-Nspire CX to a PC over USB.
 * Copyright (C) 2026 Christopher Willett / AnvilDevelopment.US
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version. See the LICENSE file for the full text.
 */

/*
 * Freestanding shims for building Ndless programs against a STOCK
 * arm-none-eabi toolchain (e.g. Ubuntu's gcc-arm-none-eabi) instead of the
 * patched newlib the Ndless SDK builds from source.
 *
 * WHY NOT JUST LINK -lc
 * ---------------------
 * An earlier version linked Ubuntu's newlib to satisfy strlen/memcpy/errno.
 * That built cleanly and then crashed the calculator. Stock newlib assumes a
 * startup sequence Ndless does not perform: it reaches global reentrancy state
 * through _impure_ptr and expects a heap via _sbrk, neither of which Ndless's
 * crt0 initialises. The SDK's own newlib is configured with
 * --disable-newlib-supplied-syscalls, -DMALLOC_PROVIDED and -DABORT_PROVIDED
 * precisely because libndls and libsyscalls supply those instead.
 *
 * So we link -nostdlib with no libc at all, and define here the small set of
 * symbols that libndls and libsyscalls actually reference. Everything below is
 * self-contained and touches no global runtime state.
 */

#include <stddef.h>

/*
 * GCC may recognise a byte-copy loop and "optimise" it into a call to the very
 * function being defined, producing infinite recursion. Disabling the loop
 * pattern pass on these definitions prevents that.
 */
#pragma GCC push_options
#pragma GCC optimize("no-tree-loop-distribute-patterns")

void *memset(void *dest, int value, size_t count) {
    unsigned char *p = (unsigned char *)dest;
    while (count--) *p++ = (unsigned char)value;
    return dest;
}

void *memcpy(void *dest, const void *src, size_t count) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (count--) *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t count) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || count == 0) return dest;
    if (d < s) {
        while (count--) *d++ = *s++;
    } else {
        d += count;
        s += count;
        while (count--) *--d = *--s;
    }
    return dest;
}

int memcmp(const void *a, const void *b, size_t count) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (count--) {
        if (*x != *y) return (int)*x - (int)*y;
        ++x; ++y;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) ++p;
    return (size_t)(p - s);
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != 0) { }
    return dest;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

#pragma GCC pop_options

/*
 * libsyscalls references a plain `errno` object. Stock newlib instead defines
 * errno as a macro expanding through _impure_ptr, which is exactly the
 * uninitialised state we are avoiding, so provide a real variable.
 */
int errno;

/*
 * Ndless programs are started and torn down by the Ndless loader, not by a
 * hosted C runtime, so there is no destructor chain to register or walk.
 */
int atexit(void (*fn)(void));
int atexit(void (*fn)(void)) {
    (void)fn;
    return 0;
}

void _fini(void);
void _fini(void) {
}

__attribute__((weak)) void _init(void);
__attribute__((weak)) void _init(void) {
}
