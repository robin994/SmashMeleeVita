#include <Runtime/platform.h>

#include <sysdolphin/baselib/class.h>
#include <sysdolphin/baselib/debug.h>
#include <sysdolphin/baselib/mtx.h>
#include <sysdolphin/baselib/objalloc.h>
#include <sysdolphin/baselib/robj.h>

#include <dolphin/mtx.h>
#include <dolphin/os.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *runtime_log;
void HSD_Panic(char *file, u32 line, char *message)
{
    fprintf(runtime_log ? runtime_log : stderr, "HSD_PANIC %s:%lu: %s\n", file ? file : "?",
            (unsigned long)line, message ? message : "?");
    if (runtime_log) fflush(runtime_log);
    abort();
}

void __wrap___assert(char *file, u32 line, char *expression)
{
    HSD_Panic(file, line, expression);
}

void OSReport(char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(runtime_log ? runtime_log : stderr, format, args);
    if (runtime_log) fflush(runtime_log);
    va_end(args);
}

void PSMTXIdentity(Mtx m)
{
    memset(m, 0, sizeof(Mtx));
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

void mv_runtime_set_log(void *file) { runtime_log = file; }
void HSD_LogInit(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (runtime_log) { fprintf(runtime_log, "HSD_LOG_INIT_PASS backend=newlib\n"); fflush(runtime_log); }
}
