// sys_io (cellPad), cellAudio and cellGameData.
#include "audio.h"
#include "input.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

std::function<void(const float*, u32)> audio_sink;

// ================================================================================= cellPad
namespace {
enum : u32 {
    CELL_PAD_ERROR_INVALID_PARAMETER = 0x80121102, CELL_PAD_ERROR_ALREADY_INITIALIZED = 0x80121103,
    CELL_PAD_ERROR_UNINITIALIZED = 0x80121104, CELL_PAD_ERROR_NO_DEVICE = 0x80121107,
    CELL_PAD_SETTING_PRESS_ON = 2, CELL_PAD_SETTING_SENSOR_ON = 4,
};
constexpr u32 kMaxPads = 7;

std::mutex g_pad_lock;
u32 g_pad_max = 0;
u32 g_pad_setting[kMaxPads];
PadState g_pads[kMaxPads];
bool g_pad_reported[kMaxPads];

void cellPadInit(PPUContext& c) {
    const u32 max = std::min<u32>(u32(ARG(0)), kMaxPads);
    std::lock_guard lk(g_pad_lock);
    if (g_pad_max) { RET(CELL_PAD_ERROR_ALREADY_INITIALIZED); return; }
    if (max == 0 || max > kMaxPads) { RET(CELL_PAD_ERROR_INVALID_PARAMETER); return; }
    g_pad_max = max;
    std::memset(g_pad_setting, 0, sizeof g_pad_setting);
    for (u32 i = 1; i < kMaxPads; ++i) g_pads[i].connected = false;
    LOG_NOTE("cellPadInit(%u)", max);
    RET(CELL_OK);
}
void cellPadEnd(PPUContext& c) { std::lock_guard lk(g_pad_lock); g_pad_max = 0; RET(CELL_OK); }

void cellPadGetInfo(PPUContext& c) {
    const u32 info = u32(ARG(0));
    std::lock_guard lk(g_pad_lock);
    if (!g_pad_max) { RET(CELL_PAD_ERROR_UNINITIALIZED); return; }
    // CellPadInfo sizes its arrays by CELL_MAX_PADS (127)
    std::memset(mem::base + info, 0, 12 + 254 + 254 + 127);
    u32 now = 0;
    for (u32 i = 0; i < g_pad_max; ++i) {
        if (!g_pads[i].connected) continue;
        u8 status = 1;                                   // CONNECTED
        if (!g_pad_reported[i]) { status |= 2; g_pad_reported[i] = true; }  // ASSIGN_CHANGES
        mem::base[info + 520 + i] = status;
        st16(mem::base, info + 12 + 2 * i, 0x054C);      // Sony
        st16(mem::base, info + 266 + 2 * i, 0x0268);      // DUALSHOCK 3
        ++now;
    }
    st32(mem::base, info + 0, g_pad_max);
    st32(mem::base, info + 4, now);
    st32(mem::base, info + 8, 0);
    RET(CELL_OK);
}

void cellPadGetData(PPUContext& c) {
    const u32 port = u32(ARG(0)), data = u32(ARG(1));
    std::lock_guard lk(g_pad_lock);
    if (!g_pad_max) { RET(CELL_PAD_ERROR_UNINITIALIZED); return; }
    if (port >= kMaxPads || !data) { RET(CELL_PAD_ERROR_INVALID_PARAMETER); return; }
    if (port >= g_pad_max || !g_pads[port].connected) { RET(CELL_PAD_ERROR_NO_DEVICE); return; }
    const PadState& p = g_pads[port];
    const u32 setting = g_pad_setting[port];
    const u32 len = (setting & CELL_PAD_SETTING_SENSOR_ON) ? 24 : (setting & CELL_PAD_SETTING_PRESS_ON) ? 20 : 8;
    u16 b[24] = {};
    b[1] = u16(0x70 | (len / 2));
    b[2] = p.digital1; b[3] = p.digital2;
    b[4] = p.rx; b[5] = p.ry; b[6] = p.lx; b[7] = p.ly;
    // pressure: full press for any held button
    const u16 d1 = p.digital1, d2 = p.digital2;
    b[8] = (d1 & 0x20) ? 255 : 0;  b[9] = (d1 & 0x80) ? 255 : 0;
    b[10] = (d1 & 0x10) ? 255 : 0; b[11] = (d1 & 0x40) ? 255 : 0;
    b[12] = (d2 & 0x10) ? 255 : 0; b[13] = (d2 & 0x20) ? 255 : 0;
    b[14] = (d2 & 0x40) ? 255 : 0; b[15] = (d2 & 0x80) ? 255 : 0;
    b[16] = (d2 & 0x04) ? 255 : 0; b[17] = (d2 & 0x08) ? 255 : 0;
    b[18] = (d2 & 0x01) ? 255 : 0; b[19] = (d2 & 0x02) ? 255 : 0;
    b[20] = 512; b[21] = 399; b[22] = 512; b[23] = 512;  // sensors at rest
    st32(mem::base, data, len);
    for (u32 i = 0; i < 64; ++i) st16(mem::base, data + 4 + 2 * i, i < len ? b[i] : 0);
    RET(CELL_OK);
}

void cellPadSetPressMode(PPUContext& c) {
    const u32 port = u32(ARG(0)), mode = u32(ARG(1));
    std::lock_guard lk(g_pad_lock);
    if (!g_pad_max) { RET(CELL_PAD_ERROR_UNINITIALIZED); return; }
    if (port >= kMaxPads) { RET(CELL_PAD_ERROR_INVALID_PARAMETER); return; }
    if (mode) g_pad_setting[port] |= CELL_PAD_SETTING_PRESS_ON; else g_pad_setting[port] &= ~CELL_PAD_SETTING_PRESS_ON;
    RET(CELL_OK);
}
void cellPadSetSensorMode(PPUContext& c) {
    const u32 port = u32(ARG(0)), mode = u32(ARG(1));
    std::lock_guard lk(g_pad_lock);
    if (!g_pad_max) { RET(CELL_PAD_ERROR_UNINITIALIZED); return; }
    if (port >= kMaxPads) { RET(CELL_PAD_ERROR_INVALID_PARAMETER); return; }
    if (mode) g_pad_setting[port] |= CELL_PAD_SETTING_SENSOR_ON; else g_pad_setting[port] &= ~CELL_PAD_SETTING_SENSOR_ON;
    RET(CELL_OK);
}
void pad_info_mode(PPUContext& c) {
    const u32 port = u32(ARG(0));
    std::lock_guard lk(g_pad_lock);
    if (!g_pad_max) { RET(CELL_PAD_ERROR_UNINITIALIZED); return; }
    if (port >= kMaxPads) { RET(CELL_PAD_ERROR_INVALID_PARAMETER); return; }
    RET(port < g_pad_max && g_pads[port].connected ? 1 : CELL_PAD_ERROR_NO_DEVICE);
}
// Rumble: CellPadActParam {u8 motor[6]}: motor[0] the small motor (on/off), motor[1] the large one
// (0-255). The window thread passes it to the host controller (pad_get_rumble).
std::atomic<u32> g_rumble{0};
void cellPadSetActDirect(PPUContext& c) {
    const u32 port = u32(ARG(0)), param = u32(ARG(1));
    std::lock_guard lk(g_pad_lock);
    if (!g_pad_max) { RET(CELL_PAD_ERROR_UNINITIALIZED); return; }
    if (port >= g_pad_max) { RET(CELL_PAD_ERROR_NO_DEVICE); return; }
    if (!param) { RET(CELL_PAD_ERROR_INVALID_PARAMETER); return; }
    if (port == 0) g_rumble = (mem::base[param] ? 0x100u : 0u) | mem::base[param + 1];
    RET(CELL_OK);
}
}  // namespace

void pad_get_rumble(u8& small, u8& large) {
    const u32 r = g_rumble;
    small = (r & 0x100) ? 255 : 0;
    large = u8(r);
}

void pad_set_state(u32 port, const PadState& s) { std::lock_guard lk(g_pad_lock); if (port < kMaxPads) g_pads[port] = s; }
PadState pad_get_state(u32 port) { std::lock_guard lk(g_pad_lock); return port < kMaxPads ? g_pads[port] : PadState{}; }

// ---- movie skipping: START or Cross ends a story movie (built/movies/*_fmv/*.bik). Menu and
// shop movies are left alone, and a button already held when the movie starts doesn't count.
namespace {
std::mutex g_movie_lock;
bool g_last_bik_is_story = false;
u32 g_skippable = 0;  // Bink handle of the story movie playing now
bool g_held = false, g_skip = false;
bool skip_buttons_down() {
    const PadState p = pad_get_state(0);
    return (p.digital1 & 0x0008) || (p.digital2 & 0x0040);  // START, Cross
}
}  // namespace

void movie_file_opened(const std::string& gp) {
    std::lock_guard lk(g_movie_lock);
    g_last_bik_is_story = gp.find("/built/movies/") != std::string::npos && gp.find("_fmv/") != std::string::npos;
}

void ppu_movie_opened(u32 hbink) {
    std::lock_guard lk(g_movie_lock);
    if (!hbink) return;
    if (!g_last_bik_is_story) { if (hbink == g_skippable) g_skippable = 0; return; }
    g_last_bik_is_story = false;
    g_skippable = hbink;
    g_held = skip_buttons_down();
    g_skip = false;
}

bool ppu_movie_skip(u32 hbink) {
    std::lock_guard lk(g_movie_lock);
    if (hbink != g_skippable || !hbink) return false;
    const bool down = skip_buttons_down();
    if (down && !g_held && !g_skip) { g_skip = true; LOG_NOTE("movie: skipped"); }
    g_held = down;
    return g_skip;
}

// =============================================================================== cellAudio
namespace {
enum : u32 {
    CELL_AUDIO_ERROR_ALREADY_INIT = 0x80310701, CELL_AUDIO_ERROR_NOT_INIT = 0x80310703,
    CELL_AUDIO_ERROR_PARAM = 0x80310704, CELL_AUDIO_ERROR_PORT_FULL = 0x80310705,
    CELL_AUDIO_ERROR_PORT_ALREADY_RUN = 0x80310706, CELL_AUDIO_ERROR_PORT_NOT_OPEN = 0x80310707,
    CELL_AUDIO_ERROR_PORT_NOT_RUN = 0x80310708, CELL_AUDIO_ERROR_PORT_OPEN = 0x8031070a,
    CELL_AUDIO_STATUS_CLOSE = 0x1010, CELL_AUDIO_STATUS_READY = 1, CELL_AUDIO_STATUS_RUN = 2,
};
constexpr u32 kPorts = 8, kSamples = 256, kMaxBlocks = 32, kMaxChannels = 8;

struct AudioPort { u32 state = CELL_AUDIO_STATUS_CLOSE; u32 channels = 0, blocks = 0, addr = 0; float level = 1.f; u64 tag = 0; };
std::mutex g_audio_lock;
bool g_audio_init = false;
u32 g_audio_mem = 0, g_audio_index = 0;  // port buffers, read-index array (u64 per port)
AudioPort g_ports[kPorts];

u32 port_size(const AudioPort& p) { return p.channels * p.blocks * kSamples * 4; }

void audio_thread() {
    using namespace std::chrono;
    const auto period = duration_cast<steady_clock::duration>(duration<double>(double(kSamples) / 48000.0));
    auto next = steady_clock::now() + period;
    std::vector<float> mix(kSamples * 2);
    for (;;) {
        std::this_thread::sleep_until(next);
        next += period;
        std::fill(mix.begin(), mix.end(), 0.f);
        {
            std::lock_guard lk(g_audio_lock);
            for (u32 n = 0; n < kPorts; ++n) {
                AudioPort& p = g_ports[n];
                if (p.state != CELL_AUDIO_STATUS_RUN) continue;
                const u32 idx_addr = g_audio_index + 8 * n;
                const u32 block = u32(ld64(mem::base, idx_addr)) % p.blocks;
                const u32 src = p.addr + block * p.channels * kSamples * 4;
                for (u32 s = 0; s < kSamples; ++s) {
                    auto smp = [&](u32 ch) { u32 w = ld32(mem::base, src + 4 * (s * p.channels + ch)); float f; std::memcpy(&f, &w, 4); return f; };
                    if (p.channels == 2) { mix[2 * s] += smp(0) * p.level; mix[2 * s + 1] += smp(1) * p.level; }
                    else {  // 7.1 (L R C LFE Ls Rs Lb Rb) down to stereo
                        const float ctr = smp(2) * 0.707f;
                        mix[2 * s] += (smp(0) + ctr + 0.5f * (smp(4) + smp(6))) * p.level;
                        mix[2 * s + 1] += (smp(1) + ctr + 0.5f * (smp(5) + smp(7))) * p.level;
                    }
                }
                std::memset(mem::base + src, 0, p.channels * kSamples * 4);
                st64(mem::base, idx_addr, (block + 1) % p.blocks);
                ++p.tag;
            }
        }
        if (audio_sink) audio_sink(mix.data(), kSamples);
        // TOD_AUDIO_DUMP=file: raw 48 kHz stereo float32 of everything mixed (for headless tests)
        static FILE* dump = [] { const char* e = std::getenv("TOD_AUDIO_DUMP"); return e ? std::fopen(e, "wb") : nullptr; }();
        if (dump) { std::fwrite(mix.data(), sizeof(float), mix.size(), dump); std::fflush(dump); }
    }
}

void cellAudioInit(PPUContext& c) {
    std::lock_guard lk(g_audio_lock);
    if (g_audio_init) { RET(CELL_AUDIO_ERROR_ALREADY_INIT); return; }
    if (!g_audio_mem) {
        g_audio_mem = mem::alloc_main(kPorts * kMaxBlocks * kMaxChannels * kSamples * 4);
        g_audio_index = mem::alloc_main(kPorts * 8, 0x80);
        std::thread(audio_thread).detach();
    }
    for (auto& p : g_ports) p = AudioPort{};
    g_audio_init = true;
    LOG_NOTE("cellAudioInit");
    RET(CELL_OK);
}
void cellAudioQuit(PPUContext& c) {
    std::lock_guard lk(g_audio_lock);
    if (!g_audio_init) { RET(CELL_AUDIO_ERROR_NOT_INIT); return; }
    for (auto& p : g_ports) p = AudioPort{};
    g_audio_init = false;
    RET(CELL_OK);
}
void cellAudioPortOpen(PPUContext& c) {
    const u32 param = u32(ARG(0)), out = u32(ARG(1));
    std::lock_guard lk(g_audio_lock);
    if (!g_audio_init) { RET(CELL_AUDIO_ERROR_NOT_INIT); return; }
    if (!param || !out) { RET(CELL_AUDIO_ERROR_PARAM); return; }
    u64 ch = ld64(mem::base, param), blocks = ld64(mem::base, param + 8), attr = ld64(mem::base, param + 16);
    const u32 lw = ld32(mem::base, param + 24);
    float level; std::memcpy(&level, &lw, 4);
    if (ch == 0) ch = 2;
    if ((ch != 2 && ch != 8) || (blocks != 2 && blocks != 4 && blocks != 8 && blocks != 16 && blocks != 32)) { RET(CELL_AUDIO_ERROR_PARAM); return; }
    for (u32 n = 0; n < kPorts; ++n) {
        AudioPort& p = g_ports[n];
        if (p.state != CELL_AUDIO_STATUS_CLOSE) continue;
        p.state = CELL_AUDIO_STATUS_READY;
        p.channels = u32(ch); p.blocks = u32(blocks);
        p.addr = g_audio_mem + n * kMaxBlocks * kMaxChannels * kSamples * 4;
        p.level = (attr & 0x1000) ? level : 1.f;  // CELL_AUDIO_PORTATTR_INITLEVEL
        std::memset(mem::base + p.addr, 0, port_size(p));
        st64(mem::base, g_audio_index + 8 * n, 0);
        st32(mem::base, out, n);
        LOG_NOTE("cellAudioPortOpen -> port %u (%llu ch, %llu blocks, attr 0x%llx)", n, (unsigned long long)ch,
                 (unsigned long long)blocks, (unsigned long long)attr);
        RET(CELL_OK);
        return;
    }
    RET(CELL_AUDIO_ERROR_PORT_FULL);
}
void cellAudioGetPortConfig(PPUContext& c) {
    const u32 n = u32(ARG(0)), cfg = u32(ARG(1));
    std::lock_guard lk(g_audio_lock);
    if (!g_audio_init) { RET(CELL_AUDIO_ERROR_NOT_INIT); return; }
    if (!cfg || n >= kPorts) { RET(CELL_AUDIO_ERROR_PARAM); return; }
    const AudioPort& p = g_ports[n];
    st32(mem::base, cfg + 0, g_audio_index + 8 * n);
    st32(mem::base, cfg + 4, p.state);
    st64(mem::base, cfg + 8, p.channels);
    st64(mem::base, cfg + 16, p.blocks);
    st32(mem::base, cfg + 24, port_size(p));
    st32(mem::base, cfg + 28, p.addr);
    RET(CELL_OK);
}
void cellAudioPortStart(PPUContext& c) {
    const u32 n = u32(ARG(0));
    std::lock_guard lk(g_audio_lock);
    if (!g_audio_init) { RET(CELL_AUDIO_ERROR_NOT_INIT); return; }
    if (n >= kPorts) { RET(CELL_AUDIO_ERROR_PARAM); return; }
    AudioPort& p = g_ports[n];
    if (p.state == CELL_AUDIO_STATUS_CLOSE) { RET(CELL_AUDIO_ERROR_PORT_NOT_OPEN); return; }
    if (p.state == CELL_AUDIO_STATUS_RUN) { RET(CELL_AUDIO_ERROR_PORT_ALREADY_RUN); return; }
    p.state = CELL_AUDIO_STATUS_RUN;
    LOG_NOTE("cellAudioPortStart(%u): buffer 0x%x, read index at 0x%x", n, p.addr, g_audio_index + 8 * n);
    RET(CELL_OK);
}
void cellAudioPortStop(PPUContext& c) {
    const u32 n = u32(ARG(0));
    std::lock_guard lk(g_audio_lock);
    if (!g_audio_init) { RET(CELL_AUDIO_ERROR_NOT_INIT); return; }
    if (n >= kPorts) { RET(CELL_AUDIO_ERROR_PARAM); return; }
    AudioPort& p = g_ports[n];
    if (p.state == CELL_AUDIO_STATUS_CLOSE) { RET(CELL_AUDIO_ERROR_PORT_NOT_OPEN); return; }
    if (p.state != CELL_AUDIO_STATUS_RUN) { RET(CELL_AUDIO_ERROR_PORT_NOT_RUN); return; }
    p.state = CELL_AUDIO_STATUS_READY;
    RET(CELL_OK);
}
void cellAudioPortClose(PPUContext& c) {
    const u32 n = u32(ARG(0));
    std::lock_guard lk(g_audio_lock);
    if (!g_audio_init) { RET(CELL_AUDIO_ERROR_NOT_INIT); return; }
    if (n >= kPorts) { RET(CELL_AUDIO_ERROR_PARAM); return; }
    if (g_ports[n].state == CELL_AUDIO_STATUS_CLOSE) { RET(CELL_AUDIO_ERROR_PORT_NOT_OPEN); return; }
    g_ports[n] = AudioPort{};
    RET(CELL_OK);
}
}  // namespace

// ============================================================================ cellGameData
namespace {
enum : u32 {
    CELL_GAMEDATA_ERROR_CBRESULT = 0x8002b601, CELL_GAMEDATA_ERROR_ACCESS_ERROR = 0x8002b602,
    CELL_GAMEDATA_ERROR_PARAM = 0x8002b604, CELL_GAMEDATA_ERROR_BROKEN = 0x8002b606,
};
// CellGameDataStatGet field offsets
constexpr u32 kGetSize = 0x1448, kGetParam = 0x860, kParamSize = 0xb9c;

// Minimal PARAM.SFO reader/writer (string and integer entries).
using Sfo = std::map<std::string, std::pair<bool, std::string>>;  // key -> (is_int, value or 4 bytes LE)
bool sfo_load(const std::string& path, Sfo& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<u8> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 20 || std::memcmp(d.data(), "\0PSF", 4)) return false;
    auto le32 = [&](size_t o) { return u32(d[o]) | u32(d[o + 1]) << 8 | u32(d[o + 2]) << 16 | u32(d[o + 3]) << 24; };
    auto le16 = [&](size_t o) { return u32(d[o]) | u32(d[o + 1]) << 8; };
    const u32 keys = le32(8), data = le32(12), n = le32(16);
    for (u32 i = 0; i < n; ++i) {
        const size_t e = 20 + 16 * i;
        const std::string key(reinterpret_cast<const char*>(&d[keys + le16(e)]));
        const u32 fmt = le16(e + 2), len = le32(e + 4), off = le32(e + 12);
        if (fmt == 0x0404) out[key] = {true, std::string(reinterpret_cast<const char*>(&d[data + off]), 4)};
        else out[key] = {false, std::string(reinterpret_cast<const char*>(&d[data + off]), len ? len - 1 : 0)};
    }
    return true;
}
void sfo_save(const std::string& path, const Sfo& s) {
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
}
std::string sfo_str(const Sfo& s, const std::string& k) { auto it = s.find(k); return it == s.end() || it->second.first ? "" : it->second.second; }
void put_str(u32 ea, const std::string& v, size_t max) { std::memset(mem::base + ea, 0, max); std::memcpy(mem::base + ea, v.data(), std::min(v.size(), max - 1)); }

void cellGameDataCheckCreate2(PPUContext& c) {
    const u32 version = u32(ARG(0)), dir_ea = u32(ARG(1)), func = u32(ARG(3));
    const std::string dir_name = read_cstr(dir_ea, 32);
    LOG_NOTE("cellGameDataCheckCreate(version=%u, dir=%s)", version, dir_name.c_str());
    if (version != 0 || !func || dir_name.empty()) { RET(CELL_GAMEDATA_ERROR_PARAM); return; }
    const std::string dir = "/dev_hdd0/game/" + dir_name;
    Sfo sfo;
    const bool exists = sfo_load(vfs_host_path(dir + "/PARAM.SFO"), sfo);
    if (exists && sfo_str(sfo, "CATEGORY") != "GD" && sfo_str(sfo, "CATEGORY") != "DG") { RET(CELL_GAMEDATA_ERROR_BROKEN); return; }

    static const u32 area = mem::alloc_main(0x2000, 0x100);
    const u32 result = area, set = area + 0x20, get = area + 0x100;
    std::memset(mem::base + area, 0, 0x100 + kGetSize);
    st32(mem::base, get + 0, 40 * 1024 * 1024 - 256);
    st32(mem::base, get + 4, exists ? 0 : 1);
    put_str(get + 0x8, dir, 1055);
    put_str(get + 0x427, dir + "/USRDIR", 1055);
    const u32 gp = get + kGetParam;
    put_str(gp + 0, sfo_str(sfo, "TITLE"), 128);
    put_str(gp + 0xa80, sfo_str(sfo, "TITLE_ID"), 10);
    put_str(gp + 0xa8c, sfo_str(sfo, "VERSION"), 6);
    st32(mem::base, gp + kParamSize, ~0u);  // sizeKB: not calculated
    st32(mem::base, get + kGetParam + kParamSize, ~0u);

    PPUContext saved = c;
    guest_call(c, func, {result, get, set});
    c = saved;

    const s32 res = s32(ld32(mem::base, result));
    const u32 set_param = ld32(mem::base, set);
    if (res == 1) { RET(CELL_OK); return; }   // OK_CANCEL
    if (res != 0) { LOG_ERR("cellGameDataCheckCreate: callback result %d", res); RET(CELL_GAMEDATA_ERROR_CBRESULT); return; }
    if (!exists && !set_param) { RET(CELL_GAMEDATA_ERROR_PARAM); return; }
    std::error_code ec;
    std::filesystem::create_directories(vfs_host_path(dir + "/USRDIR"), ec);
    if (ec) { RET(CELL_GAMEDATA_ERROR_ACCESS_ERROR); return; }
    if (set_param) {
        if (!exists) sfo["CATEGORY"] = {false, "GD"};
        sfo["TITLE"] = {false, read_cstr(set_param, 128)};
        sfo["TITLE_ID"] = {false, read_cstr(set_param + 0xa80, 10)};
        sfo["VERSION"] = {false, read_cstr(set_param + 0xa8c, 6)};
        const u32 pl = ld32(mem::base, set_param + 0xa94);
        sfo["PARENTAL_LEVEL"] = {true, std::string{char(pl), char(pl >> 8), char(pl >> 16), char(pl >> 24)}};
        sfo_save(vfs_host_path(dir + "/PARAM.SFO"), sfo);
    }
    RET(CELL_OK);
}
}  // namespace

void register_io_modules() {
    register_import(0x1cf98800, "cellPadInit", cellPadInit);
    register_import(0x4d9b75d5, "cellPadEnd", cellPadEnd);
    register_import(0x8b72cda1, "cellPadGetData", cellPadGetData);
    register_import(0x3aaad464, "cellPadGetInfo", cellPadGetInfo);
    register_import(0xf65544ee, "cellPadSetActDirect", cellPadSetActDirect);
    register_import(0xf83f8182, "cellPadSetPressMode", cellPadSetPressMode);
    register_import(0xbe5be3ba, "cellPadSetSensorMode", cellPadSetSensorMode);
    register_import(0x0e2dfaad, "cellPadInfoPressMode", pad_info_mode);
    register_import(0x78200559, "cellPadInfoSensorMode", pad_info_mode);
    register_import(0x0b168f92, "cellAudioInit", cellAudioInit);
    register_import(0xca5ac370, "cellAudioQuit", cellAudioQuit);
    register_import(0xcd7bc431, "cellAudioPortOpen", cellAudioPortOpen);
    register_import(0x74a66af0, "cellAudioGetPortConfig", cellAudioGetPortConfig);
    register_import(0x89be28f2, "cellAudioPortStart", cellAudioPortStart);
    register_import(0x5b1e2c73, "cellAudioPortStop", cellAudioPortStop);
    register_import(0x4129fe2d, "cellAudioPortClose", cellAudioPortClose);
    register_import(0xe7951dee, "cellGameDataCheckCreate", cellGameDataCheckCreate2);
    register_import(0xc9645c41, "cellGameDataCheckCreate2", cellGameDataCheckCreate2);
}
