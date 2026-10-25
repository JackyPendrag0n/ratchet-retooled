// Small library modules: cellSysmodule, cellSysutil (video/audio out, system params,
// callbacks, game data, cache, save data), cellPad.
#include "guest.h"

#include <cstring>
#include <filesystem>
#include <mutex>

namespace {
// ---------------------------------------------------------------- cellSysmodule
void cellSysmoduleInitialize(PPUContext& c) { RET(CELL_OK); }
void cellSysmoduleLoadModule(PPUContext& c) { LOG_NOTE("cellSysmoduleLoadModule(0x%04x)", u32(ARG(0))); RET(CELL_OK); }
void cellSysmoduleUnloadModule(PPUContext& c) { RET(CELL_OK); }

// ---------------------------------------------------------------- cellSysutil
std::mutex g_cb_lock;
u32 g_callbacks[4][2];  // (func opd, userdata)

void cellSysutilRegisterCallback(PPUContext& c) {
    const u32 slot = u32(ARG(0));
    if (slot >= 4) { RET(CELL_EINVAL); return; }
    std::lock_guard lk(g_cb_lock);
    g_callbacks[slot][0] = u32(ARG(1));
    g_callbacks[slot][1] = u32(ARG(2));
    RET(CELL_OK);
}
void cellSysutilCheckCallback(PPUContext& c) { RET(CELL_OK); }  // no system events are generated yet

void cellSysutilGetSystemParamInt(PPUContext& c) {
    const u32 id = u32(ARG(0));
    s32 v = 0;
    switch (id) {
    case 0x111: v = 1; break;    // language: English (US)
    case 0x112: v = 1; break;    // enter button: cross
    case 0x114: v = 2; break;    // date format MMDDYYYY
    case 0x115: v = 0; break;    // 12-hour clock
    case 0x116: v = 0; break;    // timezone offset (minutes)
    case 0x117: v = 0; break;
    case 0x121: v = 0; break;    // parental level off
    case 0x122: v = 1; break;    // license area SCEA
    case 0x123: v = 0; break;
    case 0x141: v = 0; break;
    case 0x151: v = 0; break;
    case 0x152: v = 1; break;    // rumble on
    case 0x153: v = 0; break;
    case 0x156: v = 0; break;
    default: LOG_NOTE("cellSysutilGetSystemParamInt(0x%x): unknown id", id); RET(CELL_EINVAL); return;
    }
    st32(mem::base, u32(ARG(1)), u32(v));
    RET(CELL_OK);
}

// Video output: a 1280x720 16:9 display.
void cellVideoOutGetState(PPUContext& c) {
    const u32 out = u32(ARG(0)), st = u32(ARG(2));
    std::memset(mem::base + st, 0, 16);
    if (out == 0) {
        st8(mem::base, st, 2);           // CELL_VIDEO_OUT_OUTPUT_STATE_ENABLED
        st8(mem::base, st + 8, 2);       // resolutionId 720
        st8(mem::base, st + 9, 0);       // progressive
        st8(mem::base, st + 11, 2);      // 16:9
        st16(mem::base, st + 14, 1);     // 59.94 Hz
    } else {
        st8(mem::base, st, 1);           // DISABLED
    }
    RET(CELL_OK);
}
void cellVideoOutGetResolution(PPUContext& c) {
    static const u16 res[][2] = {{0, 0}, {1920, 1080}, {1280, 720}, {0, 0}, {720, 480}, {720, 576}};
    const u32 id = u32(ARG(0));
    if (id >= 6 || !res[id][0]) { RET(CELL_EINVAL); return; }
    st16(mem::base, u32(ARG(1)), res[id][0]); st16(mem::base, u32(ARG(1)) + 2, res[id][1]);
    RET(CELL_OK);
}
void cellVideoOutGetResolutionAvailability(PPUContext& c) { RET(u32(ARG(1)) == 2 || u32(ARG(1)) == 4 ? 1 : 0); }
void cellVideoOutConfigure(PPUContext& c) {
    const u32 cfg = u32(ARG(1));
    LOG_NOTE("cellVideoOutConfigure(res=%u format=%u aspect=%u pitch=%u)", ld8(mem::base, cfg), ld8(mem::base, cfg + 1),
             ld8(mem::base, cfg + 2), ld32(mem::base, cfg + 12));
    RET(CELL_OK);
}
void cellAudioOutConfigure(PPUContext& c) { RET(CELL_OK); }
void cellAudioOutGetSoundAvailability(PPUContext& c) { RET(u32(ARG(1)) == 0 /*LPCM*/ ? 2 : 0); }  // stereo LPCM

// System cache: /dev_hdd1/caches/<id>
void cellSysCacheMount(PPUContext& c) {
    const u32 p = u32(ARG(0));
    const std::string id = read_cstr(p, 32), path = "/dev_hdd1/caches/" + (id.empty() ? std::string("BCUS98127") : id);
    std::filesystem::create_directories(vfs_host_path(path));
    write_cstr(p + 32, path);
    LOG_NOTE("cellSysCacheMount -> %s", path.c_str());
    RET(1);  // CELL_SYSCACHE_RET_OK_RELAYED (existing cache kept)
}
void cellSysCacheClear(PPUContext& c) { RET(CELL_OK); }
void cellDiscGameGetBootDiscInfo(PPUContext& c) {
    const u32 p = u32(ARG(0));
    std::memset(mem::base + p, 0, 32);
    write_cstr(p, "BCUS98127");
    st32(mem::base, p + 12, 3);
    RET(CELL_OK);
}
}  // namespace

void register_misc_modules() {
    register_import(0x9d98afa0, "cellSysutilRegisterCallback", cellSysutilRegisterCallback);
    register_import(0x189a74da, "cellSysutilCheckCallback", cellSysutilCheckCallback);
    register_import(0x40e895d3, "cellSysutilGetSystemParamInt", cellSysutilGetSystemParamInt);
    register_import(0x887572d5, "cellVideoOutGetState", cellVideoOutGetState);
    register_import(0xe558748d, "cellVideoOutGetResolution", cellVideoOutGetResolution);
    register_import(0xa322db75, "cellVideoOutGetResolutionAvailability", cellVideoOutGetResolutionAvailability);
    register_import(0x0bae8772, "cellVideoOutConfigure", cellVideoOutConfigure);
    register_import(0x4692ab35, "cellAudioOutConfigure", cellAudioOutConfigure);
    register_import(0xc01b4e7c, "cellAudioOutGetSoundAvailability", cellAudioOutGetSoundAvailability);
    register_import(0x1e7bff94, "cellSysCacheMount", cellSysCacheMount);
    register_import(0x744c1544, "cellSysCacheClear", cellSysCacheClear);
    register_import(0xb72bc4e6, "cellDiscGameGetBootDiscInfo", cellDiscGameGetBootDiscInfo);
    register_import(0x63ff6ff9, "cellSysmoduleInitialize", cellSysmoduleInitialize);
    register_import(0x32267a31, "cellSysmoduleLoadModule", cellSysmoduleLoadModule);
    register_import(0x112a5ee9, "cellSysmoduleUnloadModule", cellSysmoduleUnloadModule);
}
