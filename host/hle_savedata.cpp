// cellSaveData (exported by cellSysutil): List/ListAuto load and save, Delete2.
// Operation flow follows RPCS3's savedata_op(); there is no on-screen UI, so where the firmware would show
// a selection or confirmation dialog the choice the game's own callback made (focus / fixed dir) is taken,
// except that a list save never overwrites a save other than the one being played (see below).
// Saves live in g_config.save_dir (vfs_host_path of /dev_hdd0/home/00000001/savedata).
#include "guest.h"
#include "overlay_notify.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "platform.h"

#include <fcntl.h>
#include <sys/stat.h>

namespace {
namespace fs = std::filesystem;

enum : u32 {
    CELL_SAVEDATA_ERROR_CBRESULT = 0x8002b401, CELL_SAVEDATA_ERROR_ACCESS_ERROR = 0x8002b402,
    CELL_SAVEDATA_ERROR_INTERNAL = 0x8002b403, CELL_SAVEDATA_ERROR_PARAM = 0x8002b404,
    CELL_SAVEDATA_ERROR_BROKEN = 0x8002b406, CELL_SAVEDATA_ERROR_FAILURE = 0x8002b407,
    CELL_SAVEDATA_ERROR_BUSY = 0x8002b408,
    CELL_SAVEDATA_RET_CANCEL = 1,  // CELL_CANCEL: a "not an error" status, returned as-is
};
enum : s32 {
    CBRESULT_OK_LAST_NOCONFIRM = 2, CBRESULT_OK_LAST = 1, CBRESULT_OK_NEXT = 0, CBRESULT_ERR_INVALID = -5,
};
enum : u32 {
    OP_LIST_AUTO_SAVE = 2, OP_LIST_AUTO_LOAD = 3, OP_LIST_SAVE = 4, OP_LIST_LOAD = 5,
    SORTTYPE_MODIFIEDTIME = 0, SORTTYPE_SUBTITLE = 1, SORTORDER_DESCENT = 0, SORTORDER_ASCENT = 1,
    FOCUSPOS_DIRNAME = 0, FOCUSPOS_LISTHEAD = 1, FOCUSPOS_LISTTAIL = 2, FOCUSPOS_LATEST = 3,
    FOCUSPOS_OLDEST = 4, FOCUSPOS_NEWDATA = 5,
    FILEOP_READ = 0, FILEOP_WRITE = 1, FILEOP_DELETE = 2, FILEOP_WRITE_NOTRUNC = 3,
    FILETYPE_SECUREFILE = 0, FILETYPE_NORMALFILE = 1, FILETYPE_ICON0 = 2, FILETYPE_ICON1 = 3,
    FILETYPE_PIC1 = 4, FILETYPE_SND0 = 5,
    RECREATE_NO = 0, RECREATE_NO_NOBROKEN = 1, RECREATE_YES = 2, RECREATE_YES_RESET_OWNER = 3,
    RECREATE_MASK = 0xfffeffff, ICONPOS_HEAD = 0, ICONPOS_TAIL = 1,
    DIRNAME_SIZE = 32, FILENAME_SIZE = 13, PREFIX_SIZE = 256, LISTITEM_MAX = 2048, DIRLIST_MAX = 2048,
};
// The game predates SDK 3.70: name strings may fill their whole buffer (sysutil_check_name_string) and
// listParam may use all 8 bytes.
constexpr bool kSdk370 = false;
constexpr s32 kHddFreeKB = 40 * 1024 * 1024 - 256;

// ---- guest struct layouts (big-endian, 32-bit pointers; offsets from cellSaveData.h) -----
namespace SetList { constexpr u32 sortType = 0, sortOrder = 4, dirNamePrefix = 8, reserved = 12; }   // 16 bytes
namespace SetBuf { constexpr u32 dirListMax = 0, fileListMax = 4, reserved = 8, bufSize = 32, buf = 36; }  // 40 bytes
namespace DirList { constexpr u32 dirName = 0, listParam = 32, size = 48; }                              // 48 bytes
namespace ListNewData { constexpr u32 iconPosition = 0, dirName = 4; }                                   // 16 bytes
namespace CBResult { constexpr u32 result = 0, userdata = 16; }                                          // 20 bytes
namespace ListGet { constexpr u32 dirNum = 0, dirListNum = 4, dirList = 8; }                             // 76 bytes
namespace ListSet { constexpr u32 focusPosition = 0, focusDirName = 4, fixedListNum = 8, fixedList = 12, newData = 16; }  // 24 bytes
namespace FixedSet { constexpr u32 dirName = 0, newIcon = 4, option = 8; }                               // 12 bytes
namespace SysParam {                                                                                    // 1552 bytes
constexpr u32 title = 0, subTitle = 128, detail = 256, attribute = 1280, parental_level = 1284, listParam = 1288,
              reserved = 1296, reservedSize = 256;
}
namespace DirStat { constexpr u32 atime = 0, mtime = 8, ctime = 16, dirName = 24; }                      // 56 bytes
namespace FileStat {                                                                                    // 56 bytes
constexpr u32 fileType = 0, fsize = 8, atime = 16, mtime = 24, ctime = 32, fileName = 40, size = 56;
}
namespace StatGet {                                                                                     // 1704 bytes
constexpr u32 hddFreeSizeKB = 0, isNewData = 4, dir = 8, getParam = 64, bind = 1616, sizeKB = 1620, sysSizeKB = 1624,
              fileNum = 1628, fileListNum = 1632, fileList = 1636, size = 1704;
}
namespace StatSet { constexpr u32 setParam = 0, reCreateMode = 4, indicator = 8; }                       // 12 bytes
namespace FileGet { constexpr u32 excSize = 0, reserved = 4; }                                           // 68 bytes
namespace FileSet {                                                                                     // 48 bytes
constexpr u32 fileOperation = 0, fileType = 8, secureFileId = 12, fileName = 28, fileOffset = 32, fileSize = 36,
              fileBufSize = 40, fileBuf = 44, size = 48;
}
// Callback scratch area (one allocation, 16-byte aligned members like RPCS3's savedata_context).
constexpr u32 kResult = 0x000, kListGet = 0x020, kListSet = 0x070, kFixedSet = 0x090, kStatGet = 0x0a0,
              kStatSet = 0x750, kFileGet = 0x760, kFileSet = 0x7b0, kAreaSize = 0x800;
static_assert(kStatGet + StatGet::size <= kStatSet && kFileSet + FileSet::size <= kAreaSize);

std::mutex g_savedata_lock;

// ---- PARAM.SFO reader/writer (string and integer entries) --------------------------------
using Sfo = std::map<std::string, std::pair<bool, std::string>>;  // key -> (is_int, value or 4 bytes LE)
bool sfo_load(const std::string& path, Sfo& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<u8> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 20 || std::memcmp(d.data(), "\0PSF", 4)) return false;
    auto le32 = [&](size_t o) { return u32(d[o]) | u32(d[o + 1]) << 8 | u32(d[o + 2]) << 16 | u32(d[o + 3]) << 24; };
    auto le16 = [&](size_t o) { return u32(d[o]) | u32(d[o + 1]) << 8; };
    const u32 keys = le32(8), data = le32(12), n = le32(16);
    if (20 + size_t(n) * 16 > d.size()) return false;
    for (u32 i = 0; i < n; ++i) {
        const size_t e = 20 + 16 * i;
        const u32 fmt = le16(e + 2), len = le32(e + 4), off = le32(e + 12);
        const size_t ko = size_t(keys) + le16(e), vo = size_t(data) + off;
        if (ko >= d.size() || vo + (fmt == 0x0404 ? 4 : len) > d.size()) return false;
        const std::string key(reinterpret_cast<const char*>(&d[ko]), strnlen(reinterpret_cast<const char*>(&d[ko]), d.size() - ko));
        if (fmt == 0x0404) out[key] = {true, std::string(reinterpret_cast<const char*>(&d[vo]), 4)};
        else {
            std::string v(reinterpret_cast<const char*>(&d[vo]), len ? len - 1 : 0);
            v.resize(strnlen(v.c_str(), v.size()));
            out[key] = {false, v};
        }
    }
    return true;
}
bool sfo_save(const std::string& path, const Sfo& s) {
    std::string keys, data;
    std::vector<u8> idx;
    auto p16 = [&](u32 v) { idx.push_back(u8(v)); idx.push_back(u8(v >> 8)); };
    auto p32 = [&](u32 v) { p16(v & 0xffff); p16(v >> 16); };
    for (auto& [k, v] : s) {
        const u32 len = v.first ? 4 : u32(v.second.size() + 1);
        const u32 max = v.first ? 4 : (len + 3) & ~3u;
        p16(u32(keys.size())); p16(v.first ? 0x0404 : 0x0204); p32(len); p32(max); p32(u32(data.size()));
        keys += k; keys += '\0';
        std::string val = v.second;
        val.resize(max, '\0');
        data += val;
    }
    while (keys.size() % 4) keys += '\0';
    const u32 key_off = 20 + u32(idx.size()), data_off = key_off + u32(keys.size());
    std::vector<u8> h = {0, 'P', 'S', 'F', 1, 1, 0, 0};
    auto h32 = [&](u32 v) { for (int i = 0; i < 4; ++i) h.push_back(u8(v >> (8 * i))); };
    h32(key_off); h32(data_off); h32(u32(s.size()));
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(h.data()), h.size());
    f.write(reinterpret_cast<const char*>(idx.data()), idx.size());
    f.write(keys.data(), keys.size());
    f.write(data.data(), data.size());
    return bool(f);
}
std::string sfo_str(const Sfo& s, const std::string& k) { auto it = s.find(k); return it == s.end() || it->second.first ? "" : it->second.second; }
u32 sfo_int(const Sfo& s, const std::string& k) {
    auto it = s.find(k);
    if (it == s.end() || !it->second.first || it->second.second.size() < 4) return 0;
    const auto& v = it->second.second;
    return u32(u8(v[0])) | u32(u8(v[1])) << 8 | u32(u8(v[2])) << 16 | u32(u8(v[3])) << 24;
}
std::pair<bool, std::string> sfo_intval(u32 v) { return {true, std::string{char(v), char(v >> 8), char(v >> 16), char(v >> 24)}}; }
void put_str(u32 ea, const std::string& v, size_t max) { std::memset(mem::base + ea, 0, max); std::memcpy(mem::base + ea, v.data(), std::min(v.size(), max - 1)); }

// ---- name checks (sysutil_check_name_string / check_filename) ------------------------------
// 0 ok, -1 bad character, -2 bad length.
s32 check_name(const char* src, s32 minlen, s32 maxlen) {
    const s32 lastpos = kSdk370 ? std::max(maxlen - 1, 0) : maxlen;
    char cur = src[0];
    if (cur == '_') return -1;
    for (s32 i = 0;; cur = src[++i]) {
        if (cur == '\0' || i == maxlen) return (minlen > i || (maxlen == i && src[lastpos])) ? -2 : 0;
        if ((cur >= 'A' && cur <= 'Z') || (cur >= '0' && cur <= '9') || cur == '-' || cur == '_') continue;
        return -1;
    }
}
s32 check_name_ea(u32 ea, s32 minlen, s32 maxlen) { return check_name(reinterpret_cast<const char*>(gptr(ea)), minlen, maxlen); }
bool valid_dir_name(const std::string& n) { return !n.empty() && check_name(n.c_str(), 1, DIRNAME_SIZE) == 0; }

int check_filename(const std::string& p, bool disallow_system_files, bool account_sfo_pfd) {
    if (p.size() >= FILENAME_SIZE) return 71;
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) dot = p.size();
    if (p.empty() || dot > 8 || p.size() - dot > 4) return 70;
    if (p == "." || (!account_sfo_pfd && (p == "PARAM.SFO" || p == "PARAM.PFD"))) return 70;
    if (dot) {
        std::string name = p.substr(0, dot);
        std::replace(name.begin(), name.end(), '.', '-');
        if (name[0] == '_') name[0] = '-';
        if (disallow_system_files && (name.starts_with("PARAM") || name.starts_with("ICON") || name.starts_with("PIC") ||
                                      name.starts_with("SND")))
            return 70;
        if (check_name(name.c_str(), 1, 9) == -1) return 70;
    }
    if (p.size() > dot + 1) {
        std::string ext = p.substr(dot + 1);
        if (ext[0] == '_') ext[0] = '-';
        if (check_name(ext.c_str(), 1, 4) == -1) return 70;
    }
    return 0;
}

// ---- host filesystem helpers -------------------------------------------------------------
struct HostStat { bool ok = false, is_dir = false; u64 size = 0; s64 atime = 0, mtime = 0, ctime = 0; timespec at{}, mt{}; };
HostStat host_stat(const std::string& p) {
    HostStat r;
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return r;
    r.ok = true; r.is_dir = S_ISDIR(st.st_mode); r.size = u64(st.st_size);
    r.atime = st.st_atime; r.mtime = st.st_mtime; r.ctime = st.st_ctime;
#if defined(__APPLE__)
    r.at = st.st_atimespec; r.mt = st.st_mtimespec;
#elif defined(_WIN32)
    r.at.tv_sec = st.st_atime; r.mt.tv_sec = st.st_mtime;
#else
    r.at = st.st_atim; r.mt = st.st_mtim;
#endif
    return r;
}
std::vector<std::string> dir_names(const std::string& path) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) out.push_back(it->path().filename().string());
    return out;
}
std::vector<std::string> split(const std::string& s, char sep, bool skip_empty) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t pos = s.find(sep, start);
        std::string part = s.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
        if (!skip_empty || !part.empty()) out.push_back(std::move(part));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return out;
}
bool read_file(const std::string& p, std::vector<u8>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}
bool write_file(const std::string& p, const std::vector<u8>& d) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
    return bool(f);
}

struct Entry { std::string dirName, listParam, title, subtitle, detail; u64 size = 0; s64 atime = 0, mtime = 0, ctime = 0; };

const char* op_name(u32 op) {
    switch (op) {
    case OP_LIST_AUTO_SAVE: return "ListAutoSave";
    case OP_LIST_AUTO_LOAD: return "ListAutoLoad";
    case OP_LIST_SAVE: return "ListSave2";
    case OP_LIST_LOAD: return "ListLoad2";
    default: return "?";
    }
}

// Calls a savedata callback with (result, get, set); returns cbResult->result.
s32 call_cb(PPUContext& c, u32 area, u32 func, u32 get, u32 set) {
    std::memset(mem::base + area + kResult, 0, CBResult::userdata);
    PPUContext saved = c;
    guest_call(c, func, {area + kResult, get, set});
    c = saved;
    return s32(ld32(mem::base, area + kResult + CBResult::result));
}
// What RPCS3's display_callback_result_error_message returns (no dialog is shown here).
u32 cb_error(const char* which, s32 res) {
    LOG_ERR("cellSaveData: %s returned result %d", which, res);
    return (res >= CBRESULT_ERR_INVALID && res <= -1) ? CELL_SAVEDATA_ERROR_CBRESULT : CELL_SAVEDATA_ERROR_PARAM;
}
u32 param_error(int code) { LOG_ERR("cellSaveData: parameter error %d", code); return CELL_SAVEDATA_ERROR_PARAM; }

// savedata_check_args for the list operations (unk_op_flags = 2: funcStat/funcFile are checked later).
int check_args(u32 op, u32 version, u32 errDialog, u32 setList, u32 setBuf, u32 funcList, u32 funcFixed) {
    if (version > 1) return 1;
    if (errDialog > 2) return 5;
    if (!setList) return 11;
    if (ld32(mem::base, setList + SetList::sortType) > SORTTYPE_SUBTITLE) return 12;
    if (ld32(mem::base, setList + SetList::sortOrder) > SORTORDER_ASCENT) return 13;
    const u32 prefix = ld32(mem::base, setList + SetList::dirNamePrefix);
    if (!prefix) return 15;
    if (!std::memchr(gptr(prefix), 0, PREFIX_SIZE) || (kSdk370 && !mem::base[prefix])) return 17;
    for (const std::string& p : split(read_cstr(prefix, PREFIX_SIZE), '|', true)) {
        if (p.size() > DIRNAME_SIZE) return 17;
        switch (check_name(p.c_str(), 1, DIRNAME_SIZE)) { case -1: return 16; case -2: return 17; default: break; }
    }
    if (ld32(mem::base, setList + SetList::reserved)) return 14;
    if (!setBuf) return 74;
    const u32 dirListMax = ld32(mem::base, setBuf + SetBuf::dirListMax), fileListMax = ld32(mem::base, setBuf + SetBuf::fileListMax);
    const u32 bufSize = ld32(mem::base, setBuf + SetBuf::bufSize);
    if (dirListMax > DIRLIST_MAX) return 8;
    if (u64(dirListMax) * DirList::size > bufSize) return 7;
    if (u64(fileListMax) * FileStat::size > bufSize) return 7;
    if (bufSize && !ld32(mem::base, setBuf + SetBuf::buf)) return 6;
    for (u32 i = 0; i < 6; ++i) if (ld32(mem::base, setBuf + SetBuf::reserved + 4 * i)) return 10;
    if ((op == OP_LIST_SAVE || op == OP_LIST_LOAD) && !funcList) return 18;
    if ((op == OP_LIST_AUTO_SAVE || op == OP_LIST_AUTO_LOAD) && !funcFixed) return 19;
    return 0;
}

u32 savedata_op(PPUContext& c, u32 op, u32 version, u32 errDialog, u32 setList, u32 setBuf, u32 funcList,
                u32 funcFixed, u32 funcStat, u32 funcFile, u32 userdata) {
    if (const int e = check_args(op, version, errDialog, setList, setBuf, funcList, funcFixed)) return param_error(e);

    std::unique_lock lock(g_savedata_lock, std::try_to_lock);
    if (!lock) return CELL_SAVEDATA_ERROR_BUSY;

    static const u32 area = mem::alloc_main(0x1000, 0x100);
    std::memset(mem::base + area, 0, kAreaSize);
    const u32 result = area + kResult, listGet = area + kListGet, listSet = area + kListSet, fixedSet = area + kFixedSet;
    const u32 statGet = area + kStatGet, statSet = area + kStatSet, fileGet = area + kFileGet, fileSet = area + kFileSet;
    st32(mem::base, result + CBResult::userdata, userdata);

    const std::string base_dir = vfs_host_path("/dev_hdd0/home/00000001/savedata/");
    const u32 buf = ld32(mem::base, setBuf + SetBuf::buf), bufSize = ld32(mem::base, setBuf + SetBuf::bufSize);
    const u32 dirListMax = ld32(mem::base, setBuf + SetBuf::dirListMax), fileListMax = ld32(mem::base, setBuf + SetBuf::fileListMax);
    const std::string prefix_str = read_cstr(ld32(mem::base, setList + SetList::dirNamePrefix), PREFIX_SIZE);
    LOG_NOTE("cellSaveData%s(version=%u, prefix='%s', sort=%u/%u, dirListMax=%u, fileListMax=%u, bufSize=0x%x)", op_name(op),
             version, prefix_str.c_str(), ld32(mem::base, setList + SetList::sortType), ld32(mem::base, setList + SetList::sortOrder),
             dirListMax, fileListMax, bufSize);

    // ---- directory list
    std::vector<std::string> prefixes = split(prefix_str, '|', true);
    if (prefixes.empty() && op == OP_LIST_LOAD) prefixes = {""};
    std::vector<Entry> entries;
    u32 dirNum = 0, dirListNum = 0;
    for (const std::string& name : dir_names(base_dir)) {
        const HostStat ds = host_stat(base_dir + name);
        if (!ds.is_dir || check_name(name.c_str(), 1, DIRNAME_SIZE) != 0) continue;
        for (const std::string& prefix : prefixes) {
            if (!name.starts_with(prefix)) continue;
            if (!dirNum++) std::memset(mem::base + buf, 0, bufSize);
            if (dirListNum < dirListMax) {
                ++dirListNum;
                Sfo sfo;
                if (!sfo_load(base_dir + name + "/PARAM.SFO", sfo) || sfo.empty()) break;
                Entry e;
                e.dirName = sfo_str(sfo, "SAVEDATA_DIRECTORY");
                e.listParam = sfo_str(sfo, "SAVEDATA_LIST_PARAM");
                e.title = sfo_str(sfo, "TITLE");
                e.subtitle = sfo_str(sfo, "SUB_TITLE");
                e.detail = sfo_str(sfo, "DETAIL");
                if (!valid_dir_name(e.dirName)) {
                    LOG_ERR("cellSaveData: '%s' has an invalid SAVEDATA_DIRECTORY ('%s')", name.c_str(), e.dirName.c_str());
                    break;
                }
                for (const std::string& f : dir_names(base_dir + name)) {
                    const HostStat fsx = host_stat(base_dir + name + "/" + f);
                    if (fsx.ok && !fsx.is_dir && !check_filename(f, false, true)) e.size += fsx.size;
                }
                e.atime = ds.atime; e.mtime = ds.mtime; e.ctime = ds.ctime;
                entries.push_back(std::move(e));
            }
            break;
        }
    }
    {
        const u32 type = ld32(mem::base, setList + SetList::sortType);
        auto comp = [type](const Entry& a, const Entry& b) {
            if (type == SORTTYPE_MODIFIEDTIME) return a.mtime == b.mtime ? a.subtitle < b.subtitle : a.mtime < b.mtime;
            return a.subtitle == b.subtitle ? a.mtime < b.mtime : a.subtitle < b.subtitle;
        };
        if (ld32(mem::base, setList + SetList::sortOrder) == SORTORDER_ASCENT) std::sort(entries.begin(), entries.end(), comp);
        else std::sort(entries.rbegin(), entries.rend(), comp);
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        put_str(buf + DirList::size * u32(i) + DirList::dirName, entries[i].dirName, DIRNAME_SIZE);
        put_str(buf + DirList::size * u32(i) + DirList::listParam, entries[i].listParam, 8);
    }
    st32(mem::base, listGet + ListGet::dirNum, dirNum);
    st32(mem::base, listGet + ListGet::dirListNum, dirListNum);
    st32(mem::base, listGet + ListGet::dirList, buf);

    std::string dir_name;
    if (funcList) {
        st32(mem::base, listSet + ListSet::focusPosition, FOCUSPOS_LISTHEAD);
        const s32 res = call_cb(c, area, funcList, listGet, listSet);
        if (res != CBRESULT_OK_NEXT) {
            if (res == CBRESULT_OK_LAST || res == CBRESULT_OK_LAST_NOCONFIRM) { LOG_NOTE("cellSaveData%s: funcList ended (%d)", op_name(op), res); return CELL_OK; }
            return cb_error("funcList", res);
        }
        const u32 fixedListNum = ld32(mem::base, listSet + ListSet::fixedListNum), fixedList = ld32(mem::base, listSet + ListSet::fixedList);
        if (fixedListNum > LISTITEM_MAX) return param_error(38);
        if (fixedListNum && !fixedList) return param_error(39);
        std::set<std::string> selected_list;
        for (u32 i = 0; i < fixedListNum; ++i) {
            const u32 ea = fixedList + DirList::size * i;
            switch (check_name_ea(ea, 1, DIRNAME_SIZE)) {
            case -1: return param_error(40);
            case -2: if (mem::base[ea]) return param_error(41); break;
            default: break;
            }
            selected_list.insert(read_cstr(ea, DIRNAME_SIZE));
        }
        std::erase_if(entries, [&](const Entry& e) { return !selected_list.count(e.dirName); });

        const u32 newData = ld32(mem::base, listSet + ListSet::newData);
        std::string new_dir;
        if (newData) {
            const u32 pos = ld32(mem::base, newData + ListNewData::iconPosition);
            if (pos != ICONPOS_HEAD && pos != ICONPOS_TAIL) return param_error(43);
            const u32 nd = ld32(mem::base, newData + ListNewData::dirName);
            if (!nd) return param_error(44);
            switch (check_name_ea(nd, 1, DIRNAME_SIZE)) {
            case -1: return param_error(45);
            case -2: if (mem::base[nd]) return param_error(46); break;
            default: break;
            }
            new_dir = read_cstr(nd, DIRNAME_SIZE);
        }

        const u32 focus = ld32(mem::base, listSet + ListSet::focusPosition);
        s32 focused = -1;
        switch (focus) {
        case FOCUSPOS_DIRNAME: {
            const u32 fd = ld32(mem::base, listSet + ListSet::focusDirName);
            if (!fd) return param_error(35);
            switch (check_name_ea(fd, 1, DIRNAME_SIZE)) {
            case -1: return param_error(36);
            case -2: if (mem::base[fd]) return param_error(37); break;
            default: break;
            }
            const std::string want = read_cstr(fd, DIRNAME_SIZE);
            for (size_t i = 0; i < entries.size(); ++i) if (entries[i].dirName == want) { focused = s32(i); break; }
            break;
        }
        case FOCUSPOS_LISTHEAD: focused = entries.empty() ? -1 : 0; break;
        case FOCUSPOS_LISTTAIL: focused = s32(entries.size()) - 1; break;
        case FOCUSPOS_LATEST: {
            s64 best = INT64_MIN;
            for (size_t i = 0; i < entries.size(); ++i) if (entries[i].mtime > best) { focused = s32(i); best = entries[i].mtime; }
            break;
        }
        case FOCUSPOS_OLDEST: {
            s64 best = INT64_MAX;
            for (size_t i = 0; i < entries.size(); ++i) if (entries[i].mtime < best) { focused = s32(i); best = entries[i].mtime; }
            break;
        }
        case FOCUSPOS_NEWDATA:
            if (!newData) return param_error(34);
            break;
        default: return param_error(34);
        }

        LOG_NOTE("cellSaveData%s: funcList chose focus %u, %zu listed, newData '%s'", op_name(op), focus, entries.size(), new_dir.c_str());
        // No selection dialog. Saving: the game focuses the newest save and offers a new one, and the
        // player would pick. Overwrite only the save being played (the game's current slot, kept in its
        // save manager at *userdata + 0xec, -1 after New Game); otherwise make the new save, so New Game
        // never writes over an existing save. Loading: the focused entry, the newest save.
        if (op == OP_LIST_SAVE && newData) {
            focused = -1;
            const u32 mgr = userdata ? ld32(mem::base, userdata) : 0;
            const s32 slot = mgr ? s32(ld32(mem::base, mgr + 0xec)) : -1;
            const std::string current = slot >= 0 ? prefix_str + std::to_string(slot) : std::string();
            for (size_t i = 0; i < entries.size(); ++i) if (!current.empty() && entries[i].dirName == current) focused = s32(i);
            LOG_NOTE("cellSaveData%s: current slot %d -> %s", op_name(op), slot, focused >= 0 ? current.c_str() : new_dir.c_str());
        }
        if (focus != FOCUSPOS_NEWDATA && focused >= 0 && size_t(focused) < entries.size()) dir_name = entries[size_t(focused)].dirName;
        else if (newData) dir_name = new_dir;
        else {
            LOG_NOTE("cellSaveData%s: nothing to select (%zu entries, focus %u, no newData) -> cancel", op_name(op), entries.size(), focus);
            return CELL_SAVEDATA_RET_CANCEL;
        }
    }

    if (funcFixed) {
        const s32 res = call_cb(c, area, funcFixed, listGet, fixedSet);
        if (res != CBRESULT_OK_NEXT) {
            if (res == CBRESULT_OK_LAST) { LOG_NOTE("cellSaveData%s: funcFixed ended (OK_LAST)", op_name(op)); return CELL_OK; }
            return cb_error("funcFixed", res);
        }
        const u32 dn = ld32(mem::base, fixedSet + FixedSet::dirName);
        if (!dn) return param_error(26);
        switch (check_name_ea(dn, 1, DIRNAME_SIZE)) { case -1: return param_error(27); case -2: return param_error(28); default: break; }
        const u32 option = ld32(mem::base, fixedSet + FixedSet::option);
        if (option > 1) return param_error(81);  // NONE / NOCONFIRM: no confirmation dialog for list auto ops either way
        dir_name = read_cstr(dn, DIRNAME_SIZE);
    }

    if (dirNum) std::memset(mem::base + buf, 0, bufSize);

    if (!valid_dir_name(dir_name)) { LOG_ERR("cellSaveData%s: invalid directory name '%s'", op_name(op), dir_name.c_str()); return CELL_SAVEDATA_ERROR_BROKEN; }

    const std::string dir_path = base_dir + dir_name, old_path = base_dir + ".backup_" + dir_name,
                      new_path = base_dir + ".working_" + dir_name;
    Sfo sfo;
    sfo_load(dir_path + "/PARAM.SFO", sfo);
    bool has_modified = false, recreated = false;
    std::vector<std::string> blist;
    if (sfo.count("RPCS3_BLIST")) blist = split(sfo_str(sfo, "RPCS3_BLIST"), '/', false);

    // ---- stat callback
    if (!funcStat) return param_error(20);
    const bool is_new = sfo.empty();
    LOG_NOTE("cellSaveData%s: dir %s (%s)", op_name(op), dir_name.c_str(), is_new ? "new" : "existing");
    {
        const HostStat ds = host_stat(dir_path);
        st32(mem::base, statGet + StatGet::hddFreeSizeKB, u32(kHddFreeKB));
        st32(mem::base, statGet + StatGet::isNewData, is_new ? 1 : 0);
        st64(mem::base, statGet + StatGet::dir + DirStat::atime, u64(ds.atime));
        st64(mem::base, statGet + StatGet::dir + DirStat::mtime, u64(ds.mtime));
        st64(mem::base, statGet + StatGet::dir + DirStat::ctime, u64(ds.ctime));
        put_str(statGet + StatGet::dir + DirStat::dirName, dir_name, DIRNAME_SIZE);
        if (!is_new) {
            const u32 gp = statGet + StatGet::getParam;
            st32(mem::base, gp + SysParam::parental_level, sfo_int(sfo, "PARENTAL_LEVEL"));
            st32(mem::base, gp + SysParam::attribute, sfo_int(sfo, "ATTRIBUTE"));
            put_str(gp + SysParam::title, sfo_str(sfo, "TITLE"), 128);
            put_str(gp + SysParam::subTitle, sfo_str(sfo, "SUB_TITLE"), 128);
            put_str(gp + SysParam::detail, sfo_str(sfo, "DETAIL"), 1024);
            put_str(gp + SysParam::listParam, sfo_str(sfo, "SAVEDATA_LIST_PARAM"), 8);
        }
        st32(mem::base, statGet + StatGet::bind, 0);
        st32(mem::base, statGet + StatGet::fileList, buf);
        if (!is_new) std::memset(mem::base + buf, 0, bufSize);

        struct F { std::string name; HostStat st; };
        std::vector<F> files;
        for (const std::string& n : dir_names(dir_path)) {
            const HostStat st = host_stat(dir_path + "/" + n);
            if (st.ok && !st.is_dir && !check_filename(n, false, false)) files.push_back({n, st});
        }
        std::sort(files.begin(), files.end(), [&](const F& a, const F& b) {
            const auto ai = std::find(blist.begin(), blist.end(), a.name), bi = std::find(blist.begin(), blist.end(), b.name);
            if (ai == blist.end() && bi == blist.end()) return a.name < b.name;
            return ai < bi;
        });
        u32 fileNum = 0, fileListNum = 0;
        u64 size_bytes = 0;
        for (const F& f : files) {
            ++fileNum;
            size_bytes += (f.st.size + 1023) & ~u64(1023);
            if (fileListNum >= fileListMax) continue;
            const u32 fe = buf + FileStat::size * fileListNum++;
            u32 type = FILETYPE_NORMALFILE;
            if (f.name == "ICON0.PNG") type = FILETYPE_ICON0;
            else if (f.name == "ICON1.PAM") type = FILETYPE_ICON1;
            else if (f.name == "PIC1.PNG") type = FILETYPE_PIC1;
            else if (f.name == "SND0.AT3") type = FILETYPE_SND0;
            else if (sfo_int(sfo, "*" + f.name)) type = FILETYPE_SECUREFILE;
            st32(mem::base, fe + FileStat::fileType, type);
            st64(mem::base, fe + FileStat::fsize, f.st.size);
            st64(mem::base, fe + FileStat::atime, u64(f.st.atime));
            st64(mem::base, fe + FileStat::mtime, u64(f.st.mtime));
            st64(mem::base, fe + FileStat::ctime, u64(f.st.ctime));
            put_str(fe + FileStat::fileName, f.name, FILENAME_SIZE);
        }
        st32(mem::base, statGet + StatGet::fileNum, fileNum);
        st32(mem::base, statGet + StatGet::fileListNum, fileListNum);
        st32(mem::base, statGet + StatGet::sysSizeKB, 35);
        st32(mem::base, statGet + StatGet::sizeKB, is_new ? 0 : u32(size_bytes / 1024 + 35));

        const s32 res = call_cb(c, area, funcStat, statGet, statSet);
        if (res != CBRESULT_OK_NEXT) {
            if (res == CBRESULT_OK_LAST) { LOG_NOTE("cellSaveData%s: funcStat ended (OK_LAST)", op_name(op)); return CELL_OK; }
            return cb_error("funcStat", res);
        }

        const u32 sp = ld32(mem::base, statSet + StatSet::setParam);
        if (sp) {
            const u32 attribute = ld32(mem::base, sp + SysParam::attribute), parental = ld32(mem::base, sp + SysParam::parental_level);
            if (attribute > 1) return param_error(57);
            if (parental > 11) return param_error(58);
            const size_t lp_max = kSdk370 ? 7 : 8;
            for (size_t i = 0;; ++i) {
                const u8 ch = mem::base[sp + SysParam::listParam + i];
                if (ch == 0 || i >= lp_max) { if (ch) return param_error(76); break; }
                if ((ch < 'A' || ch > 'Z') && (ch < '0' || ch > '9') && ch != '-' && ch != '_') return param_error(77);
            }
            for (u32 i = 0; i < SysParam::reservedSize; ++i) if (mem::base[sp + SysParam::reserved + i]) return param_error(59);
            sfo["ACCOUNT_ID"] = {false, "0000000000000000"};
            sfo["ATTRIBUTE"] = sfo_intval(attribute);
            sfo["CATEGORY"] = {false, "SD"};
            sfo["PARAMS"] = {false, ""};
            sfo["PARAMS2"] = {false, ""};
            sfo["PARENTAL_LEVEL"] = sfo_intval(parental);
            sfo["DETAIL"] = {false, read_cstr(sp + SysParam::detail, 1024)};
            sfo["SAVEDATA_DIRECTORY"] = {false, dir_name};
            sfo["SAVEDATA_LIST_PARAM"] = {false, read_cstr(sp + SysParam::listParam, 8)};
            sfo["SUB_TITLE"] = {false, read_cstr(sp + SysParam::subTitle, 128)};
            sfo["TITLE"] = {false, read_cstr(sp + SysParam::title, 128)};
            has_modified = true;
            LOG_NOTE("cellSaveData%s: setParam title='%s' subTitle='%s'", op_name(op), sfo["TITLE"].second.c_str(), sfo["SUB_TITLE"].second.c_str());
        } else if (is_new) {
            return param_error(50);
        }

        const u32 mode = ld32(mem::base, statSet + StatSet::reCreateMode);
        switch (mode & RECREATE_MASK) {
        case RECREATE_NO: case RECREATE_NO_NOBROKEN: break;
        case RECREATE_YES: case RECREATE_YES_RESET_OWNER:
            if (!sp) return param_error(50);
            LOG_NOTE("cellSaveData%s: recreating %s (mode %u)", op_name(op), dir_name.c_str(), mode);
            std::erase_if(sfo, [](const auto& kv) { return !kv.first.empty() && kv.first[0] == '*'; });
            blist.clear();
            recreated = true;
            has_modified = true;
            break;
        default: return param_error(48);
        }
    }

    std::error_code ec;
    if (!sfo.empty() && is_new) {
        fs::create_directories(dir_path, ec);
        if (ec) { LOG_ERR("cellSaveData: cannot create %s (%s)", dir_path.c_str(), ec.message().c_str()); return CELL_SAVEDATA_ERROR_ACCESS_ERROR; }
    }

    // ---- file callback loop (operates on an in-memory copy, committed at the end)
    std::map<std::string, std::vector<u8>> all_files;
    std::map<std::string, std::pair<timespec, timespec>> all_times;
    if (!recreated) {
        for (const std::string& n : dir_names(dir_path)) {
            const HostStat st = host_stat(dir_path + "/" + n);
            if (!st.ok || st.is_dir || check_filename(n, false, true)) continue;
            std::vector<u8> d;
            if (!read_file(dir_path + "/" + n, d)) continue;
            all_times[n] = {st.at, st.mt};
            all_files[n] = std::move(d);
        }
    }
    st32(mem::base, fileGet + FileGet::excSize, 0);

    u32 savedata_result = CELL_OK;
    while (funcFile) {
        std::memset(mem::base + fileSet, 0, FileSet::size);
        std::memset(mem::base + fileGet + FileGet::reserved, 0, 64);
        const s32 res = call_cb(c, area, funcFile, fileGet, fileSet);
        if (res != CBRESULT_OK_NEXT) {
            if (res == CBRESULT_OK_LAST || res == CBRESULT_OK_LAST_NOCONFIRM) break;
            savedata_result = cb_error("funcFile", res);
            break;
        }
        const u32 type = ld32(mem::base, fileSet + FileSet::fileType), fop = ld32(mem::base, fileSet + FileSet::fileOperation);
        const u32 offset = ld32(mem::base, fileSet + FileSet::fileOffset), fsize = ld32(mem::base, fileSet + FileSet::fileSize);
        const u32 fbufsize = ld32(mem::base, fileSet + FileSet::fileBufSize), fbuf = ld32(mem::base, fileSet + FileSet::fileBuf);
        std::string path;
        switch (type) {
        case FILETYPE_SECUREFILE: case FILETYPE_NORMALFILE: {
            const u32 fn = ld32(mem::base, fileSet + FileSet::fileName);
            if (!fn) { savedata_result = param_error(69); break; }
            const void* term = std::memchr(gptr(fn), 0, FILENAME_SIZE);
            if (!term) { savedata_result = param_error(71); break; }
            path.assign(reinterpret_cast<const char*>(gptr(fn)), static_cast<const char*>(term));
            if (const int e = check_filename(path, true, false)) savedata_result = param_error(e);
            break;
        }
        case FILETYPE_ICON0: path = "ICON0.PNG"; break;
        case FILETYPE_ICON1: path = "ICON1.PAM"; break;
        case FILETYPE_PIC1: path = "PIC1.PNG"; break;
        case FILETYPE_SND0: path = "SND0.AT3"; break;
        default: LOG_ERR("cellSaveData: unknown fileType %u", type); savedata_result = param_error(61); break;
        }
        if (savedata_result) break;
        if ((path == "." || path == "..") && fop <= FILEOP_WRITE_NOTRUNC) { savedata_result = CELL_SAVEDATA_ERROR_BROKEN; break; }

        auto add_to_blist = [&](const std::string& n) {
            if (std::find(blist.begin(), blist.end(), n) != blist.end()) return;
            if (auto it = std::find(blist.begin(), blist.end(), ""); it != blist.end()) *it = n; else blist.push_back(n);
        };
        switch (fop) {
        case FILEOP_READ: {
            if (fbufsize < fsize) { savedata_result = param_error(72); break; }
            if (!fbuf && fbufsize) { savedata_result = param_error(73); break; }
            const auto it = all_files.find(path);
            if (it == all_files.end() || it->second.size() <= offset) {
                LOG_ERR("cellSaveData: read %s/%s failed (size=%lld, offset=%u)", dir_name.c_str(), path.c_str(),
                        it == all_files.end() ? -1LL : (long long)it->second.size(), offset);
                savedata_result = CELL_SAVEDATA_ERROR_FAILURE;
                break;
            }
            const u32 n = u32(std::min<u64>(fsize, it->second.size() - offset));
            if (n) std::memcpy(mem::base + fbuf, it->second.data() + offset, n);
            st32(mem::base, fileGet + FileGet::excSize, n);
            LOG_NOTE("cellSaveData: read %s/%s offset %u -> %u bytes", dir_name.c_str(), path.c_str(), offset, n);
            break;
        }
        case FILEOP_WRITE: case FILEOP_WRITE_NOTRUNC: {
            if (fbufsize < fsize) { savedata_result = param_error(72); break; }
            if (!fbuf && fbufsize) { savedata_result = param_error(73); break; }
            std::vector<u8>& v = all_files[path];
            const size_t end = size_t(offset) + fsize;
            if (fop == FILEOP_WRITE) v.resize(end);
            else if (v.size() < end) v.resize(end);
            if (fsize) std::memcpy(v.data() + offset, mem::base + fbuf, fsize);
            st32(mem::base, fileGet + FileGet::excSize, fsize);
            all_times.erase(path);
            add_to_blist(path);
            has_modified = true;
            LOG_NOTE("cellSaveData: write%s %s/%s offset %u, %u bytes (file now %zu)", fop == FILEOP_WRITE ? "" : "(notrunc)",
                     dir_name.c_str(), path.c_str(), offset, fsize, v.size());
            break;
        }
        case FILEOP_DELETE: {
            if (!all_files.erase(path)) {
                LOG_ERR("cellSaveData: delete %s/%s failed", dir_name.c_str(), path.c_str());
                savedata_result = CELL_SAVEDATA_ERROR_FAILURE;
                break;
            }
            sfo.erase("*" + path);
            st32(mem::base, fileGet + FileGet::excSize, 0);
            all_times.erase(path);
            if (auto it = std::find(blist.begin(), blist.end(), path); it != blist.end()) it->clear();
            has_modified = true;
            LOG_NOTE("cellSaveData: delete %s/%s", dir_name.c_str(), path.c_str());
            break;
        }
        default: LOG_ERR("cellSaveData: unknown fileOperation %u", fop); savedata_result = param_error(60); break;
        }
        if (savedata_result) break;
        if (fop != FILEOP_DELETE) sfo.try_emplace("*" + path, sfo_intval(type == FILETYPE_SECUREFILE ? 1 : 0));
    }

    // ---- commit PARAM.SFO and files: build .working_<dir>, swap it in via .backup_<dir>
    if (!sfo.empty() && has_modified) {
        std::string joined;
        for (size_t i = 0; i < blist.size(); ++i) { if (i) joined += '/'; joined += blist[i]; }
        sfo["RPCS3_BLIST"] = {false, joined};
        fs::remove_all(new_path, ec);
        ec.clear();
        fs::create_directories(new_path, ec);
        bool ok = !ec;
        ok = ok && sfo_save(new_path + "/PARAM.SFO", sfo);
        u64 total = 0;
        for (const auto& [n, d] : all_files) {
            if (!ok) break;
            if (n == "PARAM.SFO") continue;
            ok = write_file(new_path + "/" + n, d);
            total += d.size();
        }
        if (ok) {
            for (const auto& [n, t] : all_times) {
                plat::set_file_times(new_path + "/" + n, t.first, t.second);
            }
            fs::remove_all(old_path, ec);
            ec.clear();
            if (fs::exists(dir_path, ec)) fs::rename(dir_path, old_path, ec);
            if (!ec) fs::rename(new_path, dir_path, ec);
            ok = !ec;
            if (ok) fs::remove_all(old_path, ec);
        }
        if (!ok) {
            LOG_ERR("cellSaveData: failed to commit %s (%s)", dir_path.c_str(), ec ? ec.message().c_str() : "write error");
            return CELL_SAVEDATA_ERROR_ACCESS_ERROR;
        }
        LOG_NOTE("cellSaveData%s: committed %s (%zu files, %llu bytes)", op_name(op), dir_name.c_str(), all_files.size(),
                 (unsigned long long)total);
        if (!savedata_result) overlay_notify("Saved: %s (%s)", sfo_str(sfo, "SUB_TITLE").c_str(), dir_name.c_str());
    } else if (!savedata_result && (op == OP_LIST_LOAD || op == OP_LIST_AUTO_LOAD) && !sfo.empty()) {
        overlay_notify("Loaded: %s (%s)", sfo_str(sfo, "SUB_TITLE").c_str(), dir_name.c_str());
    }
    if (savedata_result) LOG_ERR("cellSaveData%s: %s -> 0x%08x", op_name(op), dir_name.c_str(), savedata_result);
    return savedata_result;
}

// Keeps host-side filesystem/allocation exceptions out of guest code; guest-thread control flow
// (ThreadExit/ProcessExit) and guest_call failures still propagate.
u32 savedata_guarded(PPUContext& c, u32 op, u32 version, u32 errDialog, u32 setList, u32 setBuf, u32 funcList,
                     u32 funcFixed, u32 funcStat, u32 funcFile, u32 userdata) {
    try {
        return savedata_op(c, op, version, errDialog, setList, setBuf, funcList, funcFixed, funcStat, funcFile, userdata);
    } catch (const fs::filesystem_error& e) {
        LOG_ERR("cellSaveData%s: filesystem error: %s", op_name(op), e.what());
        return CELL_SAVEDATA_ERROR_ACCESS_ERROR;
    } catch (const std::bad_alloc&) {
        LOG_ERR("cellSaveData%s: out of memory", op_name(op));
        return CELL_SAVEDATA_ERROR_INTERNAL;
    }
}

// (version, setList, setBuf, funcList, funcStat, funcFile, container, userdata)
void cellSaveDataListSave2(PPUContext& c) {
    RET(savedata_guarded(c, OP_LIST_SAVE, u32(ARG(0)), 1, u32(ARG(1)), u32(ARG(2)), u32(ARG(3)), 0, u32(ARG(4)), u32(ARG(5)), u32(ARG(7))));
}
void cellSaveDataListLoad2(PPUContext& c) {
    RET(savedata_guarded(c, OP_LIST_LOAD, u32(ARG(0)), 1, u32(ARG(1)), u32(ARG(2)), u32(ARG(3)), 0, u32(ARG(4)), u32(ARG(5)), u32(ARG(7))));
}
// (version, errDialog, setList, setBuf, funcFixed, funcStat, funcFile, container, userdata)
void cellSaveDataListAutoSave(PPUContext& c) {
    RET(savedata_guarded(c, OP_LIST_AUTO_SAVE, u32(ARG(0)), u32(ARG(1)), u32(ARG(2)), u32(ARG(3)), 0, u32(ARG(4)), u32(ARG(5)), u32(ARG(6)), u32(ARG(8))));
}
void cellSaveDataListAutoLoad(PPUContext& c) {
    RET(savedata_guarded(c, OP_LIST_AUTO_LOAD, u32(ARG(0)), u32(ARG(1)), u32(ARG(2)), u32(ARG(3)), 0, u32(ARG(4)), u32(ARG(5)), u32(ARG(6)), u32(ARG(8))));
}
// (container): the firmware shows a list of this title's saves to delete; without a UI the user always cancels.
void cellSaveDataDelete2(PPUContext& c) {
    LOG_NOTE("cellSaveDataDelete2(container=0x%x): no save data UI -> CELL_SAVEDATA_RET_CANCEL", u32(ARG(0)));
    RET(CELL_SAVEDATA_RET_CANCEL);
}
}  // namespace

void register_savedata() {
    register_import(0x21425307, "cellSaveDataListAutoLoad", cellSaveDataListAutoLoad);
    register_import(0x4dd03a4e, "cellSaveDataListAutoSave", cellSaveDataListAutoSave);
    register_import(0x1dfbfdd6, "cellSaveDataListLoad2", cellSaveDataListLoad2);
    register_import(0x2de0d663, "cellSaveDataListSave2", cellSaveDataListSave2);
    register_import(0xedadd797, "cellSaveDataDelete2", cellSaveDataDelete2);
}
