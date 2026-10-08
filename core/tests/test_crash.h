/*
 * A crash on a Windows runner nobody can sit at, and that hides under a
 * debugger, still has to say where it happened: install an unhandled-
 * exception filter that prints the exception, the faulting module and the
 * offset into it, and which phase of the test was running (a test calls
 * test_crash_phase as it goes; an atexit marker records "exiting").
 * Elsewhere it does nothing.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SMS_TEST_CRASH_H
#define SMS_TEST_CRASH_H

#if defined(_WIN32)
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static const char *volatile test_crash_where = "starting";

static inline void test_crash_phase(const char *where)
{
    test_crash_where = where;
}

static LONG WINAPI test_crash_filter(EXCEPTION_POINTERS *e)
{
    const void *pc = e->ExceptionRecord->ExceptionAddress;
    HMODULE mod = NULL;
    char name[MAX_PATH] = "?";

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pc, &mod))
        GetModuleFileNameA(mod, name, sizeof name);
    fprintf(stderr, "CRASH: exception %08lX at %p = %s+0x%llx, thread %lu, while %s\n",
            (unsigned long)e->ExceptionRecord->ExceptionCode, pc, name,
            (unsigned long long)((const char *)pc - (const char *)mod),
            (unsigned long)GetCurrentThreadId(), test_crash_where);
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        e->ExceptionRecord->NumberParameters >= 2)
        fprintf(stderr, "CRASH: %s address %p\n",
                e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (void *)e->ExceptionRecord->ExceptionInformation[1]);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void test_crash_atexit(void)
{
    test_crash_where = "exiting (atexit, then DLL teardown)";
}

static inline void test_crash_install(void)
{
    SetUnhandledExceptionFilter(test_crash_filter);
    atexit(test_crash_atexit);
}
#else
static inline void test_crash_phase(const char *where) { (void)where; }
static inline void test_crash_install(void) {}
#endif

#endif /* SMS_TEST_CRASH_H */
