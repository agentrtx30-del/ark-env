#pragma once

void logInit(const wchar_t* path);
void logShutdown();

void logInfo(const char* fmt, ...);
void logWarn(const char* fmt, ...);
void logError(const char* fmt, ...);