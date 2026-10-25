// SPU interpreter. It is the reference for the recompiled SPU code and runs any code the
// recompiler has not seen (self-modifying stubs, relocated or generated code).
#include "guest.h"
#include "spu_ops.h"

#include <array>

namespace {
enum Op : u8 {
    OP_INVALID,
#define X(name) OP_##name,
#include "spu_opcodes.inc"
#undef X
    OP_COUNT
};

struct OpInfo { const char* name; u8 width; u16 value; };
constexpr OpInfo kOps[] = {
    {"invalid", 0, 0},
#define X(name) {#name, 0, 0},
#include "spu_opcodes.inc"
#undef X
};

struct Encoding { Op op; u8 width; u16 value; };
// Opcode values: width in bits and the value of those leading bits.
constexpr Encoding kEnc[] = {
    {OP_selb, 4, 0x8}, {OP_shufb, 4, 0xB}, {OP_mpya, 4, 0xC}, {OP_fnms, 4, 0xD}, {OP_fma, 4, 0xE}, {OP_fms, 4, 0xF},
    {OP_ila, 7, 0x21}, {OP_hbra, 7, 0x08}, {OP_hbrr, 7, 0x09},
    {OP_mpyi, 8, 0x74}, {OP_mpyui, 8, 0x75}, {OP_cgti, 8, 0x4C}, {OP_cgthi, 8, 0x4D}, {OP_cgtbi, 8, 0x4E},
    {OP_clgti, 8, 0x5C}, {OP_clgthi, 8, 0x5D}, {OP_clgtbi, 8, 0x5E}, {OP_ceqi, 8, 0x7C}, {OP_ceqhi, 8, 0x7D},
    {OP_ceqbi, 8, 0x7E}, {OP_heqi, 8, 0x7F}, {OP_hgti, 8, 0x4F}, {OP_hlgti, 8, 0x5F}, {OP_andi, 8, 0x14},
    {OP_andhi, 8, 0x15}, {OP_andbi, 8, 0x16}, {OP_ori, 8, 0x04}, {OP_orhi, 8, 0x05}, {OP_orbi, 8, 0x06},
    {OP_xori, 8, 0x44}, {OP_xorhi, 8, 0x45}, {OP_xorbi, 8, 0x46}, {OP_ai, 8, 0x1C}, {OP_ahi, 8, 0x1D},
    {OP_sfi, 8, 0x0C}, {OP_sfhi, 8, 0x0D}, {OP_lqd, 8, 0x34}, {OP_stqd, 8, 0x24},
    {OP_br, 9, 0x064}, {OP_bra, 9, 0x060}, {OP_brsl, 9, 0x066}, {OP_brasl, 9, 0x062}, {OP_brnz, 9, 0x042},
    {OP_brz, 9, 0x040}, {OP_brhnz, 9, 0x046}, {OP_brhz, 9, 0x044}, {OP_lqa, 9, 0x061}, {OP_lqr, 9, 0x067},
    {OP_stqa, 9, 0x041}, {OP_stqr, 9, 0x047}, {OP_ilh, 9, 0x083}, {OP_ilhu, 9, 0x082}, {OP_il, 9, 0x081},
    {OP_iohl, 9, 0x0C1}, {OP_fsmbi, 9, 0x065},
    {OP_cflts, 10, 0x1D8}, {OP_cfltu, 10, 0x1D9}, {OP_csflt, 10, 0x1DA}, {OP_cuflt, 10, 0x1DB},
    {OP_stop, 11, 0x000}, {OP_lnop, 11, 0x001}, {OP_sync, 11, 0x002}, {OP_dsync, 11, 0x003}, {OP_mfspr, 11, 0x00C},
    {OP_rdch, 11, 0x00D}, {OP_rchcnt, 11, 0x00F}, {OP_sf, 11, 0x040}, {OP_or, 11, 0x041}, {OP_bg, 11, 0x042},
    {OP_sfh, 11, 0x048}, {OP_nor, 11, 0x049}, {OP_absdb, 11, 0x053}, {OP_rot, 11, 0x058}, {OP_rotm, 11, 0x059},
    {OP_rotma, 11, 0x05A}, {OP_shl, 11, 0x05B}, {OP_roth, 11, 0x05C}, {OP_rothm, 11, 0x05D}, {OP_rotmah, 11, 0x05E},
    {OP_shlh, 11, 0x05F}, {OP_roti, 11, 0x078}, {OP_rotmi, 11, 0x079}, {OP_rotmai, 11, 0x07A}, {OP_shli, 11, 0x07B},
    {OP_rothi, 11, 0x07C}, {OP_rothmi, 11, 0x07D}, {OP_rotmahi, 11, 0x07E}, {OP_shlhi, 11, 0x07F}, {OP_a, 11, 0x0C0},
    {OP_and, 11, 0x0C1}, {OP_cg, 11, 0x0C2}, {OP_ah, 11, 0x0C8}, {OP_nand, 11, 0x0C9}, {OP_avgb, 11, 0x0D3},
    {OP_mtspr, 11, 0x10C}, {OP_wrch, 11, 0x10D}, {OP_biz, 11, 0x128}, {OP_binz, 11, 0x129}, {OP_bihz, 11, 0x12A},
    {OP_bihnz, 11, 0x12B}, {OP_stopd, 11, 0x140}, {OP_stqx, 11, 0x144}, {OP_bi, 11, 0x1A8}, {OP_bisl, 11, 0x1A9},
    {OP_iret, 11, 0x1AA}, {OP_bisled, 11, 0x1AB}, {OP_hbr, 11, 0x1AC}, {OP_gb, 11, 0x1B0}, {OP_gbh, 11, 0x1B1},
    {OP_gbb, 11, 0x1B2}, {OP_fsm, 11, 0x1B4}, {OP_fsmh, 11, 0x1B5}, {OP_fsmb, 11, 0x1B6}, {OP_frest, 11, 0x1B8},
    {OP_frsqest, 11, 0x1B9}, {OP_lqx, 11, 0x1C4}, {OP_rotqbybi, 11, 0x1CC}, {OP_rotqmbybi, 11, 0x1CD},
    {OP_shlqbybi, 11, 0x1CF}, {OP_cbx, 11, 0x1D4}, {OP_chx, 11, 0x1D5}, {OP_cwx, 11, 0x1D6}, {OP_cdx, 11, 0x1D7},
    {OP_rotqbi, 11, 0x1D8}, {OP_rotqmbi, 11, 0x1D9}, {OP_shlqbi, 11, 0x1DB}, {OP_rotqby, 11, 0x1DC},
    {OP_rotqmby, 11, 0x1DD}, {OP_shlqby, 11, 0x1DF}, {OP_orx, 11, 0x1F0}, {OP_cbd, 11, 0x1F4}, {OP_chd, 11, 0x1F5},
    {OP_cwd, 11, 0x1F6}, {OP_cdd, 11, 0x1F7}, {OP_rotqbii, 11, 0x1F8}, {OP_rotqmbii, 11, 0x1F9},
    {OP_shlqbii, 11, 0x1FB}, {OP_rotqbyi, 11, 0x1FC}, {OP_rotqmbyi, 11, 0x1FD}, {OP_shlqbyi, 11, 0x1FF},
    {OP_nop, 11, 0x201}, {OP_cgt, 11, 0x240}, {OP_xor, 11, 0x241}, {OP_cgth, 11, 0x248}, {OP_eqv, 11, 0x249},
    {OP_cgtb, 11, 0x250}, {OP_sumb, 11, 0x253}, {OP_hgt, 11, 0x258}, {OP_clz, 11, 0x2A5}, {OP_xswd, 11, 0x2A6},
    {OP_xshw, 11, 0x2AE}, {OP_cntb, 11, 0x2B4}, {OP_xsbh, 11, 0x2B6}, {OP_clgt, 11, 0x2C0}, {OP_andc, 11, 0x2C1},
    {OP_fcgt, 11, 0x2C2}, {OP_dfcgt, 11, 0x2C3}, {OP_fa, 11, 0x2C4}, {OP_fs, 11, 0x2C5}, {OP_fm, 11, 0x2C6},
    {OP_clgth, 11, 0x2C8}, {OP_orc, 11, 0x2C9}, {OP_fcmgt, 11, 0x2CA}, {OP_dfcmgt, 11, 0x2CB}, {OP_dfa, 11, 0x2CC},
    {OP_dfs, 11, 0x2CD}, {OP_dfm, 11, 0x2CE}, {OP_clgtb, 11, 0x2D0}, {OP_hlgt, 11, 0x2D8}, {OP_dfma, 11, 0x35C},
    {OP_dfms, 11, 0x35D}, {OP_dfnms, 11, 0x35E}, {OP_dfnma, 11, 0x35F}, {OP_ceq, 11, 0x3C0}, {OP_mpyhhu, 11, 0x3CE},
    {OP_addx, 11, 0x340}, {OP_sfx, 11, 0x341}, {OP_cgx, 11, 0x342}, {OP_bgx, 11, 0x343}, {OP_mpyhha, 11, 0x346},
    {OP_mpyhhau, 11, 0x34E}, {OP_fscrrd, 11, 0x398}, {OP_fesd, 11, 0x3B8}, {OP_frds, 11, 0x3B9},
    {OP_fscrwr, 11, 0x3BA}, {OP_dftsv, 11, 0x3BF}, {OP_fceq, 11, 0x3C2}, {OP_dfceq, 11, 0x3C3}, {OP_mpy, 11, 0x3C4},
    {OP_mpyh, 11, 0x3C5}, {OP_mpyhh, 11, 0x3C6}, {OP_mpys, 11, 0x3C7}, {OP_ceqh, 11, 0x3C8}, {OP_fcmeq, 11, 0x3CA},
    {OP_dfcmeq, 11, 0x3CB}, {OP_mpyu, 11, 0x3CC}, {OP_ceqb, 11, 0x3D0}, {OP_fi, 11, 0x3D4}, {OP_heq, 11, 0x3D8},
};

struct Table {
    std::array<Op, 2048> t{};
    Table() {
        for (int width : {4, 7, 8, 9, 10, 11})
            for (const Encoding& e : kEnc) {
                if (e.width != width) continue;
                const u32 lo = u32(e.value) << (11 - width), n = 1u << (11 - width);
                for (u32 i = 0; i < n; ++i) t[lo + i] = e.op;
            }
    }
};
const Table g_table;

[[noreturn]] void spu_fatal(SPUThread& s, const char* what, u32 op) {
    LOG_ERR("SPU%d (%s): %s at 0x%05x (word 0x%08x)", s.index, s.name.c_str(), what, s.pc, op);
    throw std::runtime_error("SPU fault");
}
}  // namespace

const char* spu_op_name(u32 op) { return kOps[g_table.t[op >> 21]].name; }

std::string spu_where(const SPUThread& s) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "SPU%d pc=0x%05x lr=0x%05x sp=0x%05x", s.index, s.pc, s.gpr[0].w[3], s.gpr[1].w[3]);
    return buf;
}

bool spu_check_interrupt(SPUThread& s, u32 next_pc) {
    if (!s.int_enabled) return false;
    if (!(spu_event_pending(s) & s.event_mask)) return false;
    s.srr0 = next_pc;
    s.int_enabled = false;
    s.pc = 0;
    return true;
}

// Branch-indirect flags: bit 0x20 of the RB field disables interrupts, 0x10 enables them.
static inline bool spu_branch_flags(SPUThread& s, u32 op, u32 target) {
    if (op & (0x20u << 14)) s.int_enabled = false;
    else if (op & (0x10u << 14)) { s.int_enabled = true; s.pc = target; return spu_check_interrupt(s, target); }
    return false;
}

bool spu_interpret(SPUThread& s, bool one_block) {
    using namespace spu;
    for (;;) {
        const u32 pc = s.pc;
        const u32 op = ls_read32(s, pc);
        u32 next = pc + 4;
        ++s.insn_count;
        switch (g_table.t[op >> 21]) {
#define SIMPLE(name, fn) case OP_##name: fn(s, op); break;
        SIMPLE(selb, selb) SIMPLE(shufb, shufb) SIMPLE(mpya, mpya) SIMPLE(fnms, fnms) SIMPLE(fma, fma) SIMPLE(fms, fms)
        SIMPLE(ila, ila) SIMPLE(il, il) SIMPLE(ilh, ilh) SIMPLE(ilhu, ilhu) SIMPLE(iohl, iohl) SIMPLE(fsmbi, fsmbi)
        SIMPLE(ai, ai) SIMPLE(ahi, ahi) SIMPLE(sfi, sfi) SIMPLE(sfhi, sfhi) SIMPLE(andi, andi) SIMPLE(andhi, andhi)
        SIMPLE(andbi, andbi) SIMPLE(ori, ori) SIMPLE(orhi, orhi) SIMPLE(orbi, orbi) SIMPLE(xori, xori)
        SIMPLE(xorhi, xorhi) SIMPLE(xorbi, xorbi) SIMPLE(mpyi, mpyi) SIMPLE(mpyui, mpyui) SIMPLE(cgti, cgti)
        SIMPLE(cgthi, cgthi) SIMPLE(cgtbi, cgtbi) SIMPLE(ceqi, ceqi) SIMPLE(ceqhi, ceqhi) SIMPLE(ceqbi, ceqbi)
        SIMPLE(clgti, clgti) SIMPLE(clgthi, clgthi) SIMPLE(clgtbi, clgtbi)
        SIMPLE(a, a) SIMPLE(ah, ah) SIMPLE(sf, sf) SIMPLE(sfh, sfh) SIMPLE(and, and_) SIMPLE(andc, andc) SIMPLE(or, or_)
        SIMPLE(orc, orc) SIMPLE(xor, xor_) SIMPLE(nand, nand) SIMPLE(nor, nor) SIMPLE(eqv, eqv) SIMPLE(orx, orx)
        SIMPLE(cg, cg) SIMPLE(bg, bg) SIMPLE(addx, addx) SIMPLE(sfx, sfx) SIMPLE(cgx, cgx) SIMPLE(bgx, bgx)
        SIMPLE(absdb, absdb) SIMPLE(avgb, avgb) SIMPLE(sumb, sumb) SIMPLE(clz, clz) SIMPLE(cntb, cntb)
        SIMPLE(xsbh, xsbh) SIMPLE(xshw, xshw) SIMPLE(xswd, xswd) SIMPLE(cgt, cgt) SIMPLE(cgth, cgth) SIMPLE(cgtb, cgtb)
        SIMPLE(clgt, clgt) SIMPLE(clgth, clgth) SIMPLE(clgtb, clgtb) SIMPLE(ceq, ceq) SIMPLE(ceqh, ceqh) SIMPLE(ceqb, ceqb)
        SIMPLE(mpy, mpy) SIMPLE(mpyu, mpyu) SIMPLE(mpyh, mpyh) SIMPLE(mpys, mpys) SIMPLE(mpyhh, mpyhh)
        SIMPLE(mpyhhu, mpyhhu) SIMPLE(mpyhha, mpyhha) SIMPLE(mpyhhau, mpyhhau)
        SIMPLE(rot, rot) SIMPLE(rotm, rotm) SIMPLE(rotma, rotma) SIMPLE(shl, shl) SIMPLE(roth, roth) SIMPLE(rothm, rothm)
        SIMPLE(rotmah, rotmah) SIMPLE(shlh, shlh) SIMPLE(roti, roti) SIMPLE(rotmi, rotmi) SIMPLE(rotmai, rotmai)
        SIMPLE(shli, shli) SIMPLE(rothi, rothi) SIMPLE(rothmi, rothmi) SIMPLE(rotmahi, rotmahi) SIMPLE(shlhi, shlhi)
        SIMPLE(rotqby, rotqby) SIMPLE(rotqbybi, rotqbybi) SIMPLE(rotqbyi, rotqbyi) SIMPLE(rotqmby, rotqmby)
        SIMPLE(rotqmbybi, rotqmbybi) SIMPLE(rotqmbyi, rotqmbyi) SIMPLE(shlqby, shlqby) SIMPLE(shlqbybi, shlqbybi)
        SIMPLE(shlqbyi, shlqbyi) SIMPLE(rotqbi, rotqbi) SIMPLE(rotqbii, rotqbii) SIMPLE(rotqmbi, rotqmbi)
        SIMPLE(rotqmbii, rotqmbii) SIMPLE(shlqbi, shlqbi) SIMPLE(shlqbii, shlqbii)
        SIMPLE(cbx, cbx) SIMPLE(chx, chx) SIMPLE(cwx, cwx) SIMPLE(cdx, cdx) SIMPLE(cbd, cbd) SIMPLE(chd, chd)
        SIMPLE(cwd, cwd) SIMPLE(cdd, cdd) SIMPLE(gb, gb) SIMPLE(gbh, gbh) SIMPLE(gbb, gbb) SIMPLE(fsm, fsm)
        SIMPLE(fsmh, fsmh) SIMPLE(fsmb, fsmb)
        SIMPLE(lqd, lqd) SIMPLE(stqd, stqd) SIMPLE(lqx, lqx) SIMPLE(stqx, stqx) SIMPLE(lqa, lqa) SIMPLE(stqa, stqa)
        SIMPLE(fa, fa) SIMPLE(fs, fs) SIMPLE(fm, fm) SIMPLE(fcgt, fcgt) SIMPLE(fceq, fceq) SIMPLE(fcmgt, fcmgt)
        SIMPLE(fcmeq, fcmeq) SIMPLE(frest, frest) SIMPLE(frsqest, frsqest) SIMPLE(fi, fi) SIMPLE(cflts, cflts)
        SIMPLE(cfltu, cfltu) SIMPLE(csflt, csflt) SIMPLE(cuflt, cuflt)
        SIMPLE(dfa, dfa) SIMPLE(dfs, dfs) SIMPLE(dfm, dfm) SIMPLE(dfma, dfma) SIMPLE(dfms, dfms) SIMPLE(dfnms, dfnms)
        SIMPLE(dfnma, dfnma) SIMPLE(dfcgt, dfcgt) SIMPLE(dfceq, dfceq) SIMPLE(dfcmgt, dfcmgt) SIMPLE(dfcmeq, dfcmeq)
        SIMPLE(fesd, fesd) SIMPLE(frds, frds) SIMPLE(dftsv, dftsv) SIMPLE(fscrrd, fscrrd)
#undef SIMPLE
        case OP_lqr: lqr(s, op, pc); break;
        case OP_stqr: stqr(s, op, pc); break;
        case OP_hbra: case OP_hbrr: case OP_hbr: case OP_lnop: case OP_nop: case OP_sync: case OP_dsync:
        case OP_fscrwr: case OP_mtspr:
            break;
        case OP_mfspr: G(SPU_RT).q = 0; break;
        case OP_rdch: {
            s.pc = pc;  // a blocking read may report where it waits
            const u32 v = spu_read_channel(s, SPU_RA);
            G(SPU_RT) = pref_only(v);
            if (s.halted) return false;
            if (one_block) { s.pc = next; return true; }
            break;
        }
        case OP_rchcnt:
            G(SPU_RT) = pref_only(spu_channel_count(s, SPU_RA));
            if (one_block) { s.pc = next; return true; }
            break;
        case OP_wrch:
            s.pc = pc;
            spu_write_channel(s, SPU_RA, PREF(SPU_RT));
            if (s.halted) return false;
            if (s.int_enabled && spu_check_interrupt(s, next)) { if (one_block) return true; continue; }
            if (one_block) { s.pc = next; return true; }
            break;
        // ---- branches
        case OP_br: next = (pc + (u32(SPU_I16) << 2)) & 0x3fffc; break;
        case OP_bra: next = (u32(SPU_I16) << 2) & 0x3fffc; break;
        case OP_brsl: G(SPU_RT) = pref_only(pc + 4); next = (pc + (u32(SPU_I16) << 2)) & 0x3fffc; break;
        case OP_brasl: G(SPU_RT) = pref_only(pc + 4); next = (u32(SPU_I16) << 2) & 0x3fffc; break;
        case OP_brz: if (PREF(SPU_RT) == 0) next = (pc + (u32(SPU_I16) << 2)) & 0x3fffc; break;
        case OP_brnz: if (PREF(SPU_RT) != 0) next = (pc + (u32(SPU_I16) << 2)) & 0x3fffc; break;
        case OP_brhz: if (SPU_H(G(SPU_RT), 1) == 0) next = (pc + (u32(SPU_I16) << 2)) & 0x3fffc; break;
        case OP_brhnz: if (SPU_H(G(SPU_RT), 1) != 0) next = (pc + (u32(SPU_I16) << 2)) & 0x3fffc; break;
        case OP_bi: {
            const u32 t = PREF(SPU_RA) & 0x3fffc;
            if (spu_branch_flags(s, op, t)) { if (one_block) return true; continue; }
            next = t;
            break;
        }
        case OP_bisl: {
            const u32 t = PREF(SPU_RA) & 0x3fffc;
            G(SPU_RT) = pref_only(pc + 4);
            if (spu_branch_flags(s, op, t)) { if (one_block) return true; continue; }
            next = t;
            break;
        }
        case OP_biz: case OP_binz: case OP_bihz: case OP_bihnz: {
            const Op o = g_table.t[op >> 21];
            const bool taken = o == OP_biz ? PREF(SPU_RT) == 0 : o == OP_binz ? PREF(SPU_RT) != 0
                             : o == OP_bihz ? SPU_H(G(SPU_RT), 1) == 0 : SPU_H(G(SPU_RT), 1) != 0;
            if (taken) {
                const u32 t = PREF(SPU_RA) & 0x3fffc;
                if (spu_branch_flags(s, op, t)) { if (one_block) return true; continue; }
                next = t;
            }
            break;
        }
        case OP_iret: {
            const u32 t = s.srr0 & 0x3fffc;
            if (spu_branch_flags(s, op, t)) { if (one_block) return true; continue; }
            next = t;
            break;
        }
        case OP_bisled: {
            const u32 t = PREF(SPU_RA) & 0x3fffc;
            G(SPU_RT) = pref_only(pc + 4);
            if (spu_event_pending(s) & s.event_mask) {
                if (spu_branch_flags(s, op, t)) { if (one_block) return true; continue; }
                next = t;
            }
            break;
        }
        case OP_stop: case OP_stopd: {
            s.pc = pc;
            const u32 code = op & 0x3fff;
            if (!s.stop_handler || !s.stop_handler(s, code)) return false;
            if (s.halted) return false;
            if (s.pc == pc) s.pc = pc + 4;
            if (one_block) return true;
            continue;
        }
        case OP_heq: if (s32(PREF(SPU_RA)) == s32(PREF(SPU_RB))) spu_fatal(s, "halt (heq)", op); break;
        case OP_heqi: if (s32(PREF(SPU_RA)) == SPU_I10) spu_fatal(s, "halt (heqi)", op); break;
        case OP_hgt: if (s32(PREF(SPU_RA)) > s32(PREF(SPU_RB))) spu_fatal(s, "halt (hgt)", op); break;
        case OP_hgti: if (s32(PREF(SPU_RA)) > SPU_I10) spu_fatal(s, "halt (hgti)", op); break;
        case OP_hlgt: if (PREF(SPU_RA) > PREF(SPU_RB)) spu_fatal(s, "halt (hlgt)", op); break;
        case OP_hlgti: if (PREF(SPU_RA) > u32(SPU_I10)) spu_fatal(s, "halt (hlgti)", op); break;
        default:
            s.pc = pc;
            spu_fatal(s, "invalid instruction", op);
        }
        s.pc = next;
        if (next != pc + 4) {
            if (one_block) return true;
            if (s.stop_request.load(std::memory_order_relaxed)) return false;
        }
    }
}

