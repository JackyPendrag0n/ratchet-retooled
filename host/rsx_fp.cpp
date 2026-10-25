// RSX fragment program -> GLSL translator. See rsx_fp.h.
#include "rsx_fp.h"
#include "rsx_iface.h"
#include "rsx_tex.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace rsx {
namespace {

// ---- decoding --------------------------------------------------------------------------
// Guest stores each 32-bit word big-endian with its two halfwords swapped.
inline u32 rdw(const u8* p, u32 off) {
    const u32 w = (u32(p[off]) << 24) | (u32(p[off + 1]) << 16) | (u32(p[off + 2]) << 8) | u32(p[off + 3]);
    return (w << 16) | (w >> 16);
}
inline u32 bf(u32 v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }

enum Op : u32 {
    NOP = 0x00, MOV = 0x01, MUL = 0x02, ADD = 0x03, MAD = 0x04, DP3 = 0x05, DP4 = 0x06, DST = 0x07,
    MIN = 0x08, MAX = 0x09, SLT = 0x0A, SGE = 0x0B, SLE = 0x0C, SGT = 0x0D, SNE = 0x0E, SEQ = 0x0F,
    FRC = 0x10, FLR = 0x11, KIL = 0x12, PK4 = 0x13, UP4 = 0x14, DDX = 0x15, DDY = 0x16, TEX = 0x17,
    TXP = 0x18, TXD = 0x19, RCP = 0x1A, RSQ = 0x1B, EX2 = 0x1C, LG2 = 0x1D, LIT = 0x1E, LRP = 0x1F,
    STR = 0x20, SFL = 0x21, COS = 0x22, SIN = 0x23, PK2 = 0x24, UP2 = 0x25, POW = 0x26, PKB = 0x27,
    UPB = 0x28, PK16 = 0x29, UP16 = 0x2A, BEM = 0x2B, PKG = 0x2C, UPG = 0x2D, DP2A = 0x2E, TXL = 0x2F,
    TXB = 0x31, TEXBEM = 0x33, TXPBEM = 0x34, BEMLUM = 0x35, REFL = 0x36, TIMESWTEX = 0x37, DP2 = 0x38,
    NRM = 0x39, DIV = 0x3A, DIVSQ = 0x3B, LIF = 0x3C, FENCT = 0x3D, FENCB = 0x3E,
    BRK = 0x40, CAL = 0x41, IFE = 0x42, LOOP = 0x43, REP = 0x44, RET = 0x45,
};

enum : u32 { REG_TEMP = 0, REG_INPUT = 1, REG_CONST = 2, REG_UNK = 3 };

struct Insn {
    u32 d = 0, s[3] = {0, 0, 0};
    u32 pc = 0;        // instruction slot index (16-byte units)
    u32 op = 0;
    int cidx = -1;     // fc[] index of the inline constant, -1 if none

    // OPDEST
    bool end() const { return d & 1; }
    u32 dest_reg() const { return bf(d, 1, 6); }
    bool dst_fp16() const { return bf(d, 7, 1); }
    bool set_cond() const { return bf(d, 8, 1); }
    u32 mask() const { return bf(d, 9, 4); }
    u32 attr() const { return bf(d, 13, 4); }
    u32 tex_num() const { return bf(d, 17, 4); }
    bool exp_tex() const { return bf(d, 21, 1); }
    u32 prec() const { return bf(d, 22, 2); }
    bool no_dest() const { return bf(d, 30, 1); }
    bool saturate() const { return bf(d, 31, 1); }
    // SRC0 condition fields
    u32 exec() const { return bf(s[0], 18, 3); }  // bit0 lt, bit1 eq, bit2 gr
    u32 cond_swz() const { return bf(s[0], 21, 8); }
    u32 cond_mod_reg() const { return bf(s[0], 30, 1); }
    u32 cond_reg() const { return bf(s[0], 31, 1); }
    // SRC1 specials
    u32 scale() const { return bf(s[1], 28, 3); }
    u32 src_prec_mod(int n) const { return bf(s[1], 19 + 3 * n, 3); }
    u32 else_off() const { return s[1] & 0x7fffffffu; }
    u32 rep_count() const { return bf(s[1], 2, 8); }
    u32 init_counter() const { return bf(s[1], 10, 8); }
    int increment() const { return int(s8(bf(s[1], 19, 8))); }
    // SRC2 specials
    u32 end_off() const { return s[2]; }
    u32 addr_reg() const { return bf(s[2], 19, 11); }
    bool use_index_reg() const { return bf(s[2], 30, 1); }
    bool persp_corr() const { return bf(s[2], 31, 1); }
};
// Common source fields.
inline u32 s_type(u32 s) { return bf(s, 0, 2); }
inline u32 s_index(u32 s) { return bf(s, 2, 6); }
inline bool s_fp16(u32 s) { return bf(s, 8, 1); }
inline u32 s_swz(u32 s) { return bf(s, 9, 8); }
inline bool s_neg(u32 s) { return bf(s, 17, 1); }
inline bool s_abs(u32 s, int n) { return n == 0 ? bf(s, 29, 1) : bf(s, 18, 1); }

inline bool is_flow(u32 op) { return op >= BRK && op <= RET; }

inline bool is_tex_op(u32 op) {
    return op == TEX || op == TXP || op == TXD || op == TXB || op == TXL || op == TEXBEM || op == TXPBEM;
}

// Whether the instruction is followed by an inline constant block (RPCS3 FPToCFG rules).
inline bool takes_constant(const Insn& in) {
    if (is_flow(in.op) || in.op == FENCT || in.op == FENCB) return false;
    return s_type(in.s[0]) == REG_CONST || s_type(in.s[1]) == REG_CONST || s_type(in.s[2]) == REG_CONST;
}

int operand_count(u32 op) {
    switch (op) {
    case NOP: case KIL: case SFL: case STR: case FENCT: case FENCB:
    case BRK: case CAL: case IFE: case LOOP: case REP: case RET:
        return 0;
    case MOV: case FRC: case FLR: case PK4: case UP4: case DDX: case DDY: case TEX: case TXP:
    case RCP: case RSQ: case EX2: case LG2: case LIT: case COS: case SIN: case PK2: case UP2:
    case PKB: case UPB: case PK16: case UP16: case PKG: case UPG: case NRM: case LIF:
        return 1;
    case MAD: case LRP: case DP2A: case BEM: case TXD: case TEXBEM: case TXPBEM:
        return 3;
    default:
        return 2;
    }
}

struct Scan {
    std::vector<Insn> insns;  // includes NOPs (for flow target bookkeeping)
    FpInfo info;
};

Scan scan_program(const u8* p, u32 max_bytes) {
    Scan sc;
    FpInfo& info = sc.info;
    u64 h = 0xcbf29ce484222325ull;
    auto mix = [&](u32 w) {
        for (int i = 0; i < 4; ++i) { h ^= (w >> (8 * i)) & 0xff; h *= 0x100000001b3ull; }
    };
    u32 off = 0;
    int nconst = 0;
    while (off + 16 <= max_bytes) {
        Insn in;
        in.d = rdw(p, off);
        for (int k = 0; k < 3; ++k) in.s[k] = rdw(p, off + 4 + 4 * k);
        in.pc = off / 16;
        in.op = bf(in.d, 24, 6) | (bf(in.s[1], 31, 1) << 6);
        for (u32 w : {in.d, in.s[0], in.s[1], in.s[2]}) mix(w);
        off += 16;
        if (takes_constant(in)) {
            if (in.op != NOP && off + 16 <= max_bytes) {
                in.cidx = nconst++;
                info.const_offsets.push_back(off);
            }
            off += 16;
        }
        if (is_tex_op(in.op)) info.textures_used |= u16(1u << in.tex_num());
        if (in.op == KIL) info.uses_kill = true;
        if (!is_flow(in.op) && in.op != NOP && in.op != KIL && !in.no_dest() && in.exec()) {
            const u32 m = in.mask() ? in.mask() : 0xf;
            if (!in.dst_fp16() && in.dest_reg() == 1 && (m & 4)) info.writes_depth = true;
            if (in.dst_fp16() && in.dest_reg() == 3 && (m & 3)) info.writes_depth = true;
        }
        sc.insns.push_back(in);
        if (in.end()) { info.found_end = true; break; }
    }
    info.size_bytes = off < max_bytes ? off : max_bytes;
    info.hash = h;
    return sc;
}

// ---- GLSL generation ---------------------------------------------------------------------
const char* const kXYZW = "xyzw";

std::string swz_str(u32 swz) {
    std::string s = ".";
    for (int i = 0; i < 4; ++i) s += kXYZW[(swz >> (2 * i)) & 3];
    return s == ".xyzw" ? "" : s;
}

class Decompiler {
public:
    Decompiler(const Scan& sc, const FpKey& key) : sc_(sc), key_(key) {}
    std::string run();

private:
    const Scan& sc_;
    const FpKey& key_;
    const Insn* cur_ = nullptr;
    std::string body_;
    int level_ = 1;
    bool valid_ = true;
    std::vector<std::string> notes_;

    // register usage
    std::set<u32> r_used_, h_used_;
    bool cc_used_[2] = {false, false};
    bool x2d_used_ = false;
    // lanes read anywhere via the R view / H view (for alias syncing)
    bool readR_[64][4] = {}, readH_[128][4] = {};

    // feature flags
    bool use_wpos_ = false, use_col0_ = false, use_col1_ = false, use_fog_ = false, use_ssa_ = false;
    bool use_clamp_ = false, use_divsq_ = false, use_lit_ = false, use_pkg_ = false, use_upg_ = false, use_srgb_ = false, use_expand_ = false,
         use_depth_rgba_ = false, use_shadow_soft_ = false, use_shadow_tap_ = false;
    bool use_indexed_ = false, use_kill_ = false;

    struct Block { bool loop; u32 else_pc; u32 end_pc; bool in_else; u32 init; int incr; };
    std::vector<Block> stack_;
    int loop_depth_ = 0;

    void emit(const std::string& s) { body_.append(size_t(level_), '\t'); body_ += s; body_ += '\n'; }
    std::string reg(u32 idx, bool fp16) {
        (fp16 ? h_used_ : r_used_).insert(idx);
        return (fp16 ? "h" : "r") + std::to_string(idx);
    }
    std::string cc(u32 idx) { cc_used_[idx] = true; return "cc" + std::to_string(idx); }
    std::string mask_str() const {
        const u32 m = cur_->mask();
        if (m == 0 || m == 0xf) return "";
        std::string s = ".";
        for (int i = 0; i < 4; ++i) if (m & (1u << i)) s += kXYZW[i];
        return s;
    }
    std::string clamp_value(const std::string& code, u32 prec) {
        switch (prec) {
        case 1: return "clamp16(" + code + ")";
        case 2: use_clamp_ = true; return "precision_clamp(" + code + ", -2., 2.)";
        case 3: use_clamp_ = true; return "precision_clamp(" + code + ", -1., 1.)";
        case 4: use_clamp_ = true; return "precision_clamp(" + code + ", 0., 1.)";
        default: return code;
        }
    }
    std::string raw_cond(u32 exec, u32 ccidx, u32 swz) {
        const std::string c = cc(ccidx) + swz_str(swz);
        const std::string z = "vec4(0.)";
        const bool lt = exec & 1, eq = exec & 2, gr = exec & 4;
        if (gr && eq) return "greaterThanEqual(" + c + ", " + z + ")";
        if (lt && eq) return "lessThanEqual(" + c + ", " + z + ")";
        if (gr && lt) return "notEqual(" + c + ", " + z + ")";
        if (gr) return "greaterThan(" + c + ", " + z + ")";
        if (lt) return "lessThan(" + c + ", " + z + ")";
        return "equal(" + c + ", " + z + ")";
    }
    std::string cond_expr(u32 exec) {  // scalar "any(...)" form
        if (exec == 7) return "true";
        if (exec == 0) return "false";
        return "any(" + raw_cond(exec, cur_->cond_reg(), cur_->cond_swz()) + ")";
    }
    void flow_op(const std::string& code) {
        const u32 e = cur_->exec();
        if (e == 7) emit(code + ";");
        else if (e == 0) emit("//" + code + ";");
        else emit("if (" + cond_expr(e) + ") " + code + ";");
    }
    // Conditional assignment lhs = rhs honoring the execution condition.
    void cond_assign(const std::string& lhs, const std::string& rhs, const std::string& m) {
        const u32 e = cur_->exec();
        if (e == 7) { emit(lhs + " = " + rhs + ";"); return; }
        const std::string c = raw_cond(e, cur_->cond_reg(), cur_->cond_swz()) + m;
        emit(lhs + " = _select(" + lhs + ", " + rhs + ", " + c + ");");
    }

    std::string src(int n);
    std::string src0_tex() { return (cur_->op == TEXBEM || cur_->op == TXPBEM) ? std::string("x2d") : src(0); }
    void set_dst(std::string code);
    void sync_aliases();
    void texture_op(int base);
    void instruction();
    void mark_reads();
};

// Source operand expression (RPCS3 GetSRC).
std::string Decompiler::src(int n) {
    const Insn& in = *cur_;
    const u32 s = in.s[n];
    u32 prec = in.src_prec_mod(n);
    std::string ret;
    switch (s_type(s)) {
    case REG_TEMP:
        if (s_fp16(s) && prec == 1) prec = 0;
        ret = reg(s_index(s), s_fp16(s));
        break;
    case REG_INPUT: {
        u32 id = in.use_index_reg() ? in.addr_reg() + 4 : in.attr();
        const bool dynamic = in.use_index_reg() && loop_depth_ > 0;
        if (dynamic) {
            const Block* lp = nullptr;
            for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) if (it->loop) { lp = &*it; break; }
            const std::string ivar = "i" + std::to_string(loop_depth_ - 1);
            const std::string idx = "int(" + std::to_string(lp ? lp->init : 0) + " + " + ivar + " * " +
                                    std::to_string(lp ? lp->incr : 1) + ") + " + std::to_string(in.addr_reg());
            use_indexed_ = true;
            ret = "_indexed_load(" + idx + ")";
            if (in.persp_corr()) ret = "(" + ret + " * gl_FragCoord.w)";
            break;
        }
        switch (id) {
        case 0: use_wpos_ = true; ret = "wpos"; break;
        case 1: use_col0_ = true; ret = "_saturate(in_col0)"; prec = 0; break;
        case 2: use_col1_ = true; ret = "_saturate(in_col1)"; prec = 0; break;
        case 3: use_fog_ = true; ret = "fogc"; break;
        case 14: use_ssa_ = true; ret = "ssa"; prec = 0; break;
        default:
            if (id >= 4 && id <= 13) {
                ret = "tc" + std::to_string(id - 4);
                if (in.persp_corr()) ret = "(" + ret + " * gl_FragCoord.w)";
            } else {
                valid_ = false;
                notes_.push_back("bad input register " + std::to_string(id));
                ret = "vec4(0.)";
                prec = 0;
            }
            break;
        }
        break;
    }
    case REG_CONST:
        ret = "fc[" + std::to_string(in.cidx < 0 ? 0 : in.cidx) + "]";
        if (in.cidx < 0) { valid_ = false; notes_.push_back("constant past end of program"); }
        break;
    default:
        valid_ = false;
        notes_.push_back("invalid source type 3");
        ret = "vec4(0.)";
        prec = 0;
        break;
    }
    ret += swz_str(s_swz(s));
    const bool prec_before_abs = prec == 4;
    if (prec_before_abs) ret = clamp_value(ret, prec);
    if (s_abs(s, n)) ret = "abs(" + ret + ")";
    if (prec && !prec_before_abs) ret = clamp_value(ret, prec);
    if (s_neg(s)) ret = "-" + ret;
    return ret;
}

// Destination write (RPCS3 SetDst).
void Decompiler::set_dst(std::string code) {
    const Insn& in = *cur_;
    if (in.exec() == 0) return;
    switch (in.scale()) {
    case 0: break;
    case 1: code = "(" + code + " * 2.)"; break;
    case 2: code = "(" + code + " * 4.)"; break;
    case 3: code = "(" + code + " * 8.)"; break;
    case 5: code = "(" + code + " / 2.)"; break;
    case 6: code = "(" + code + " / 4.)"; break;
    case 7: code = "(" + code + " / 8.)"; break;
    default: notes_.push_back("bad scale 4"); break;
    }
    if (!in.no_dest()) {
        if (in.saturate()) {
            code = clamp_value(code, 4);
        } else if (in.prec()) {
            switch (in.op) {
            case NRM: case MAX: case MIN: case COS: case SIN: case REFL: case FRC: case LIT: case LIF: case LG2:
                break;
            case MOV:
                if (in.dst_fp16() && s_fp16(in.s[0]) && s_type(in.s[0]) == REG_TEMP) break;
                [[fallthrough]];
            default:
                if (in.prec() == 1 && !in.dst_fp16()) break;
                code = clamp_value(code, in.prec());
                break;
            }
        }
    }
    const std::string m = mask_str();
    code += m;
    if (in.no_dest()) {
        if (in.set_cond()) cond_assign(cc(in.cond_mod_reg()) + m, code, m);
        else emit("// no-dest result: " + code);
        return;
    }
    const std::string dest = reg(in.dest_reg(), in.dst_fp16()) + m;
    cond_assign(dest, code, m);
    if (in.set_cond()) emit(cc(in.cond_mod_reg()) + m + " = " + dest + ";");
    sync_aliases();
}

// After a write, refresh the aliased view of every written lane that is read through it.
void Decompiler::sync_aliases() {
    const Insn& in = *cur_;
    const u32 m = in.mask() ? in.mask() : 0xf;
    const u32 d = in.dest_reg();
    if (!in.dst_fp16()) {
        // Rd.c aliases H(2d + c/2) lanes (c&1)*2 .. +1
        for (u32 c = 0; c < 4; ++c) {
            if (!(m & (1u << c))) continue;
            const u32 hreg = 2 * d + (c >> 1), lane = (c & 1) * 2;
            if (!readH_[hreg][lane] && !readH_[hreg][lane + 1]) continue;
            emit(reg(hreg, true) + "." + kXYZW[lane] + kXYZW[lane + 1] + " = unpackHalf2x16(floatBitsToUint(" +
                 reg(d, false) + "." + kXYZW[c] + "));");
        }
    } else {
        // Hd lanes (x,y) -> R(d/2).((d&1)*2), lanes (z,w) -> R(d/2).((d&1)*2+1)
        const u32 r = d >> 1;
        for (u32 pair = 0; pair < 2; ++pair) {
            const u32 ch = (d & 1) * 2 + pair;
            const bool lo = m & (1u << (2 * pair)), hi = m & (1u << (2 * pair + 1));
            if ((!lo && !hi) || !readR_[r][ch]) continue;
            const std::string rv = reg(r, false) + "." + kXYZW[ch];
            const std::string hv = reg(d, true);
            const char a = kXYZW[2 * pair], b = kXYZW[2 * pair + 1];
            if (lo && hi)
                emit(rv + " = uintBitsToFloat(packHalf2x16(" + hv + "." + a + b + "));");
            else if (lo)
                emit(rv + " = uintBitsToFloat((floatBitsToUint(" + rv + ") & 0xffff0000u) | (packHalf2x16(vec2(" + hv +
                     "." + a + ", 0.)) & 0xffffu));");
            else
                emit(rv + " = uintBitsToFloat((floatBitsToUint(" + rv + ") & 0xffffu) | (packHalf2x16(vec2(0., " + hv +
                     "." + b + ")) & 0xffff0000u));");
        }
    }
}

// base: 0 TEX, 1 TXP, 2 TXD, 3 TXB, 4 TXL
void Decompiler::texture_op(int base) {
    const Insn& in = *cur_;
    const u32 i = in.tex_num();
    const std::string t = "tex" + std::to_string(i);
    const std::string sc = "tex_scale[" + std::to_string(i) + "]";
    const std::string c = src0_tex();
    std::string e;
    switch (key_.tex_kind[i]) {
    case TexKind::Tex1D: {
        const std::string k = sc + ".x";
        switch (base) {
        case 0: e = "texture(" + t + ", " + c + ".x * " + k + ")"; break;
        case 1: e = "texture(" + t + ", (" + c + ".x / " + c + ".w) * " + k + ")"; break;
        case 2: e = "textureGrad(" + t + ", " + c + ".x * " + k + ", " + src(1) + ".x * " + k + ", " + src(2) + ".x * " + k + ")"; break;
        case 3: e = "texture(" + t + ", " + c + ".x * " + k + ", " + src(1) + ".x)"; break;
        default: e = "textureLod(" + t + ", " + c + ".x * " + k + ", " + src(1) + ".x)"; break;
        }
        break;
    }
    case TexKind::Tex2D: default: {
        const std::string k = sc + ".xy";
        switch (base) {
        case 0: e = "texture(" + t + ", " + c + ".xy * " + k + ")"; break;
        case 1: e = "texture(" + t + ", (" + c + ".xy / " + c + ".w) * " + k + ")"; break;
        case 2: e = "textureGrad(" + t + ", " + c + ".xy * " + k + ", " + src(1) + ".xy * " + k + ", " + src(2) + ".xy * " + k + ")"; break;
        case 3: e = "texture(" + t + ", " + c + ".xy * " + k + ", " + src(1) + ".x)"; break;
        default: e = "textureLod(" + t + ", " + c + ".xy * " + k + ", " + src(1) + ".x)"; break;
        }
        break;
    }
    case TexKind::Cube: case TexKind::Tex3D: {
        switch (base) {
        case 0: e = "texture(" + t + ", " + c + ".xyz)"; break;
        case 1: e = "texture(" + t + ", " + c + ".xyz / " + c + ".w)"; break;
        case 2: e = "textureGrad(" + t + ", " + c + ".xyz, " + src(1) + ".xyz, " + src(2) + ".xyz)"; break;
        case 3: e = "texture(" + t + ", " + c + ".xyz, " + src(1) + ".x)"; break;
        default: e = "textureLod(" + t + ", " + c + ".xyz, " + src(1) + ".x)"; break;
        }
        break;
    }
    case TexKind::Shadow2D: {
        // A shadow map can be a smaller part of the surface it was drawn into (the game reuses one
        // surface for maps of different sizes): coordinates outside the texture read the border,
        // not what an earlier, larger map left in the rest of the surface.
        const std::string k = sc + ".xy";
        const std::string uv = "vec3(" + c + ".xy * " + k + ", " + c + ".z)";
        const std::string puv = "vec3((" + c + ".xy / " + c + ".w) * " + k + ", " + c + ".z / " + c + ".w)";
        use_shadow_tap_ = true;
        switch (base) {
        case 0: e = "_shadow_tap(" + t + ", " + uv + ", " + k + ")"; break;
        case 1: e = "_shadow_tap(" + t + ", " + puv + ", " + k + ")"; break;
        case 2: e = "textureGrad(" + t + ", " + uv + ", " + src(1) + ".xy * " + k + ", " + src(2) + ".xy * " + k + ")"; break;
        case 3: e = "texture(" + t + ", " + uv + ", " + src(1) + ".x)"; break;
        default: e = "textureLod(" + t + ", " + uv + ", " + src(1) + ".x)"; break;
        }
        if (key_.shadow_soft && base <= 1) {
            use_shadow_soft_ = true;
            e = "_shadow_soft(" + t + ", " + (base == 0 ? uv : puv) + ", " + k + ")";
        }
        e = "vec4(" + e + ")";  // depth compare result broadcast (.xxxx)
        break;
    }
    }
    // TODO: remaining texture format conversions (_process_texel: sign-extension, renormalize,
    // alpha kill, depth->RGBA reinterpretation) are not emulated.
    // TODO: _bx2 should only expand channels the format/remap marks as expandable (RPCS3 uses
    // the BIASED_RENORMALIZATION feature + remap bits); here all four channels are expanded.
    if (const u32 dr = key_.depth_rgba[i]) {
        use_depth_rgba_ = true;
        e = "_depth_rgba(" + e + ".r, " + std::to_string(dr) + "u)";
    }
    if (in.exp_tex()) e = "(" + e + " * 2. - 1.)";
    else if (const u8 g = key_.tex_gamma[i]) {
        use_srgb_ = true;
        if (g == 0xF) e = "_srgb_to_linear(" + e + ")";
        else {
            std::string m = "bvec4(";
            for (int c = 0; c < 4; ++c) m += std::string(c ? ", " : "") + ((g >> c) & 1 ? "true" : "false");
            e = "_select_gamma(" + e + ", " + m + "))";
        }
    }
    if (const u8 x = key_.tex_expand[i]; x && !in.exp_tex()) {
        use_expand_ = true;
        std::string m = "bvec4(";
        for (int c = 0; c < 4; ++c) m += std::string(c ? ", " : "") + ((x >> c) & 1 ? "true" : "false");
        e = std::string(x & 0x10 ? "_expand16(" : "_expand8(") + e + ", " + m + "))";
    }
    set_dst(e);
}

void Decompiler::instruction() {
    const Insn& in = *cur_;
    switch (in.op) {
    case NOP: break;
    case MOV: set_dst(src(0)); break;
    case MUL: set_dst("(" + src(0) + " * " + src(1) + ")"); break;
    case ADD: set_dst("(" + src(0) + " + " + src(1) + ")"); break;
    case MAD: set_dst("fma(" + src(0) + ", " + src(1) + ", " + src(2) + ")"); break;
    case DP2: set_dst("vec4(dot(" + src(0) + ".xy, " + src(1) + ".xy))"); break;
    case DP3: set_dst("vec4(dot(" + src(0) + ".xyz, " + src(1) + ".xyz))"); break;
    case DP4: set_dst("vec4(dot(" + src(0) + ", " + src(1) + "))"); break;
    case DP2A: set_dst("vec4(dot(" + src(0) + ".xy, " + src(1) + ".xy) + " + src(2) + ".x)"); break;
    case DST: set_dst("vec4(1.0, " + src(0) + ".y * " + src(1) + ".y, " + src(0) + ".z, " + src(1) + ".w)"); break;
    case MIN: set_dst("min(" + src(0) + ", " + src(1) + ")"); break;
    case MAX: set_dst("max(" + src(0) + ", " + src(1) + ")"); break;
    case SLT: set_dst("vec4(lessThan(" + src(0) + ", " + src(1) + "))"); break;
    case SGE: set_dst("vec4(greaterThanEqual(" + src(0) + ", " + src(1) + "))"); break;
    case SLE: set_dst("vec4(lessThanEqual(" + src(0) + ", " + src(1) + "))"); break;
    case SGT: set_dst("vec4(greaterThan(" + src(0) + ", " + src(1) + "))"); break;
    case SNE: set_dst("vec4(notEqual(" + src(0) + ", " + src(1) + "))"); break;
    case SEQ: set_dst("vec4(equal(" + src(0) + ", " + src(1) + "))"); break;
    case SFL: set_dst("vec4(0.)"); break;
    case STR: set_dst("vec4(1.)"); break;
    case FRC: set_dst("fract(" + src(0) + ")"); break;
    case FLR: set_dst("floor(" + src(0) + ")"); break;
    case RCP: set_dst("_builtin_rcp(" + src(0) + ".x).xxxx"); break;
    case RSQ: set_dst("_builtin_rsq(" + src(0) + ".x).xxxx"); break;
    case EX2: set_dst("exp2(" + src(0) + ".xxxx)"); break;
    case LG2: set_dst("log2(" + src(0) + ".x).xxxx"); break;
    case POW:  // not implemented by RPCS3; emulate as exp2(b*log2(a))
        set_dst("exp2(" + src(1) + ".x * log2(" + src(0) + ".x)).xxxx"); break;
    case COS: set_dst("cos(" + src(0) + ".xxxx)"); break;
    case SIN: set_dst("sin(" + src(0) + ".xxxx)"); break;
    case DIV: set_dst("_builtin_div(" + src(0) + ", " + src(1) + ".x)"); break;
    case DIVSQ: use_divsq_ = true; set_dst("_builtin_divsq(" + src(0) + ", " + src(1) + ".x)"); break;
    case LIT: use_lit_ = true; set_dst("_builtin_lit(" + src(0) + ")"); break;
    case LIF: use_lit_ = true; set_dst("_builtin_lif(" + src(0) + ")"); break;
    case LRP: set_dst("mix(" + src(2) + ", " + src(1) + ", " + src(0) + ")"); break;
    case REFL: set_dst("reflect(" + src(0) + ", " + src(1) + ")"); break;
    case NRM: set_dst("_builtin_normalize(" + src(0) + ".xyz).xyzz"); break;
    case DDX: set_dst("dFdx(" + src(0) + ")"); break;
    case DDY: set_dst("dFdy(" + src(0) + ")"); break;
    case BEM: set_dst("(" + src(0) + ".xyxy + " + src(1) + ".xxxx * " + src(2) + ".xzxz + " + src(1) + ".yyyy * " + src(2) + ".ywyw)"); break;
    case PK2: set_dst("vec4(uintBitsToFloat(packHalf2x16(" + src(0) + ".xy)))"); break;
    case PK4: set_dst("vec4(uintBitsToFloat(packSnorm4x8(" + src(0) + ")))"); break;
    case PK16: set_dst("vec4(uintBitsToFloat(packUnorm2x16(" + src(0) + ".xy)))"); break;
    case PKB: set_dst("vec4(uintBitsToFloat(packUnorm4x8(" + src(0) + ")))"); break;
    case PKG: use_pkg_ = true; set_dst("_builtin_pkg(" + src(0) + ")"); break;
    case UP2: set_dst("unpackHalf2x16(floatBitsToUint(" + src(0) + ".x)).xyxy"); break;
    case UP4: set_dst("unpackSnorm4x8(floatBitsToUint(" + src(0) + ".x))"); break;
    case UP16: set_dst("unpackUnorm2x16(floatBitsToUint(" + src(0) + ".x)).xyxy"); break;
    case UPB: set_dst("unpackUnorm4x8(floatBitsToUint(" + src(0) + ".x))"); break;
    case UPG: use_upg_ = true; set_dst("_builtin_upg(" + src(0) + ".x)"); break;
    case TEXBEM: case TXPBEM:
        x2d_used_ = true;
        emit("x2d = " + src(0) + ".xyxy + " + src(1) + ".xxxx * " + src(2) + ".xzxz + " + src(1) + ".yyyy * " + src(2) + ".ywyw;");
        texture_op(in.op == TEXBEM ? 0 : 1);
        break;
    case TEX: texture_op(0); break;
    case TXP: texture_op(1); break;
    case TXD: texture_op(2); break;
    case TXB: texture_op(3); break;
    case TXL: texture_op(4); break;
    case KIL: use_kill_ = true; flow_op("_fragment_discard = true"); break;
    case FENCT: emit("// FENCT"); break;
    case FENCB: emit("// FENCB"); break;
    case BRK:
        if (loop_depth_) flow_op("break");
        else emit("// BRK outside loop");
        break;
    case RET: flow_op("return"); break;
    case CAL: emit("// CAL (unimplemented, unused by the RSX compiler)"); notes_.push_back("CAL"); break;
    case IFE: {
        const u32 end_pc = in.end_off() >> 2, else_pc = in.else_off() >> 2;
        if (end_pc == in.pc + 1) break;  // empty if
        u32 exec = in.exec();
        bool has_else = else_pc != end_pc;
        if (else_pc == in.pc + 1) { exec ^= 7; has_else = false; }  // empty if-body: invert, use else body
        emit("if (" + cond_expr(exec) + ")");
        emit("{");
        ++level_;
        stack_.push_back({false, has_else ? else_pc : ~0u, end_pc, false, 0, 1});
        break;
    }
    case LOOP: case REP: {
        const u32 end_pc = in.end_off() >> 2;
        if (end_pc == in.pc + 1) break;
        const std::string iv = "i" + std::to_string(loop_depth_);
        std::string pre;
        if (in.exec() != 7) pre = "if (" + cond_expr(in.exec()) + ") ";
        char buf[160];
        std::snprintf(buf, sizeof buf, "for (int %s = 0; %s < %u; %s++) // %s { init %u, incr %d }", iv.c_str(), iv.c_str(),
                      in.rep_count(), iv.c_str(), in.op == LOOP ? "LOOP" : "REP", in.init_counter(), in.increment());
        emit(pre + buf);
        emit("{");
        ++level_;
        stack_.push_back({true, ~0u, end_pc, false, in.init_counter(), in.increment()});
        ++loop_depth_;
        break;
    }
    default: {
        char buf[64];
        std::snprintf(buf, sizeof buf, "// unknown opcode 0x%02x", in.op);
        emit(buf);
        notes_.push_back(buf + 3);
        break;
    }
    }
}

void Decompiler::mark_reads() {
    for (const Insn& in : sc_.insns) {
        const int n = operand_count(in.op);
        for (int k = 0; k < n; ++k) {
            const u32 s = in.s[k];
            if (s_type(s) != REG_TEMP) continue;
            const u32 idx = s_index(s), sw = s_swz(s);
            for (int c = 0; c < 4; ++c) {
                const u32 ch = (sw >> (2 * c)) & 3;
                (s_fp16(s) ? readH_[idx][ch] : readR_[idx][ch]) = true;
            }
        }
    }
    const bool fp32_out = key_.shader_control & 0x40;
    for (u32 r : {0u, 2u, 3u, 4u}) for (int c = 0; c < 4; ++c) (fp32_out ? readR_[r][c] : readH_[r * 2][c]) = true;
    if (key_.shader_control & 0x0e) readR_[1][2] = true;
}

std::string Decompiler::run() {
    mark_reads();
    for (const Insn& in : sc_.insns) {
        cur_ = &in;
        // close / switch blocks that end at this slot
        while (!stack_.empty()) {
            Block& b = stack_.back();
            if (b.end_pc <= in.pc) {
                --level_;
                emit("}");
                if (b.loop) --loop_depth_;
                stack_.pop_back();
                continue;
            }
            if (!b.loop && !b.in_else && b.else_pc <= in.pc) {
                b.in_else = true;
                emit("}");
                emit("else");
                emit("{");
            }
            break;
        }
        instruction();
    }
    while (!stack_.empty()) {
        --level_;
        emit("}");
        stack_.pop_back();
    }

    const bool fp32_out = key_.shader_control & 0x40;
    const char* outs[4] = {"r0", "r2", "r3", "r4"};
    const char* outs16[4] = {"h0", "h4", "h6", "h8"};
    for (int i = 0; i < 4; ++i) reg(fp32_out ? (i == 0 ? 0 : i + 1) : (i == 0 ? 0 : 2 * i + 2), !fp32_out);
    const bool depth = sc_.info.writes_depth && (key_.shader_control & 0x0e);
    if (depth) reg(1, false);

    std::string o;
    o += "#version 450\n";
    char hb[128];
    std::snprintf(hb, sizeof hb, "// RSX fragment program %016llx, %u bytes, %zu constants, shader_control %08x\n",
                  (unsigned long long)sc_.info.hash, sc_.info.size_bytes, sc_.info.const_offsets.size(), key_.shader_control);
    o += hb;
    if (!sc_.info.found_end) o += "// WARNING: no end bit found within the scan bound\n";
    for (auto& n : notes_) o += "// note: " + n + "\n";
    o += kDrawStateGlsl;
    if (!sc_.info.const_offsets.empty())
        o += "layout(std140, set = 0, binding = 1) uniform FragmentConstants { vec4 fc[" + std::to_string(kMaxFpConsts) + "]; };\n";
    for (u32 i = 0; i < 16; ++i) {
        if (!(sc_.info.textures_used & (1u << i))) continue;
        const char* ty = "sampler2D";
        switch (key_.tex_kind[i]) {
        case TexKind::Cube: ty = "samplerCube"; break;
        case TexKind::Tex3D: ty = "sampler3D"; break;
        case TexKind::Shadow2D: ty = "sampler2DShadow"; break;
        case TexKind::Tex1D: ty = "sampler1D"; break;
        default: break;
        }
        o += "layout(set = 0, binding = " + std::to_string(kTextureBindingBase + i) + ") uniform " + ty + " tex" + std::to_string(i) + ";\n";
    }
    o += "\nlayout(location = 0) in vec4 diff_color;\n"
         "layout(location = 1) in vec4 spec_color;\n"
         "layout(location = 2) in vec4 diff_color1;\n"
         "layout(location = 3) in vec4 spec_color1;\n"
         "layout(location = 4) in vec4 fog_c;\n";
    for (int i = 0; i < 10; ++i) o += "layout(location = " + std::to_string(5 + i) + ") in vec4 tc" + std::to_string(i) + ";\n";
    o += "\n";
    for (int i = 0; i < 4; ++i) o += "layout(location = " + std::to_string(i) + ") out vec4 ocol" + std::to_string(i) + ";\n";

    o += R"(
#define _select mix
#define _saturate(x) clamp(x, 0., 1.)
#define _builtin_sqrt(x) sqrt(abs(x))
#define _builtin_rcp(x) (1. / x)
#define _builtin_rsq(x) (1. / _builtin_sqrt(x))
#define _builtin_div(x, y) (x / y)
#define _builtin_normalize(x) (length(x) > 0. ? normalize(x) : x)
vec2 clamp16(vec2 v) { return unpackHalf2x16(packHalf2x16(v)); }
vec4 clamp16(vec4 v) { return vec4(clamp16(v.xy), clamp16(v.zw)); }
)";
    if (use_clamp_)
        o += "vec4 precision_clamp(vec4 x, float lo, float hi) { x = _select(x, vec4(0.), isnan(x)); return clamp(x, lo, hi); }\n";
    if (use_divsq_)
        o += "vec4 _builtin_divsq(vec4 a, float b) { vec4 t = a / _builtin_sqrt(b); return _select(a, t, greaterThan(abs(a), vec4(0.))); }\n";
    if (use_lit_)
        o += R"(vec4 _builtin_lit(vec4 v) {
    vec4 t = vec4(max(v.xy, vec2(0., 1e-10)), v.zw);
    return vec4(1., t.x, t.x > 0. ? exp2(t.w * log2(t.y)) : 0., 1.);
}
vec4 _builtin_lif(vec4 t) { return vec4(1., t.y, t.y > 0. ? exp2(t.w) : 0., 1.); }
)";
    if (use_pkg_)
        o += R"(vec4 _builtin_pkg(vec4 cl) {
    vec4 lo = cl * 12.92, hi = 1.055 * pow(cl, vec4(1. / 2.4)) - 0.055;
    vec4 s = clamp(mix(hi, lo, lessThan(cl, vec4(0.0031308))), 0., 1.);
    return uintBitsToFloat(packUnorm4x8(s)).xxxx;
}
)";
    if (use_srgb_ || key_.srgb_output)
        o += R"(vec4 _srgb_to_linear(vec4 cs) {
    return _select(cs / 12.92, pow((cs + 0.055) / 1.055, vec4(2.4)), greaterThan(cs, vec4(0.04045)));
}
vec4 _select_gamma(vec4 cs, bvec4 m) { return _select(cs, _srgb_to_linear(cs), m); }
vec3 _linear_to_srgb(vec3 cl) {
    cl = clamp(cl, 0., 1.);
    return _select(1.055 * pow(cl, vec3(1. / 2.4)) - 0.055, cl * 12.92, lessThan(cl, vec3(0.0031308)));
}
)";
    if (use_depth_rgba_)
        o += R"(vec4 _depth_rgba(float d, uint ctl) {
    uint z = uint(clamp(d, 0., 1.) * 16777215. + 0.5);
    vec4 argb = vec4(float(z >> 16u), float((z >> 8u) & 255u), float(z & 255u), 0.) / 255.;
    if ((ctl & 0x20000u) != 0u) argb.x = 1.;
    vec4 o;
    for (uint c = 0u; c < 4u; ++c) {
        uint m = (ctl >> (8u + 2u * c)) & 3u;
        o[c] = m == 0u ? 0. : m == 1u ? 1. : argb[(ctl >> (2u * c)) & 3u];
    }
    return o.yzwx;
}
)";
    if (use_shadow_tap_) {
        static const bool noborder = std::getenv("TOD_SHADOW_NOBORDER") != nullptr;   // A/B testing
        o += noborder ? "float _shadow_tap(sampler2DShadow s, vec3 p, vec2 lim) { return texture(s, p); }\n"
                      : R"(float _shadow_tap(sampler2DShadow s, vec3 p, vec2 lim) {
    if (any(lessThan(p.xy, vec2(0.))) || any(greaterThan(p.xy, lim))) p.xy = vec2(-1.);
    return texture(s, p);
}
)";
    }
    if (use_shadow_soft_)
        o += R"(float _shadow_soft(sampler2DShadow s, vec3 p, vec2 lim) {
    vec2 d = 0.85 / (res_inv * vec2(textureSize(s, 0)));
    float r = 0.;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) r += _shadow_tap(s, vec3(p.xy + vec2(x, y) * d, p.z), lim);
    return r / 9.;
}
)";
    if (use_expand_)
        o += R"(vec4 _expand8(vec4 x, bvec4 m) { return _select(x, (floor(fma(x, vec4(255.), vec4(0.5))) - 128.) / 127., m); }
vec4 _expand16(vec4 x, bvec4 m) { return _select(x, (floor(fma(x, vec4(65535.), vec4(0.5))) - 32768.) / 32767., m); }
)";
    if (use_upg_)
        o += R"(vec4 _builtin_upg(float v) {
    vec4 cs = unpackUnorm4x8(floatBitsToUint(v));
    return _select(cs / 12.92, pow((cs + 0.055) / 1.055, vec4(2.4)), greaterThan(cs, vec4(0.04045)));
}
)";
    if (use_indexed_) {
        o += "vec4 _indexed_load(int i) {\n\tswitch (i) {\n";
        for (int i = 0; i < 10; ++i) o += "\tcase " + std::to_string(i) + ": return tc" + std::to_string(i) + ";\n";
        o += "\t}\n\treturn vec4(0., 0., 0., 1.);\n}\n";
    }
    if (use_fog_)
        o += R"(vec4 fetch_fog_value() {
    vec4 r = vec4(fog_c.x, 0., 0., 0.);
    float f = fog_c.x, fa = abs(fog_c.x);
    switch (fog_mode) {
    case 0x2601u: r.y = fog_param1 * f + (fog_param0 - 1.); break;
    case 0x0800u: r.y = exp(11.084 * (fog_param1 * f + fog_param0 - 1.5)); break;
    case 0x0801u: r.y = exp(-pow(4.709 * (fog_param1 * f + fog_param0 - 1.5), 2.)); break;
    case 0x0802u: r.y = exp(11.084 * (fog_param1 * fa + fog_param0 - 1.5)); break;
    case 0x0803u: r.y = exp(-pow(4.709 * (fog_param1 * fa + fog_param0 - 1.5), 2.)); break;
    case 0x0804u: r.y = fog_param1 * fa + (fog_param0 - 1.); break;
    default: return r;
    }
    r.y = clamp(r.y, 0., 1.);
    return r;
}
)";

    // registers (globals so fs_main can 'return' early and main() still gathers outputs)
    o += "\n";
    for (u32 r : r_used_) o += "vec4 r" + std::to_string(r) + " = vec4(0.);\n";
    for (u32 h : h_used_) o += "vec4 h" + std::to_string(h) + " = vec4(0.);\n";
    for (int i = 0; i < 2; ++i) if (cc_used_[i]) o += "vec4 cc" + std::to_string(i) + " = vec4(0.);\n";
    if (x2d_used_) o += "vec4 x2d = vec4(0.);\n";
    o += "bool _fragment_discard = false;\n\nvoid fs_main()\n{\n";
    if (use_wpos_) o += "\tvec4 wpos = vec4(gl_FragCoord.x * res_inv * wpos_ax, gl_FragCoord.y * res_inv * wpos_ay * wpos_scale + wpos_bias, gl_FragCoord.zw);\n";
    if (use_col0_) o += "\tvec4 in_col0 = (two_sided_lighting != 0u && !gl_FrontFacing) ? diff_color1 : diff_color;\n";
    if (use_col1_) o += "\tvec4 in_col1 = (two_sided_lighting != 0u && !gl_FrontFacing) ? spec_color1 : spec_color;\n";
    if (use_fog_) o += "\tvec4 fogc = fetch_fog_value();\n";
    if (use_ssa_) o += "\tvec4 ssa = gl_FrontFacing ? vec4(1.) : vec4(-1.);\n";
    if (valid_) {
        o += body_;
    } else {
        o += "#if 0\n" + body_ + "#endif\n\t_fragment_discard = true;\n";
    }
    o += "}\n\nvoid main()\n{\n\tfs_main();\n\tif (_fragment_discard) discard;\n";
    for (int i = 0; i < 4; ++i)
        o += std::string("\tocol") + char('0' + i) + " = " + (fp32_out ? outs[i] : outs16[i]) + ";\n";
    // sRGB framebuffer: the ROP encodes the exports (rgb only) before quantising to 8 bits
    if (key_.srgb_output)
        for (int i = 0; i < 4; ++i) o += std::string("\tocol") + char('0' + i) + ".rgb = _linear_to_srgb(ocol" + char('0' + i) + ".rgb);\n";
    o += R"(	if (alpha_func != 0u) {
		float a = ocol0.a;
		bool pass;
		switch (alpha_func) {
		case 0x200u: pass = false; break;
		case 0x201u: pass = a < alpha_ref; break;
		case 0x202u: pass = a == alpha_ref; break;
		case 0x203u: pass = a <= alpha_ref; break;
		case 0x204u: pass = a > alpha_ref; break;
		case 0x205u: pass = a != alpha_ref; break;
		case 0x206u: pass = a >= alpha_ref; break;
		default: pass = true; break;
		}
		if (!pass) discard;
	}
	if (alpha_to_coverage != 0u) {
		// supersampled AA renders each sample as its own pixel: keep this one when alpha covers
		// its sample, sample k of n being covered for alpha > (k + 0.5) / n
		ivec2 p = ivec2(gl_FragCoord.xy * res_inv);
		float k = alpha_to_coverage == 2u ? float(p.x & 1) : alpha_to_coverage == 4u ? float[4](0., 2., 3., 1.)[(p.x & 1) + 2 * (p.y & 1)] : 0.;
		if (ocol0.a <= (k + 0.5) / float(alpha_to_coverage)) discard;
		if (alpha_to_one != 0u) ocol0.a = 1.0;
	}
)";
    if (depth) o += "\tgl_FragDepth = r1.z;\n";
    o += "}\n";
    return o;
}

}  // namespace

FpInfo fp_analyze(const u8* p, u32 max_bytes) { return scan_program(p, max_bytes).info; }

std::string fp_to_glsl(const u8* p, const FpInfo& info, const FpKey& key) {
    Scan sc = scan_program(p, info.size_bytes);
    sc.info.writes_depth = info.writes_depth;
    Decompiler d(sc, key);
    return d.run();
}

u8 fp_texture_gamma(u32 format_reg, u32 address_reg) {
    switch (tex_base_format(format_reg)) {
    case TEX_B8: case TEX_A1R5G5B5: case TEX_A4R4G4B4: case TEX_R5G6B5: case TEX_A8R8G8B8:
    case TEX_DXT1: case TEX_DXT23: case TEX_DXT45: case TEX_G8B8: case TEX_B8R8_G8R8: case TEX_R8B8_R8G8:
    case TEX_R6G5B5: case TEX_R5G5B5A1: case TEX_D1R5G5B5: case TEX_D8R8G8B8:
        return u8((address_reg >> 20) & 0xF);
    default:
        return 0;  // depth, 16-bit channel, HILO and float formats have no gamma decode
    }
}

u8 fp_texture_expand(u32 format_reg, u32 address_reg, u32 remap_reg) {
    if (((address_reg >> 12) & 0xF) != 1) return 0;
    const u32 f = tex_base_format(format_reg);
    u8 wide = 0;
    switch (f) {
    case TEX_X16: case TEX_Y16_X16: wide = 0x10; break;
    case TEX_HILO8: break;
    default:
        if (!fp_texture_gamma(format_reg, 0xF00000)) return 0;  // same 8-bit formats as gamma
    }
    const u32 remap = effective_remap(f, remap_reg);
    u8 m = 0;
    for (u32 ch = 0; ch < 4; ++ch)  // remap channels are ARGB, the mask is RGBA
        if (((remap >> (8 + 2 * ch)) & 3) >= 2) m |= u8(1u << (ch == 0 ? 3 : ch - 1));
    m &= u8(~fp_texture_gamma(format_reg, address_reg) & 0xF);
    return m ? u8(m | wide) : 0;
}

bool fp_srgb_output(u32 shader_packer, u32 surface_format, u32 shader_control) {
    const u32 color = surface_format & 0x1F;
    const bool float_target = color == 0xB || color == 0xC || color == 0xD;  // W16Z16Y16X16, W32Z32Y32X32, X32
    return shader_packer != 0 && !float_target && !(shader_control & 0x40);
}

void fp_read_constant(const u8* p, u32 byte_offset, float out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = std::bit_cast<float>(rdw(p, byte_offset + 4 * i));
}

}  // namespace rsx
