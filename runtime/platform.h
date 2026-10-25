// The few OS services that POSIX (Linux, macOS) and Windows spell differently: the guest's 4 GiB
// address space reservation and page protection, a host thread id for logs, the user's home folder.
#pragma once
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

namespace plat {
enum Prot { kNone = 0, kRead = 1, kReadWrite = 3 };
void* vm_reserve(size_t size);                    // inaccessible, no memory committed; nullptr on failure
bool vm_protect(void* p, size_t size, int prot);  // commits pages as needed (Windows)
uint32_t thread_id();
std::string home_dir();                           // $HOME, or %USERPROFILE% on Windows; empty if unknown
std::string exe_dir();                            // folder holding the running executable; empty if unknown
bool own_console();                               // Windows: the console window closes with us (double-click start)
// Windows: when the log would only go to a console window (which closes with us), send stdout and
// stderr to this file instead; elsewhere, or when already redirected, nothing changes.
void log_to_file(const std::string& path);
bool show_stopped(int code);                      // Windows: a message box naming the log, if log_to_file took it
void set_file_times(const std::string& path, const timespec& atime, const timespec& mtime);
}  // namespace plat
