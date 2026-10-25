#include "platform.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sys/utime.h>
#include <io.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#endif

namespace plat {
#ifdef _WIN32
void* vm_reserve(size_t size) { return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS); }

bool vm_protect(void* p, size_t size, int prot) {
    if (prot == kNone) {
        DWORD old;
        return VirtualProtect(p, size, PAGE_NOACCESS, &old) != 0;
    }
    // MEM_COMMIT on pages already committed only changes their protection
    return VirtualAlloc(p, size, MEM_COMMIT, prot == kRead ? PAGE_READONLY : PAGE_READWRITE) != nullptr;
}

uint32_t thread_id() { return uint32_t(GetCurrentThreadId()); }

std::string home_dir() {
    if (const char* h = std::getenv("USERPROFILE")) return h;
    if (const char* h = std::getenv("HOME")) return h;
    return {};
}

std::string exe_dir() {
    char buf[4096];
    const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    if (n == 0 || n >= sizeof buf) return {};
    return std::filesystem::path(std::string(buf, n)).parent_path().string();
}

static std::string g_log_path;

void log_to_file(const std::string& path) {
    if (GetFileType(GetStdHandle(STD_ERROR_HANDLE)) != FILE_TYPE_CHAR) return;  // already a file or pipe
    std::fprintf(stderr, "The log is written to %s\n", path.c_str());
    std::fflush(stderr);
    if (!std::freopen(path.c_str(), "w", stderr)) return;
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::fflush(stdout);
    if (_dup2(_fileno(stderr), _fileno(stdout)) == 0) std::setvbuf(stdout, nullptr, _IOLBF, 4096);
    g_log_path = path;
}

bool show_stopped(int code) {
    if (g_log_path.empty()) return false;
    std::fflush(stdout);
    std::fflush(stderr);
    char msg[1200];
    std::snprintf(msg, sizeof msg, "TOD Recomp stopped (code %d).\n\nThe log is at:\n%s", code, g_log_path.c_str());
    MessageBoxA(nullptr, msg, "TOD Recomp", MB_OK | MB_ICONERROR);
    return true;
}

bool own_console() {
    DWORD ids[2];
    return GetConsoleProcessList(ids, 2) == 1;
}

void set_file_times(const std::string& path, const timespec& atime, const timespec& mtime) {
    __utimbuf64 t{atime.tv_sec, mtime.tv_sec};
    _utime64(path.c_str(), &t);
}
#else
void* vm_reserve(size_t size) {
    void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

bool vm_protect(void* p, size_t size, int prot) {
    const int pr = prot == kNone ? PROT_NONE : prot == kRead ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect(p, size, pr) == 0;
}

uint32_t thread_id() {
#if defined(__linux__)
    return uint32_t(syscall(SYS_gettid));
#elif defined(__APPLE__)
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return uint32_t(id);
#else
    return uint32_t(uintptr_t(pthread_self()));
#endif
}

std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h ? h : "";
}

std::string exe_dir() {
    std::error_code ec;
#ifdef __APPLE__
    char buf[4096];
    uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) != 0) return {};
    const auto p = std::filesystem::canonical(buf, ec);
#else
    const auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
    return ec ? std::string() : p.parent_path().string();
}

bool own_console() { return false; }

void log_to_file(const std::string&) {}
bool show_stopped(int) { return false; }

void set_file_times(const std::string& path, const timespec& atime, const timespec& mtime) {
    const timespec ts[2] = {atime, mtime};
    ::utimensat(AT_FDCWD, path.c_str(), ts, 0);
}
#endif
}  // namespace plat
