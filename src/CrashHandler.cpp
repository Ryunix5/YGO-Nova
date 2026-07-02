#include "CrashHandler.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>

namespace {

char g_version[32] = "dev";
char g_repo[96]    = "";

// Timestamped base path "crash_dumps\crash_YYYYMMDD_HHMMSS" (no extension).
// The directory sits next to the exe like the rest of the runtime data.
void basePath(char* out, size_t n) {
    CreateDirectoryA("crash_dumps", nullptr);
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
    snprintf(out, n, "crash_dumps\\crash_%s", stamp);
}

// Write a minidump via dbghelp, loaded dynamically so the exe carries no
// import-time dependency on dbghelp.dll (it ships with Windows anyway).
bool writeDump(EXCEPTION_POINTERS* ep, const char* base) {
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    if (!dbg) return false;
    using Fn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                             PMINIDUMP_EXCEPTION_INFORMATION,
                             PMINIDUMP_USER_STREAM_INFORMATION,
                             PMINIDUMP_CALLBACK_INFORMATION);
    Fn write = (Fn)(void*)GetProcAddress(dbg, "MiniDumpWriteDump");
    if (!write) return false;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s.dmp", base);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId          = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers    = FALSE;
    BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f,
                    MiniDumpNormal, ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
    return ok == TRUE;
}

void writeTextReport(EXCEPTION_POINTERS* ep, const char* base,
                     const char* what) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s.txt", base);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) return;
    fprintf(f, "YGO: Nova crash report\n");
    fprintf(f, "version : %s\n", g_version);
    if (what && what[0]) fprintf(f, "reason  : %s\n", what);
    if (ep && ep->ExceptionRecord) {
        fprintf(f, "code    : 0x%08lX\n",
                (unsigned long)ep->ExceptionRecord->ExceptionCode);
        fprintf(f, "address : %p\n", ep->ExceptionRecord->ExceptionAddress);
    }
    OSVERSIONINFOEXA vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
#pragma warning(suppress : 4996)
    GetVersionExA((OSVERSIONINFOA*)&vi);
    fprintf(f, "windows : %lu.%lu build %lu\n",
            vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    fclose(f);
}

// Offer a pre-filled GitHub issue. Body stays short (URLs have limits) and
// points the reporter at the .dmp/.txt pair on disk.
void offerIssue(const char* base, const char* what) {
    if (!g_repo[0]) return;
    char msg[512];
    snprintf(msg, sizeof(msg),
             "YGO: Nova hit a fatal error and had to close.\n\n"
             "A crash report was saved to:\n  %s.dmp / .txt\n\n"
             "Open the bug-report page? (Please attach both files.)",
             base);
    if (MessageBoxA(nullptr, msg, "YGO: Nova — crash",
                    MB_YESNO | MB_ICONERROR) != IDYES)
        return;
    char url[512];
    snprintf(url, sizeof(url),
             "https://github.com/%s/issues/new"
             "?title=Crash%%20report%%20(v%s)"
             "&body=What%%20I%%20was%%20doing%%3A%%0A%%0A"
             "%%0A%%0AReason%%3A%%20%s%%0A%%0A"
             "Please%%20attach%%20the%%20.dmp%%20and%%20.txt%%20from%%20"
             "the%%20crash_dumps%%20folder.",
             g_repo, g_version, (what && what[0]) ? what : "unknown");
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

LONG WINAPI onUnhandled(EXCEPTION_POINTERS* ep) {
    char base[MAX_PATH];
    basePath(base, sizeof(base));
    writeDump(ep, base);
    writeTextReport(ep, base, "unhandled SEH exception");
    offerIssue(base, "unhandled%20exception");
    return EXCEPTION_EXECUTE_HANDLER;   // terminate after reporting
}

void onTerminate() {
    // Unhandled C++ exception (throw with no catch). Route through the same
    // reporting path; RaiseException gives the filter real context to dump.
    char base[MAX_PATH];
    basePath(base, sizeof(base));
    writeDump(nullptr, base);
    writeTextReport(nullptr, base, "std::terminate (uncaught C++ exception)");
    offerIssue(base, "uncaught%20C%2B%2B%20exception");
    TerminateProcess(GetCurrentProcess(), 3);
}

} // namespace

namespace crash {
void install(const char* version, const char* repo) {
    if (version && version[0]) {
        strncpy_s(g_version, version, _TRUNCATE);
    }
    if (repo && repo[0]) {
        strncpy_s(g_repo, repo, _TRUNCATE);
    }
    SetUnhandledExceptionFilter(onUnhandled);
    std::set_terminate(onTerminate);
}
}

#else  // !_WIN32
namespace crash {
void install(const char*, const char*) {}
}
#endif
