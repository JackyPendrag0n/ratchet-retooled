// Runs recompiled SPU blocks (gen_spu/, from spu_codegen.py). The code in local store is matched
// against the blocks by its bytes, so it does not matter where a program was loaded. Each SPU
// keeps a cache from instruction slot to block, revalidated when that part of local store has
// been written since; anything without a block (generated stubs, patched code, channel and stop
// instructions) runs in the interpreter one block at a time.
#include "guest.h"
#include "spu_core.h"

#ifdef TOD_SPU_RECOMP
#include "spu_blocks.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

struct CodeCacheEntry {
    SpuBlockFn fn;      // nullptr: no block here (interpret)
    u32 gen_first, gen_last;
    u32 info;           // index into g_spu_blocks
    u8 last_page;       // local store page of the block's last instruction, so the hot path
    bool valid;         // never has to touch g_spu_blocks
};

namespace {
constexpr int kKeyWords = 4;

struct Index {
    // key: first min(len, 4) words; separate maps per key length
    std::unordered_multimap<u64, u32> by_key[kKeyWords + 1];
    Index() {
        for (u32 i = 0; i < g_spu_block_count; ++i) {
            const SpuBlockInfo& b = g_spu_blocks[i];
            const u32 kw = std::min<u32>(b.len / 4, kKeyWords);
            by_key[kw].emplace(hash(g_spu_block_bytes + b.offset, kw * 4), i);
        }
    }
    static u64 hash(const u8* p, u32 n) {
        u64 h = 0xcbf29ce484222325ull;
        for (u32 i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
        return h;
    }
    // Longest block whose bytes match local store at pc.
    int find(const SPUThread& s, u32 pc) const {
        int best = -1;
        u32 best_len = 0;
        const u8* code = s.ls + pc;
        const u32 avail = kLsSize - pc;
        for (int kw = kKeyWords; kw >= 1; --kw) {
            if (u32(kw * 4) > avail) continue;
            auto [lo, hi] = by_key[kw].equal_range(hash(code, kw * 4));
            for (auto it = lo; it != hi; ++it) {
                const SpuBlockInfo& b = g_spu_blocks[it->second];
                if (b.len > avail || b.len <= best_len) continue;
                if (std::memcmp(code, g_spu_block_bytes + b.offset, b.len) == 0) { best = int(it->second); best_len = b.len; }
            }
            if (best >= 0) break;  // a longer key beats any shorter block
        }
        return best;
    }
};
const Index& index() { static const Index idx; return idx; }

// TOD_SPU_PROFILE=1: every 10 s, per SPU, instructions interpreted vs blocks run, and the
// interpreted spots that ran most (SPU index, pc, first instruction word there).
struct Profile {
    std::mutex m;
    std::map<std::tuple<int, u32, u32>, u64> interp;  // (spu, pc, word) -> times entered
    std::vector<SPUThread*> spus;
    std::set<std::string> entries;  // 32 bytes of code at each interpreted entry, hex
};
Profile* profile() {
    static Profile* p = [] () -> Profile* {
        if (!std::getenv("TOD_SPU_PROFILE")) return nullptr;
        auto* pr = new Profile;
        std::thread([pr] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                std::lock_guard lk(pr->m);
                u64 ti = 0, tb = 0;
                for (SPUThread* t : pr->spus) {
                    LOG_NOTE("spu profile: SPU%d %llu interpreted instructions, %llu blocks (%llu instructions)", t->index,
                             (unsigned long long)t->insn_count, (unsigned long long)t->block_count, (unsigned long long)t->block_insns);
                    ti += t->insn_count; tb += t->block_insns;
                }
                if (ti + tb) LOG_NOTE("spu profile: %.3f%% of SPU instructions ran as recompiled code", 100.0 * double(tb) / double(ti + tb));
                std::vector<std::pair<u64, std::tuple<int, u32, u32>>> top;
                for (auto& [k, v] : pr->interp) top.push_back({v, k});
                std::sort(top.rbegin(), top.rend());
                for (size_t i = 0; i < top.size() && i < 25; ++i)
                    LOG_NOTE("spu profile:   SPU%d pc 0x%05x word %08x: %llu entries", std::get<0>(top[i].second),
                             std::get<1>(top[i].second), std::get<2>(top[i].second), (unsigned long long)top[i].first);
                pr->interp.clear();
                // the code at every interpreted entry, for spu_codegen.py --entries
                if (const char* path = std::getenv("TOD_SPU_ENTRIES")) {
                    if (FILE* f = std::fopen(path, "w")) {
                        for (auto& e : pr->entries) std::fprintf(f, "%s\n", e.c_str());
                        std::fclose(f);
                    }
                }
            }
        }).detach();
        return pr;
    }();
    return p;
}

bool recomp_enabled() {
    static const bool on = !std::getenv("TOD_SPU_INTERP");
    return on;
}
}  // namespace

void spu_run(SPUThread& s) {
    if (!recomp_enabled()) { spu_interpret(s); return; }
    if (!s.code_cache) s.code_cache = static_cast<CodeCacheEntry*>(std::calloc(kLsSize / 4, sizeof(CodeCacheEntry)));
    const Index& idx = index();
    Profile* prof = profile();
    if (prof) { std::lock_guard lk(prof->m); if (std::find(prof->spus.begin(), prof->spus.end(), &s) == prof->spus.end()) prof->spus.push_back(&s); }
    for (;;) {
        if (s.stop_request.load(std::memory_order_relaxed)) return;
        const u32 pc = s.pc & 0x3fffc;
        CodeCacheEntry& e = s.code_cache[pc >> 2];
        u32 g0 = s.page_gen[pc >> 10];
        if (!e.valid || e.gen_first != g0 || e.gen_last != s.page_gen[e.last_page]) {
            // (Re)validate: same block still in place, or look it up again.
            bool same = false;
            if (e.valid && e.fn) {
                const SpuBlockInfo& b = g_spu_blocks[e.info];
                same = pc + b.len <= kLsSize && std::memcmp(s.ls + pc, g_spu_block_bytes + b.offset, b.len) == 0;
            }
            if (!same) {
                const int i = idx.find(s, pc);
                e.fn = i >= 0 ? g_spu_blocks[i].fn : nullptr;
                e.info = i >= 0 ? u32(i) : 0;
            }
            e.valid = true;
            e.gen_first = g0;
            e.last_page = u8((e.fn ? pc + g_spu_blocks[e.info].len - 1 : pc) >> 10);
            e.gen_last = s.page_gen[e.last_page];
        }
        if (e.fn) {
            ++s.block_count;
            if (prof) s.block_insns += g_spu_blocks[e.info].len / 4;
            s.pc = e.fn(s, pc);
            if (s.pc & 1) {  // a channel write with interrupts enabled: interpret it
                s.pc &= ~3u;
                if (!spu_interpret(s, true)) return;
            }
        } else if (prof && [&] { std::lock_guard lk(prof->m); ++prof->interp[{s.index, pc, ls_read32(s, pc)}];
                    if (pc + 32 <= kLsSize) {
                        char hex[65];
                        for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", s.ls[pc + i]);
                        prof->entries.insert(hex);
                    }
                    return true; }() && !spu_interpret(s, true)) {
            return;
        } else if (!prof && !spu_interpret(s, true)) {
            return;
        }
    }
}
#else
void spu_run(SPUThread& s) { spu_interpret(s); }
#endif
