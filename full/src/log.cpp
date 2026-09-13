#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <windows.h>

static FILE* g_log = nullptr;

static void writeLine(const char* level, const char* fmt, va_list args)
{
    char body[2048] = {};
    vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);

    SYSTEMTIME st = {};
    GetLocalTime(&st);

    char line[2300] = {};
    snprintf(
        line,
        sizeof(line),
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] %s\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        level,
        body
    );

    fputs(line, stdout);

    if (g_log)
    {
        fputs(line, g_log);
        fflush(g_log);
    }
}

void logInit(const wchar_t* path)
{
    if (g_log)
        return;

    if (path && path[0])
        _wfopen_s(&g_log, path, L"a");
}

void logShutdown()
{
    if (g_log)
    {
        fclose(g_log);
        g_log = nullptr;
    }
}

void logInfo(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("info", fmt, args);
    va_end(args);
}

void logWarn(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("warn", fmt, args);
    va_end(args);
}

void logError(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("error", fmt, args);
    va_end(args);
}