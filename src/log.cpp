#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace xrperf {

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;

// Logs go to a "logs" folder next to xrperf_layer.dll rather than %LOCALAPPDATA%:
// AppData is virtualized for MSIX-packaged tools, so they would not see the game's logs.
std::wstring LogDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LogDir), &self);
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(self, path, MAX_PATH))
        return {};
    std::wstring dir(path);
    dir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}
}

void LogOpen(bool enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file || !enabled)
        return;

    std::wstring dir = LogDir();
    if (!dir.empty())
        g_file = _wfsopen((dir + L"\\xrperf.log").c_str(), L"w", _SH_DENYWR);
}

void LogLoadEvent(const char* fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::wstring dir = LogDir();
    if (dir.empty())
        return;
    FILE* f = _wfsopen((dir + L"\\xrperf_loads.log").c_str(), L"a", _SH_DENYNO);
    if (!f)
        return;
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d pid %lu: %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
            t.wSecond, GetCurrentProcessId(), msg);
    fclose(f);
}

void Log(const char* fmt, ...) {
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2100];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);

    OutputDebugStringA(line);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
}

} // namespace xrperf
