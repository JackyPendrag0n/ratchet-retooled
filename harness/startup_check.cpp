// Startup cross-check: runs the recompiled EBOOT from its entry point under the
// same synthetic launch fixture and HLE models as checkpoint 004, stops at the
// probe address, and prints state in the same form (incl. the same checksum).
#include "ppu.h"
#include "ppu_decls.h"
#include "ppu_tables.h"

#include <sys/mman.h>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
u8* g_base = nullptr;
struct Region { u32 base, size; };
std::vector<Region> g_regions;  // in checkpoint-004 creation order (affects checksum)

void map_region(u32 addr, u32 size, u8 fill = 0) {
    const u32 lo = addr & ~0xfffu, hi = (addr + size + 0xfff) & ~0xfffu;
    if (mprotect(g_base + lo, hi - lo, PROT_READ | PROT_WRITE)) throw std::runtime_error("mprotect");
    std::fill(g_base + addr, g_base + addr + size, fill);
    g_regions.push_back({addr, size});
}
bool mapped(u64 addr, u64 len) {
    if (addr == 0 || addr > 0xffffffffull) return false;
    for (auto& r : g_regions)
        if (addr >= r.base && addr - r.base <= r.size && len <= r.size - (addr - r.base)) return true;
    return false;
}

struct Event { u32 nid, stub; u64 lr, sp; u64 args[8]; u64 result; };
std::vector<Event> g_events;
const PPUImportEntry* g_import_by_index[256];

// HLE state matching checkpoint 004's diagnostic models.
struct {
    bool tls_init = false;
    u32 tmpl = 0, filesz = 0, memsz = 0, stride = 0, used = 0;
    std::map<u32, u32> mutexes;
    u32 next_mutex = 0x95000100;
    u32 atexitspawn = 0, at_exitspawn = 0;
    u64 clock_reads = 0;
} hle;

u32 tls_alloc() {
    const u32 arena = 0x20000000, block = arena + hle.used * hle.stride;
    ++hle.used;
    std::fill(g_base + block, g_base + block + 0x30, 0);
    std::copy(g_base + hle.tmpl, g_base + hle.tmpl + hle.filesz, g_base + block + 0x30);
    std::fill(g_base + block + 0x30 + hle.filesz, g_base + block + 0x30 + hle.memsz, 0);
    return block + 0x7030;
}

struct Stop { u32 addr; };
}  // namespace

void ppu_import(PPUContext& c, u8* base, u32 index) {
    const PPUImportEntry& imp = *g_import_by_index[index];
    Event e{imp.nid, imp.stub, c.lr, c.r[1], {}, 0};
    for (int i = 0; i < 8; ++i) e.args[i] = c.r[3 + i];
    switch (imp.nid) {
    case 0x744680a2:  // sys_initialize_tls
        if (c.r[13] == 0) {
            hle.tmpl = u32(c.r[4]); hle.filesz = u32(c.r[5]); hle.memsz = u32(c.r[6]);
            hle.stride = (hle.memsz + 0x30 + 15) & ~15u;
            map_region(0x20000000, 0x40000, 0xcc);
            hle.tls_init = true;
            c.r[13] = tls_alloc();
        }
        break;
    case 0x2f85c0ef: {  // sys_lwmutex_create
        const u64 m = c.r[3], attr = c.r[4];
        if (!mapped(m, 24) || !mapped(attr, 16)) { c.r[3] = 0x8001000d; break; }
        const u32 protocol = ld32(base, attr), recursive = ld32(base, attr + 4);
        if ((recursive != 0x10 && recursive != 0x20) || (protocol != 1 && protocol != 2 && protocol != 4)) {
            c.r[3] = 0x80010002; break;
        }
        const u32 id = hle.next_mutex; hle.next_mutex += 0x100;
        hle.mutexes[id] = u32(m);
        st32(base, m, 0xffffffff); st32(base, m + 4, 0); st32(base, m + 8, protocol | recursive);
        st32(base, m + 12, 0); st32(base, m + 16, id);
        c.r[3] = 0;
        break;
    }
    case 0x8461e528: c.r[3] = 1000000; ++hle.clock_reads; break;  // sys_time_get_system_time (frozen)
    case 0x2c847572: if (!hle.atexitspawn) hle.atexitspawn = u32(c.r[3]); break;
    case 0x96328741: if (!hle.at_exitspawn) hle.at_exitspawn = u32(c.r[3]); break;
    default: {
        char b[160];
        std::snprintf(b, sizeof b, "unimplemented import %s 0x%08X (stub 0x%X) called from 0x%llX",
                      imp.library, imp.nid, imp.stub, (unsigned long long)c.lr);
        throw std::runtime_error(b);
    }
    }
    e.result = c.r[3];
    g_events.push_back(e);
}

void ppu_syscall(PPUContext& c, u8*, u32 addr) {
    if (getenv("TOD_EXPLORE") && c.r[11] == 147) { c.r[3] = 79800000; return; }  // sys_time_get_timebase_frequency
    char b[96];
    std::snprintf(b, sizeof b, "unimplemented syscall %llu at 0x%X", (unsigned long long)c.r[11], addr);
    throw std::runtime_error(b);
}

void ppu_probe(PPUContext&, u8*, u32 addr) { throw Stop{addr}; }

static u64 checksum(const PPUContext& c) {
    u64 s = 14695981039346656037ull;
    auto byte = [&](u8 v) { s ^= v; s *= 1099511628211ull; };
    auto word = [&](u64 v) { for (int k = 56; k >= 0; k -= 8) byte(u8(v >> k)); };
    for (u64 v : c.r) word(v);
    word(c.lr); word(c.ctr); word(cr_get(c)); word(u32(xer_get(c)));
    for (auto& r : g_regions) {
        word(r.base); word(r.size);
        for (u32 i = 0; i < r.size; ++i) byte(g_base[r.base + i]);
    }
    return s;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: startup_check EBOOT.elf [empty|one|many] [--trace FILE]\n"; return 2; }
    std::string fixture = argc > 2 ? argv[2] : "one", trace;
    for (int i = 3; i + 1 < argc; ++i) if (std::string(argv[i]) == "--trace") trace = argv[i + 1];
    try {
        g_base = static_cast<u8*>(mmap(nullptr, 0x100000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
        if (g_base == MAP_FAILED) throw std::runtime_error("cannot reserve 4 GiB guest space");
        std::ifstream f(argv[1], std::ios::binary);
        std::vector<u8> elf((std::istreambuf_iterator<char>(f)), {});
        if (elf.size() != 10923400) throw std::runtime_error("unexpected EBOOT.elf size");
        auto be = [&](size_t o, int n) { u64 v = 0; for (int i = 0; i < n; ++i) v = v << 8 | elf[o + i]; return v; };
        u32 tls_addr = 0, tls_filesz = 0, tls_memsz = 0;
        for (int i = 0; i < 8; ++i) {
            const size_t o = 64 + 56 * i;
            const u32 type = be(o, 4);
            const u64 off = be(o + 8, 8), va = be(o + 16, 8), fs = be(o + 32, 8), ms = be(o + 40, 8);
            if (type == 1 && ms) { map_region(u32(va), u32(ms)); std::copy(&elf[off], &elf[off] + fs, g_base + va); }
            if (type == 7) { tls_addr = u32(va); tls_filesz = u32(fs); tls_memsz = u32(ms); }
        }
        map_region(0xd0000000, 1 << 20);  // stack

        PPUContext c{};
        std::vector<std::string> args, env;
        if (fixture == "one") args = {"/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN"};
        else if (fixture == "many") { args = {"/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN", "fixture", "three"}; env = {"TOD_TEST=1", "LANG=en_GB.UTF-8"}; }
        else if (fixture != "empty") throw std::runtime_error("fixture must be empty, one or many");
        map_region(0x21000000, 0x10000);
        u32 text = 0x21001000;
        auto table = [&](u32 b, const std::vector<std::string>& v) {
            for (size_t i = 0; i < v.size(); ++i) {
                st64(g_base, b + 8 * i, text);
                for (char ch : v[i]) st8(g_base, text++, u8(ch));
                st8(g_base, text++, 0);
            }
            st64(g_base, b + 8 * v.size(), 0);
        };
        table(0x21000000, args); table(0x21000100, env);
        c.r[1] = 0xd00fff00; c.r[3] = args.size(); c.r[4] = 0x21000000; c.r[5] = 0x21000100; c.r[6] = env.size();
        c.r[7] = 1; c.r[8] = tls_addr; c.r[9] = tls_filesz; c.r[10] = tls_memsz;
        map_region(0x20100000, 0x1000);
        st32(g_base, 0x20100000, 0xf0000000); st32(g_base, 0x20100004, 0); st32(g_base, 0x856c64, 0x20100000);
        // Import descriptors laid out exactly as checkpoint 004 does (tls first, then by stub).
        std::vector<const PPUImportEntry*> order;
        for (unsigned i = 0; i < ppu_import_count; ++i) { order.push_back(&ppu_import_table[i]); g_import_by_index[ppu_import_table[i].index] = &ppu_import_table[i]; }
        std::stable_sort(order.begin(), order.end(), [](auto* a, auto* b) {
            return std::make_pair(a->nid != 0x744680a2, a->stub) < std::make_pair(b->nid != 0x744680a2, b->stub); });
        for (size_t i = 0; i < order.size(); ++i) {
            const u32 d = 0x20100000 + u32(i) * 8;
            st32(g_base, order[i]->slot, d); st32(g_base, d, 0xf0000000 + 4 * u32(i)); st32(g_base, d + 4, 0);
        }

        // Entry descriptor 0x859418 -> code, TOC.
        const u32 entry = ld32(g_base, 0x859418);
        c.r[2] = ld32(g_base, 0x859418 + 4);
        const PPUFunc fn = ppu_lookup(entry);
        if (!fn) throw std::runtime_error("entry not recompiled");
        u32 stop = 0;
        std::string reason = "returned";
        try { fn(c, g_base); } catch (const Stop& s) { stop = s.addr; reason = "probe"; }
        catch (const std::exception& e) { reason = e.what(); }
        std::printf("RECOMP STARTUP: fixture=%s argc=%zu envc=%zu\n", fixture.c_str(), args.size(), env.size());
        std::printf("stop_address=0x%x reason=%s\n", stop, reason.c_str());
        std::printf("mutexes_created=%zu clock_reads=%llu hle_calls=%zu\n", hle.mutexes.size(), (unsigned long long)hle.clock_reads, g_events.size());
        std::printf("r13=0x%llx TOC=0x%llx SP=0x%llx LR=0x%llx\n", (unsigned long long)c.r[13], (unsigned long long)c.r[2],
                    (unsigned long long)c.r[1], (unsigned long long)c.lr);
        std::printf("r3=0x%llx r4=0x%llx\n", (unsigned long long)c.r[3], (unsigned long long)c.r[4]);
        std::printf("state_checksum=0x%llx\n", (unsigned long long)checksum(c));
        if (getenv("TOD_DUMP")) {  // state-dump (diff aid)
            std::ofstream d("rec-state-" + fixture + ".txt");
            for (int i = 0; i < 32; ++i) d << "r" << i << " " << std::hex << c.r[i] << "\n";
            d << "lr " << c.lr << "\nctr " << c.ctr << "\ncr " << cr_get(c) << "\nxer " << u32(xer_get(c)) << "\n";
            std::ofstream mm("rec-mem-" + fixture + ".bin", std::ios::binary);
            for (auto& r : g_regions) { mm.write((char*)&r.base, 4); mm.write((char*)&r.size, 4); mm.write((char*)g_base + r.base, r.size); }
        }
        if (!trace.empty()) {
            std::ofstream t(trace);
            t << "[\n";
            for (size_t i = 0; i < g_events.size(); ++i) {
                auto& e = g_events[i];
                char b[512];
                int n = std::snprintf(b, sizeof b, "  {\"nid\":\"0x%x\",\"stub\":\"0x%x\",\"lr\":\"0x%llx\",\"sp\":\"0x%llx\",\"args\":[",
                                      e.nid, e.stub, (unsigned long long)e.lr, (unsigned long long)e.sp);
                for (int k = 0; k < 8; ++k) n += std::snprintf(b + n, sizeof b - n, "%s\"0x%llx\"", k ? "," : "", (unsigned long long)e.args[k]);
                std::snprintf(b + n, sizeof b - n, "],\"r3_after\":\"0x%llx\"}%s\n", (unsigned long long)e.result, i + 1 < g_events.size() ? "," : "");
                t << b;
            }
            t << "]\n";
        }
        return reason == "probe" ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
