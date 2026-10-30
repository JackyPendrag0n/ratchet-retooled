// File system: guest path mapping, lv2 fs syscalls (801-834) and the cellFs imports.
#include "fs.h"
#include "coop.h"
#include "skins.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#endif
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <set>

namespace fsys = std::filesystem;

namespace {
// Mods: a file at <data>/mods/<path under PS3_GAME> (e.g. mods/USRDIR/packed/levels/metropolis/level_cached.psarc)
// is used instead of the game's own copy. tools/modding/tod_model.py writes rebuilt archives there.
std::string mod_override(const std::string& game, const std::string& host) {
    if (host.rfind(game, 0) != 0) return host;
    const std::string mod = g_config.data_dir + "/mods" + host.substr(game.size());
    std::error_code ec;
    if (fsys::is_regular_file(mod, ec)) {
        static std::set<std::string> noted;
        if (noted.insert(mod).second) LOG_NOTE("fs: mod file %s", mod.c_str());
        return mod;
    }
    return host;
}
}  // namespace

std::string vfs_host_path(const std::string& g) {
    auto starts = [&](const char* p) { return g.rfind(p, 0) == 0; };
    auto rest = [&](size_t n) { return g.substr(n); };
    std::string game = g_config.game_dir;
    // accept either the folder containing PS3_GAME or PS3_GAME itself
    if (fsys::exists(game + "/PS3_GAME")) game += "/PS3_GAME";
    // skip_intro: the logo movies (l00_*) look missing, which the game handles by moving on.
    // The title screen's background movie (l00_s00_attr_fmv, opened when the menu appears) stays.
    // The opening story movie still plays; START skips it (see PATCHES in ppu_codegen.py).
    if (g_config.skip_intro && g.find("/built/movies/l00_") != std::string::npos && g.find("_attr_") == std::string::npos)
        return game + "/.skipped-intro-movie";
    if (starts("/dev_bdvd/PS3_GAME")) {
        std::string host = mod_override(game, game + rest(18));
        skins::level_archive(host);
        if (const size_t lv = g.find("/packed/levels/"), end = g.rfind("/level_cached.psarc"); lv != std::string::npos && end != std::string::npos && end > lv + 15)
            coop::on_level_file(g.substr(lv + 15, end - lv - 15));
        return skins::movie_override(g, host);
    }
    if (starts("/app_home")) return mod_override(game, game + "/USRDIR" + rest(9));
    if (starts("/host_root")) return mod_override(game, game + "/USRDIR" + rest(10));
    if (starts("/dev_bdvd")) return fsys::path(game).parent_path().string() + rest(9);
    if (starts("/dev_hdd0/home/00000001/savedata") && !g_config.save_dir.empty()) return g_config.save_dir + rest(32);
    if (starts("/dev_hdd0")) return g_config.data_dir + "/dev_hdd0" + rest(9);
    if (starts("/dev_hdd1")) return g_config.data_dir + "/dev_hdd1" + rest(9);
    return {};
}

namespace {
u32 map_errno(int e) {
    switch (e) {
    case ENOENT: return CELL_ENOENT;
    case EEXIST: return CELL_EEXIST;
    case EISDIR: return CELL_EISDIR;
    case ENOTDIR: return CELL_ENOTDIR;
    case ENOTEMPTY: return CELL_ENOTEMPTY;
    case EBADF: return CELL_EBADF;
    case EACCES: case EPERM: return CELL_EPERM;
    default: return CELL_EIO;
    }
}

struct FileObj { int fd = -1; DIR* dir = nullptr; std::string path, host_path; };

void sync_fd(int fd) {
#ifdef _WIN32
    _commit(fd);
#else
    fsync(fd);
#endif
}
std::mutex g_lock;
std::map<u32, FileObj> g_files;
u32 g_next_fd = 3;

u32 add_file(FileObj f) {
    std::lock_guard lk(g_lock);
    const u32 id = g_next_fd++;
    g_files[id] = f;
    return id;
}
bool get_file(u32 id, FileObj& out) {
    std::lock_guard lk(g_lock);
    auto it = g_files.find(id);
    if (it == g_files.end()) return false;
    out = it->second;
    return true;
}

void write_stat(u32 ea, const struct stat& st) {
    const bool dir = S_ISDIR(st.st_mode);
    st32(mem::base, ea + 0, dir ? 040777 : 0100666);
    st32(mem::base, ea + 4, 0); st32(mem::base, ea + 8, 0);
    st64(mem::base, ea + 12, st.st_atime); st64(mem::base, ea + 20, st.st_mtime); st64(mem::base, ea + 28, st.st_ctime);
    st64(mem::base, ea + 36, dir ? 4096 : st.st_size);
    st64(mem::base, ea + 44, 4096);
}

u32 fs_open(u32 path_ea, s32 flags, u32 fd_ptr) {
    const std::string gp = read_cstr(path_ea), hp = vfs_host_path(gp);
    if (hp.empty()) { LOG_NOTE("open %s: no mapping", gp.c_str()); return CELL_ENOENT; }
    int f = 0;
    switch (flags & 3) { case 0: f = O_RDONLY; break; case 1: f = O_WRONLY; break; default: f = O_RDWR; }
    if (flags & 0100) f |= O_CREAT;
    if (flags & 0200) f |= O_EXCL;
    if (flags & 01000) f |= O_TRUNC;
    if (flags & 02000) f |= O_APPEND;
#ifdef O_BINARY
    f |= O_BINARY;  // Windows: no newline translation
#endif
    if (f & O_CREAT) fsys::create_directories(fsys::path(hp).parent_path());
    const int fd = ::open(hp.c_str(), f, 0644);
    if (fd < 0) {
        LOG_NOTE("open %s -> %s (errno %d)", gp.c_str(), hp.c_str(), errno);
        return map_errno(errno);
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { ::close(fd); return CELL_EISDIR; }
    const u32 id = add_file({fd, nullptr, gp});
    LOG_NOTE("open %s flags=0%o -> fd %u", gp.c_str(), flags, id);
    if (gp.size() > 4 && gp.compare(gp.size() - 4, 4, ".bik") == 0) movie_file_opened(gp);
    if (std::getenv("TOD_OPEN_CALLERS")) {
        if (const PPUThread* t = current_thread()) {
            std::string chain;
            u64 sp = t->ctx.r[1];
            for (int i = 0; i < 12 && sp >= t->stack_addr && sp < t->stack_addr + t->stack_size; ++i) {
                const u64 next = ld64(mem::base, u32(sp));
                if (next <= sp || next >= t->stack_addr + t->stack_size) break;
                char b[16];
                std::snprintf(b, sizeof b, " 0x%X", u32(ld64(mem::base, u32(next) + 16)));
                chain += b;
                sp = next;
            }
            LOG_NOTE("  opened from lr 0x%X, callers:%s", u32(t->ctx.lr), chain.c_str());
        }
    }
    st32(mem::base, fd_ptr, id);
    return CELL_OK;
}

// The guest buffer [buf, buf+n) must lie inside the 4 GB guest space; a bad size from the game
// fails the call instead of reading or writing past the reservation.
bool guest_range_ok(u32 buf, u64 n) { return n <= 0x100000000ull - buf; }

// read()/write() may move less than asked (a signal, a large count, Windows' 32-bit counts):
// repeat until all is moved, the file ends, or an error.
constexpr u64 kIoChunk = 1u << 30;
s64 read_all(int fd, u8* p, u64 n) {
    u64 done = 0;
    while (done < n) {
        const ssize_t r = ::read(fd, p + done, unsigned(std::min(n - done, kIoChunk)));
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return done ? s64(done) : -1;
        if (r == 0) break;
        done += u64(r);
    }
    return s64(done);
}
s64 write_all(int fd, const u8* p, u64 n) {
    u64 done = 0;
    while (done < n) {
        const ssize_t r = ::write(fd, p + done, unsigned(std::min(n - done, kIoChunk)));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return done ? s64(done) : -1;
        done += u64(r);
    }
    return s64(done);
}
// 64-bit offsets everywhere (Windows' lseek and off_t are 32-bit)
s64 seek64(int fd, s64 off, int whence) {
#ifdef _WIN32
    return _lseeki64(fd, off, whence);
#else
    return s64(::lseek(fd, off_t(off), whence));
#endif
}

u32 fs_read(u32 id, u32 buf, u64 n, u32 nread_ptr) {
    FileObj f;
    if (!get_file(id, f) || f.fd < 0) return CELL_EBADF;
    if (!guest_range_ok(buf, n)) { LOG_NOTE("fs: read of 0x%llx bytes to 0x%08x is outside guest memory", (unsigned long long)n, buf); return CELL_EFAULT; }
    static const bool trace = std::getenv("TOD_FS_TRACE") != nullptr;
    const s64 at = trace ? seek64(f.fd, 0, SEEK_CUR) : 0;
    const s64 r = read_all(f.fd, mem::base + buf, n);
    if (r < 0) return map_errno(errno);
    if (trace) LOG_NOTE("fs: read %s at 0x%llx: 0x%llx bytes to 0x%08x", f.path.c_str(), (unsigned long long)at, (unsigned long long)r, buf);
    if (nread_ptr) st64(mem::base, nread_ptr, r);
    return CELL_OK;
}
u32 fs_write(u32 id, u32 buf, u64 n, u32 nwrite_ptr) {
    FileObj f;
    if (!get_file(id, f) || f.fd < 0) return CELL_EBADF;
    if (!guest_range_ok(buf, n)) { LOG_NOTE("fs: write of 0x%llx bytes from 0x%08x is outside guest memory", (unsigned long long)n, buf); return CELL_EFAULT; }
    const s64 r = write_all(f.fd, mem::base + buf, n);
    if (r < 0) return map_errno(errno);
    if (nwrite_ptr) st64(mem::base, nwrite_ptr, r);
    return CELL_OK;
}
u32 fs_close(u32 id) {
    std::lock_guard lk(g_lock);
    auto it = g_files.find(id);
    if (it == g_files.end() || it->second.fd < 0) return CELL_EBADF;
    ::close(it->second.fd);
    g_files.erase(it);
    return CELL_OK;
}
u32 fs_lseek(u32 id, s64 off, s32 whence, u32 pos_ptr) {
    FileObj f;
    if (!get_file(id, f) || f.fd < 0) return CELL_EBADF;
    if (whence < 0 || whence > 2) return CELL_EINVAL;
    const s64 r = seek64(f.fd, off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) return CELL_EINVAL;
    if (pos_ptr) st64(mem::base, pos_ptr, r);
    return CELL_OK;
}
u32 fs_stat(u32 path_ea, u32 sb) {
    const std::string gp = read_cstr(path_ea), hp = vfs_host_path(gp);
    struct stat st;
    if (hp.empty() || ::stat(hp.c_str(), &st)) { LOG_NOTE("stat %s: not found", gp.c_str()); return CELL_ENOENT; }
    write_stat(sb, st);
    return CELL_OK;
}
u32 fs_fstat(u32 id, u32 sb) {
    FileObj f;
    if (!get_file(id, f) || f.fd < 0) return CELL_EBADF;
    struct stat st;
    if (::fstat(f.fd, &st)) return CELL_EIO;
    write_stat(sb, st);
    return CELL_OK;
}
u32 fs_opendir(u32 path_ea, u32 fd_ptr) {
    const std::string gp = read_cstr(path_ea), hp = vfs_host_path(gp);
    DIR* d = hp.empty() ? nullptr : ::opendir(hp.c_str());
    if (!d) { LOG_NOTE("opendir %s: not found", gp.c_str()); return CELL_ENOENT; }
    st32(mem::base, fd_ptr, add_file({-1, d, gp, hp}));
    return CELL_OK;
}
u32 fs_readdir(u32 id, u32 dirent, u32 nread_ptr) {
    FileObj f;
    if (!get_file(id, f) || !f.dir) return CELL_EBADF;
    struct dirent* e = ::readdir(f.dir);
    if (!e) { st64(mem::base, nread_ptr, 0); return CELL_OK; }
    const std::string name = e->d_name;
#ifdef _WIN32
    std::error_code ec;
    const bool is_dir = fsys::is_directory(fsys::path(f.host_path) / name, ec);
#else
    const bool is_dir = e->d_type == DT_DIR;
#endif
    st8(mem::base, dirent, is_dir ? 1 : 2);
    st8(mem::base, dirent + 1, u8(std::min<size_t>(name.size(), 255)));
    std::memset(mem::base + dirent + 2, 0, 256);
    std::memcpy(mem::base + dirent + 2, name.data(), std::min<size_t>(name.size(), 255));
    st64(mem::base, nread_ptr, 258);
    return CELL_OK;
}
u32 fs_closedir(u32 id) {
    std::lock_guard lk(g_lock);
    auto it = g_files.find(id);
    if (it == g_files.end() || !it->second.dir) return CELL_EBADF;
    ::closedir(it->second.dir);
    g_files.erase(it);
    return CELL_OK;
}
u32 fs_path_op(u32 path_ea, int op) {
    const std::string gp = read_cstr(path_ea), hp = vfs_host_path(gp);
    if (hp.empty()) return CELL_ENOENT;
    int r = 0;
    switch (op) {
#ifdef _WIN32
    case 0: r = ::_mkdir(hp.c_str()); break;
#else
    case 0: r = ::mkdir(hp.c_str(), 0755); break;
#endif
    case 1: r = ::rmdir(hp.c_str()); break;
    case 2: r = ::unlink(hp.c_str()); break;
    }
    LOG_NOTE("%s %s -> %d", op == 0 ? "mkdir" : op == 1 ? "rmdir" : "unlink", gp.c_str(), r ? errno : 0);
    return r ? map_errno(errno) : CELL_OK;
}

// cellFs imports (the PRX wrappers around the syscalls)
void cellFsOpen(PPUContext& c) { RET(fs_open(u32(ARG(0)), s32(ARG(1)), u32(ARG(2)))); }
void cellFsRead(PPUContext& c) { RET(fs_read(u32(ARG(0)), u32(ARG(1)), ARG(2), u32(ARG(3)))); }
void cellFsWrite(PPUContext& c) { RET(fs_write(u32(ARG(0)), u32(ARG(1)), ARG(2), u32(ARG(3)))); }
void cellFsClose(PPUContext& c) { RET(fs_close(u32(ARG(0)))); }
void cellFsLseek(PPUContext& c) { RET(fs_lseek(u32(ARG(0)), s64(ARG(1)), s32(ARG(2)), u32(ARG(3)))); }
void cellFsStat(PPUContext& c) { RET(fs_stat(u32(ARG(0)), u32(ARG(1)))); }
void cellFsFstat(PPUContext& c) { RET(fs_fstat(u32(ARG(0)), u32(ARG(1)))); }
void cellFsOpendir(PPUContext& c) { RET(fs_opendir(u32(ARG(0)), u32(ARG(1)))); }
void cellFsReaddir(PPUContext& c) { RET(fs_readdir(u32(ARG(0)), u32(ARG(1)), u32(ARG(2)))); }
void cellFsClosedir(PPUContext& c) { RET(fs_closedir(u32(ARG(0)))); }
void cellFsMkdir(PPUContext& c) { RET(fs_path_op(u32(ARG(0)), 0)); }
void cellFsRmdir(PPUContext& c) { RET(fs_path_op(u32(ARG(0)), 1)); }
void cellFsUnlink(PPUContext& c) { RET(fs_path_op(u32(ARG(0)), 2)); }
void cellFsFsync(PPUContext& c) { FileObj f; RET(get_file(u32(ARG(0)), f) ? (sync_fd(f.fd), CELL_OK) : CELL_EBADF); }
}  // namespace

void fs_syscall(PPUContext& c, u32 num) {
    switch (num) {
    case 801: RET(fs_open(u32(ARG(0)), s32(ARG(1)), u32(ARG(2)))); return;
    case 802: RET(fs_read(u32(ARG(0)), u32(ARG(1)), ARG(2), u32(ARG(3)))); return;
    case 803: RET(fs_write(u32(ARG(0)), u32(ARG(1)), ARG(2), u32(ARG(3)))); return;
    case 804: RET(fs_close(u32(ARG(0)))); return;
    case 805: RET(fs_opendir(u32(ARG(0)), u32(ARG(1)))); return;
    case 806: RET(fs_readdir(u32(ARG(0)), u32(ARG(1)), u32(ARG(2)))); return;
    case 807: RET(fs_closedir(u32(ARG(0)))); return;
    case 808: RET(fs_stat(u32(ARG(0)), u32(ARG(1)))); return;
    case 809: RET(fs_fstat(u32(ARG(0)), u32(ARG(1)))); return;
    case 811: RET(fs_path_op(u32(ARG(0)), 0)); return;
    case 813: RET(fs_path_op(u32(ARG(0)), 1)); return;
    case 814: RET(fs_path_op(u32(ARG(0)), 2)); return;
    case 818: RET(fs_lseek(u32(ARG(0)), s64(ARG(1)), s32(ARG(2)), u32(ARG(3)))); return;
    case 820: RET(CELL_OK); return;
    }
    char b[64];
    std::snprintf(b, sizeof b, "unimplemented fs syscall %u", num);
    throw std::runtime_error(b);
}

void fs_register_imports() {
    register_import(0x718bf5f8, "cellFsOpen", cellFsOpen);
    register_import(0x4d5ff8e2, "cellFsRead", cellFsRead);
    register_import(0xecdcf2ab, "cellFsWrite", cellFsWrite);
    register_import(0x2cb51f0d, "cellFsClose", cellFsClose);
    register_import(0xa397d042, "cellFsLseek", cellFsLseek);
    register_import(0x7de6dced, "cellFsStat", cellFsStat);
    register_import(0xef3efa34, "cellFsFstat", cellFsFstat);
    register_import(0x3f61245c, "cellFsOpendir", cellFsOpendir);
    register_import(0x5c74903d, "cellFsReaddir", cellFsReaddir);
    register_import(0xff42dcc3, "cellFsClosedir", cellFsClosedir);
    register_import(0xba901fe6, "cellFsMkdir", cellFsMkdir);
    register_import(0x2796fdf3, "cellFsRmdir", cellFsRmdir);
    register_import(0x7f4677a8, "cellFsUnlink", cellFsUnlink);
    register_import(0x967a162b, "cellFsFsync", cellFsFsync);
}
