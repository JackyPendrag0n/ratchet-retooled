// RSX (NV40) vertex program microcode -> GLSL 450 translator. See rsx_vp.h.
//
// Instruction semantics follow RPCS3's VertexProgramDecompiler / GLSLCommon helpers:
//   - per-op formulas (RCP = 1/x, RCC clamped, RSQ = 1/sqrt(max(x,1e-10)), LG2 = log2(max(x,1e-10)),
//     LIT = _builtin_lit, DST, DPH, SSG = sign, FRC = fract, ADD uses src0+src2),
//   - condition codes: writes are masked per component with cc.<cond swizzle> compared against 0;
//     branch conditions use any(); cond == FL suppresses ALU writes entirely,
//   - indexed constants: vc[(a + offset) & 511], returning vec4(0) at index >= 468,
//   - BRB tests DrawState.transform_branch_bits.
// Deviations (documented): dual issue reads all sources before either op writes; EXP/LOG use the
// NV_vertex_program definitions (RPCS3 emits exp()/log()); ARL rounds down (floor); an op with
// cond-update enabled writes cc in addition to its destination; PSH/POP push/pop the address
// register; control flow is a pc-dispatch loop with a call stack and an iteration guard.
#include "rsx_vp.h"
#include "rsx_iface.h"
#include "guest.h"

#include <cstdio>
#include <string>
#include <vector>

namespace rsx {
namespace {

enum : u32 {
    REG_TEMP = 1, REG_INPUT = 2, REG_CONST = 3,
};
enum VecOp : u32 {
    V_NOP = 0, V_MOV, V_MUL, V_ADD, V_MAD, V_DP3, V_DPH, V_DP4, V_DST, V_MIN, V_MAX, V_SLT, V_SGE,
    V_ARL, V_FRC, V_FLR, V_SEQ, V_SFL, V_SGT, V_SLE, V_SNE, V_STR, V_SSG, V_TXL = 0x19,
};
enum ScaOp : u32 {
    S_NOP = 0, S_MOV, S_RCP, S_RCC, S_RSQ, S_EXP, S_LOG, S_LIT, S_BRA, S_BRI, S_CAL, S_CLI, S_RET,
    S_LG2, S_EX2, S_SIN, S_COS, S_BRB, S_CLB, S_PSH, S_POP,
};

const char* const kVecNames[32] = {
    "NOP", "MOV", "MUL", "ADD", "MAD", "DP3", "DPH", "DP4", "DST", "MIN", "MAX", "SLT", "SGE",
    "ARL", "FRC", "FLR", "SEQ", "SFL", "SGT", "SLE", "SNE", "STR", "SSG", "V17", "V18", "TXL",
    "V1A", "V1B", "V1C", "V1D", "V1E", "V1F"};
const char* const kScaNames[32] = {
    "NOP", "MOV", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT", "BRA", "BRI", "CAL", "CLI", "RET",
    "LG2", "EX2", "SIN", "COS", "BRB", "CLB", "PSH", "POP", "S15", "S16", "S17", "S18", "S19",
    "S1A", "S1B", "S1C", "S1D", "S1E", "S1F"};

// Decoded instruction (bit positions as in RPCS3's D0..D3 unions).
struct Insn {
    u32 w[4];
    // d0
    u32 addr_swz, cond_swz[4], cond, cond_test, cc_update, dst_tmp, abs[3], addr_sel, cc_sel, sat,
        index_input, vec_result;
    // d1
    u32 input_src, const_src, vec_op, sca_op;
    // d2/d3
    u32 tex_num, end, index_const, dst, sca_dst_tmp, vec_mask, sca_mask, branch_index, brb_true;
    u32 src[3];
    u32 target;  // branch/call address

    explicit Insn(const u32* p) {
        for (int i = 0; i < 4; i++) w[i] = p[i];
        const u32 d0 = w[0], d1 = w[1], d2 = w[2], d3 = w[3];
        addr_swz = d0 & 3;
        cond_swz[3] = (d0 >> 2) & 3;
        cond_swz[2] = (d0 >> 4) & 3;
        cond_swz[1] = (d0 >> 6) & 3;
        cond_swz[0] = (d0 >> 8) & 3;
        cond = (d0 >> 10) & 7;
        cond_test = (d0 >> 13) & 1;
        cc_update = ((d0 >> 14) & 1) | ((d0 >> 29) & 1);
        dst_tmp = (d0 >> 15) & 63;
        abs[0] = (d0 >> 21) & 1;
        abs[1] = (d0 >> 22) & 1;
        abs[2] = (d0 >> 23) & 1;
        addr_sel = (d0 >> 24) & 1;
        cc_sel = (d0 >> 25) & 1;
        sat = (d0 >> 26) & 1;
        index_input = (d0 >> 27) & 1;
        vec_result = (d0 >> 30) & 1;

        input_src = (d1 >> 8) & 0xf;
        const_src = (d1 >> 12) & 0x3ff;
        vec_op = (d1 >> 22) & 0x1f;
        sca_op = (d1 >> 27) & 0x1f;

        tex_num = (d2 >> 8) & 3;
        end = d3 & 1;
        index_const = (d3 >> 1) & 1;
        dst = (d3 >> 2) & 0x1f;
        sca_dst_tmp = (d3 >> 7) & 0x3f;
        // masks as bit0=x .. bit3=w
        vec_mask = ((d3 >> 16) & 1) | (((d3 >> 15) & 1) << 1) | (((d3 >> 14) & 1) << 2) |
                   (((d3 >> 13) & 1) << 3);
        sca_mask = ((d3 >> 20) & 1) | (((d3 >> 19) & 1) << 1) | (((d3 >> 18) & 1) << 2) |
                   (((d3 >> 17) & 1) << 3);
        branch_index = (d3 >> 23) & 0x1f;
        brb_true = (d3 >> 28) & 1;

        src[0] = ((d2 >> 23) & 0x1ff) | ((d1 & 0xff) << 9);
        src[1] = (d2 >> 6) & 0x1ffff;
        src[2] = ((d3 >> 21) & 0x7ff) | ((d2 & 0x3f) << 11);

        target = (((d0 >> 23) & 1) << 9) | ((d2 & 0x3f) << 3) | ((d3 >> 29) & 7);
    }

    bool is_flow() const {
        switch (sca_op) {
        case S_BRA: case S_BRI: case S_CAL: case S_CLI: case S_RET: case S_BRB: case S_CLB:
            return true;
        default:
            return false;
        }
    }
    bool is_call() const { return sca_op == S_CAL || sca_op == S_CLI || sca_op == S_CLB; }
    bool vec_valid() const { return vec_op <= V_SSG || vec_op == V_TXL; }
    bool sca_valid() const { return sca_op <= S_POP; }
    bool sca_is_alu() const {
        switch (sca_op) {
        case S_MOV: case S_RCP: case S_RCC: case S_RSQ: case S_EXP: case S_LOG: case S_LIT:
        case S_LG2: case S_EX2: case S_SIN: case S_COS:
            return true;
        default:
            return false;
        }
    }
    // Bitmask of sources (0..2) the vector op reads.
    u32 vec_srcs() const {
        switch (vec_op) {
        case V_MOV: case V_ARL: case V_FRC: case V_FLR: case V_SSG: case V_TXL: return 1;
        case V_MUL: case V_DP3: case V_DPH: case V_DP4: case V_DST: case V_MIN: case V_MAX:
        case V_SLT: case V_SGE: case V_SEQ: case V_SGT: case V_SLE: case V_SNE: return 3;
        case V_ADD: return 5;
        case V_MAD: return 7;
        default: return 0;  // NOP, SFL, STR, invalid
        }
    }
    u32 sca_srcs() const { return sca_is_alu() ? 4u : 0u; }
};

// Control-flow successors of instruction i; returns false when a target is out of range.
template <typename F>
void for_each_successor(const Insn& in, u32 i, F&& f) {
    const bool next_ok = i + 1 < kVpInstructions;
    auto fall = [&] { if (!in.end && next_ok) f(i + 1); };
    auto tgt = [&] { if (in.target < kVpInstructions) f(in.target); };
    switch (in.sca_op) {
    case S_BRA:
    case S_BRI:
        if (in.cond == 7) { tgt(); }
        else if (in.cond == 0) { fall(); }
        else { tgt(); fall(); }
        break;
    case S_BRB:
        tgt(); fall();
        break;
    case S_CAL:
    case S_CLI:
    case S_CLB:
        tgt();
        if (next_ok) f(i + 1);  // return site
        break;
    case S_RET:
        if (in.cond != 7) fall();
        break;
    default:
        fall();
        break;
    }
}

struct Walk {
    bool reach[kVpInstructions] = {};
    bool flow = false;
};

void walk(const u32* ucode, u32 start, Walk& w) {
    if (start >= kVpInstructions) return;
    u32 stack[kVpInstructions + 8];
    u32 sp = 0;
    stack[sp++] = start;
    w.reach[start] = true;
    while (sp) {
        const u32 i = stack[--sp];
        // Fast path: run linearly while there's no flow control.
        u32 cur = i;
        while (true) {
            const Insn in(ucode + cur * 4);
            if (in.is_flow()) w.flow = true;
            u32 lin = ~0u;
            for_each_successor(in, cur, [&](u32 s) {
                if (w.reach[s]) return;
                w.reach[s] = true;
                if (lin == ~0u) lin = s; else stack[sp++] = s;
            });
            if (lin == ~0u) break;
            cur = lin;
        }
    }
}

inline u64 mix64(u64 h, u64 v) {
    h ^= v * 0x9E3779B97F4A7C15ull;
    h = (h << 27) | (h >> 37);
    return h * 0xBF58476D1CE4E5B9ull + 0x94D049BB133111EBull;
}

// ---------------------------------------------------------------------------------------
struct Gen {
    const u32* ucode;
    u32 start;
    VpInfo info;  // recomputed locally so declarations always match the code
    Walk w;
    std::string body;
    bool used_r[64] = {};
    bool used_a[2] = {};
    bool used_cc[2] = {};
    bool use_lit = false, use_exp = false, use_log = false, use_idx = false, use_astack = false;
    bool pull = false;  // attributes fetched from the vertex data storage buffer (native renderer)
    u32 used_in = 0;    // input registers the generated code reads
    int indent = 1;

    Gen(const u32* u, u32 s) : ucode(u), start(s) {}

    void line(const std::string& s) {
        body.append(indent * 4, ' ');
        body += s;
        body += '\n';
    }

    static std::string mask_str(u32 m) {
        std::string s;
        for (int c = 0; c < 4; c++)
            if (m & (1u << c)) s += "xyzw"[c];
        return s;
    }

    std::string areg(const Insn& in) {
        used_a[in.addr_sel] = true;
        return std::string("a") + char('0' + in.addr_sel) + "." + "xyzw"[in.addr_swz];
    }

    std::string cc_name(const Insn& in) {
        used_cc[in.cc_sel] = true;
        return std::string("cc") + char('0' + in.cc_sel);
    }

    std::string src(const Insn& in, int n) {
        const u32 s = in.src[n];
        const u32 type = s & 3;
        std::string r;
        switch (type) {
        case REG_TEMP: {
            const u32 t = (s >> 2) & 63;
            used_r[t] = true;
            r = "r" + std::to_string(t);
            break;
        }
        case REG_INPUT:
            used_in |= 1u << in.input_src;
            r = "in_attr" + std::to_string(in.input_src);
            break;
        case REG_CONST:
            if (in.index_const) {
                use_idx = true;
                r = "vc_idx(" + areg(in) + " + " + std::to_string(in.const_src) + ")";
            } else if (in.const_src < kVpConstants) {
                r = "vc[" + std::to_string(in.const_src) + "]";
            } else {
                r = "vec4(0.0)";
            }
            break;
        default:
            r = "vec4(0.0)";  // invalid register type
            break;
        }
        const u32 sx = (s >> 14) & 3, sy = (s >> 12) & 3, sz = (s >> 10) & 3, sw = (s >> 8) & 3;
        if (!(sx == 0 && sy == 1 && sz == 2 && sw == 3)) {
            r += '.';
            r += "xyzw"[sx];
            r += "xyzw"[sy];
            r += "xyzw"[sz];
            r += "xyzw"[sw];
        }
        if (in.abs[n]) r = "abs(" + r + ")";
        if ((s >> 16) & 1) r = "(-" + r + ")";
        return r;
    }

    // bvec4 expression comparing the condition register against 0 per RPCS3's table.
    std::string cond_cmp(const Insn& in) {
        static const char* fn[8] = {"lessThan", "lessThan", "equal", "lessThanEqual",
                                    "greaterThan", "notEqual", "greaterThanEqual", "lessThan"};
        std::string sw;
        for (int c = 0; c < 4; c++) sw += "xyzw"[in.cond_swz[c]];
        std::string ccs = cc_name(in);
        if (sw != "xyzw") ccs += "." + sw;
        return std::string(fn[in.cond]) + "(" + ccs + ", vec4(0.0))";
    }

    std::string branch_cond(const Insn& in) {
        if (in.cond == 7) return "true";
        if (in.cond == 0) return "false";
        return "any(" + cond_cmp(in) + ")";
    }

    std::string brb_cond(const Insn& in) {
        return "(transform_branch_bits & " + std::to_string(1u << in.branch_index) + "u) " +
               (in.brb_true ? "!=" : "==") + " 0u";
    }

    std::string vec_expr(const Insn& in) {
        auto s = [&](int n) { return src(in, n); };
        auto p = [&](int n) { return "(" + src(in, n) + ")"; };
        switch (in.vec_op) {
        case V_MOV: return s(0);
        case V_MUL: return "(" + s(0) + " * " + s(1) + ")";
        case V_ADD: return "(" + s(0) + " + " + s(2) + ")";
        case V_MAD: return "fma(" + s(0) + ", " + s(1) + ", " + s(2) + ")";
        case V_DP3: return "vec4(dot(" + p(0) + ".xyz, " + p(1) + ".xyz))";
        case V_DPH: return "vec4(dot(vec4(" + p(0) + ".xyz, 1.0), " + s(1) + "))";
        case V_DP4: return "vec4(dot(" + s(0) + ", " + s(1) + "))";
        case V_DST: {
            const std::string a = p(0), b = p(1);
            return "vec4(1.0, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)";
        }
        case V_MIN: return "min(" + s(0) + ", " + s(1) + ")";
        case V_MAX: return "max(" + s(0) + ", " + s(1) + ")";
        case V_SLT: return "vec4(lessThan(" + s(0) + ", " + s(1) + "))";
        case V_SGE: return "vec4(greaterThanEqual(" + s(0) + ", " + s(1) + "))";
        case V_SEQ: return "vec4(equal(" + s(0) + ", " + s(1) + "))";
        case V_SGT: return "vec4(greaterThan(" + s(0) + ", " + s(1) + "))";
        case V_SLE: return "vec4(lessThanEqual(" + s(0) + ", " + s(1) + "))";
        case V_SNE: return "vec4(notEqual(" + s(0) + ", " + s(1) + "))";
        case V_SFL: return "vec4(0.0)";
        case V_STR: return "vec4(1.0)";
        case V_FRC: return "fract(" + s(0) + ")";
        case V_FLR: return "floor(" + s(0) + ")";
        case V_SSG: return "sign(" + s(0) + ")";
        case V_ARL: return "ivec4(floor(" + s(0) + "))";
        case V_TXL: return "textureLod(vtex" + std::to_string(in.tex_num) + ", " + p(0) + ".xy, 0.0)";
        default: return "";
        }
    }

    std::string sca_expr(const Insn& in) {
        const std::string s = src(in, 2);
        const std::string p = "(" + s + ")";
        switch (in.sca_op) {
        case S_MOV: return s;
        case S_RCP: return "(1.0 / " + s + ")";
        case S_RCC: return "clamp(1.0 / " + s + ", 5.42101e-20, 1.884467e19)";
        case S_RSQ: return "vec4(1.0 / sqrt(max(" + p + ".x, 1e-10)))";
        case S_EXP: use_exp = true; return "vp_exp(" + p + ".x)";
        case S_LOG: use_log = true; return "vp_log(" + p + ".x)";
        case S_LIT: use_lit = true; return "vp_lit(" + s + ")";
        case S_LG2: return "log2(max(" + s + ", vec4(1e-10)))";
        case S_EX2: return "exp2(" + s + ")";
        case S_SIN: return "sin(" + s + ")";
        case S_COS: return "cos(" + s + ")";
        default: return "";
        }
    }

    // Emits "lhs.mask = value.mask" (or the conditional mix form).
    void write(const std::string& lhs, u32 mask, const std::string& val, bool cond) {
        if (!mask) return;
        const std::string m = mask == 0xf ? "" : "." + mask_str(mask);
        if (cond)
            line(lhs + m + " = mix(" + lhs + m + ", " + val + m + ", cnd" + m + ");");
        else
            line(lhs + m + " = " + val + m + ";");
    }

    static std::string hex8(u32 v) {
        char b[16];
        std::snprintf(b, sizeof b, "%08x", v);
        return b;
    }

    // Emits the ALU part (and PSH/POP) of one instruction.
    void emit_alu(const Insn& in, u32 idx) {
        std::string c = "// " + std::to_string(idx) + ": " + hex8(in.w[0]) + " " + hex8(in.w[1]) +
                        " " + hex8(in.w[2]) + " " + hex8(in.w[3]) + "  " + kVecNames[in.vec_op] +
                        " / " + kScaNames[in.sca_op];
        if (in.end) c += " END";
        line(c);
        if (!in.vec_valid()) line("// unknown vector opcode, treated as NOP");
        if (!in.sca_valid()) line("// unknown scalar opcode, treated as NOP");

        const bool vec = in.vec_op != V_NOP && in.vec_valid();
        const bool sca = in.sca_is_alu();
        const bool psh = in.sca_op == S_PSH, pop = in.sca_op == S_POP;
        if (!vec && !sca && !psh && !pop) return;

        // cond == FL: RPCS3 drops the ALU write entirely.
        const bool writes = in.cond != 0;
        if (!writes && !psh && !pop) {
            line("// condition FL: ALU writes suppressed");
            return;
        }
        const bool cond = writes && in.cond_test && in.cond != 7;

        line("{");
        indent++;
        if (cond && (vec || sca)) line("bvec4 cnd = " + cond_cmp(in) + ";");
        const std::string satl = in.sat ? "clamp(" : "", satr = in.sat ? ", 0.0, 1.0)" : "";
        const bool arl = vec && in.vec_op == V_ARL;
        if (vec) {
            if (arl) line("ivec4 av = " + vec_expr(in) + ";");
            else line("vec4 vv = " + satl + vec_expr(in) + satr + ";");
        }
        if (sca) line("vec4 sv = " + satl + "vec4(" + sca_expr(in) + ")" + satr + ";");

        if (writes) {
            if (vec) {
                if (arl) {
                    if (in.dst_tmp < 2) {
                        used_a[in.dst_tmp] = true;
                        write(std::string("a") + char('0' + in.dst_tmp), in.vec_mask, "av", cond);
                    }
                } else {
                    const bool out = in.vec_result && in.dst != 0x1f && in.dst < 16;
                    const bool tmp = in.dst_tmp != 0x3f;
                    if (out) write("dst_reg" + std::to_string(in.dst), in.vec_mask, "vv", cond);
                    if (tmp) {
                        used_r[in.dst_tmp] = true;
                        write("r" + std::to_string(in.dst_tmp), in.vec_mask, "vv", cond);
                    }
                    if (in.cc_update || (!out && !tmp && !(in.vec_result && in.dst != 0x1f)))
                        write(cc_name(in), in.vec_mask, "vv", cond);
                }
            }
            if (sca) {
                const bool out = !in.vec_result && in.dst != 0x1f && in.dst < 16;
                const bool tmp = in.sca_dst_tmp != 0x3f;
                if (out) write("dst_reg" + std::to_string(in.dst), in.sca_mask, "sv", cond);
                if (tmp) {
                    used_r[in.sca_dst_tmp] = true;
                    write("r" + std::to_string(in.sca_dst_tmp), in.sca_mask, "sv", cond);
                }
                if (in.cc_update || (!out && !tmp && !(!in.vec_result && in.dst != 0x1f)))
                    write(cc_name(in), in.sca_mask, "sv", cond);
            }
        }
        if (psh || pop) {
            use_astack = true;
            const std::string a = std::string("a") + char('0' + in.addr_sel);
            used_a[in.addr_sel] = true;
            if (psh) line("if (asp < 8) { astack[asp] = " + a + "; asp++; }");
            else line("if (asp > 0) { asp--; " + a + " = astack[asp]; }");
        }
        indent--;
        line("}");
    }

    std::string gen_straight() {
        for (u32 i = start; i < kVpInstructions; i++) {
            const Insn in(ucode + i * 4);
            emit_alu(in, i);
            if (in.end) break;
        }
        return body;
    }

    // Control flow: pc-dispatch loop over basic blocks.
    std::string gen_dispatch() {
        bool leader[kVpInstructions] = {};
        leader[start] = true;
        for (u32 i = 0; i < kVpInstructions; i++) {
            if (!w.reach[i]) continue;
            const Insn in(ucode + i * 4);
            if (!in.is_flow()) continue;
            if (in.target < kVpInstructions) leader[in.target] = true;
            if (i + 1 < kVpInstructions) leader[i + 1] = true;
        }
        line("int pc = " + std::to_string(start) + ";");
        line("int csp = 0;");
        line("int cstack[8];");
        line("for (uint guard = 0u; pc >= 0 && guard < 65536u; guard++)");
        line("{");
        indent++;
        line("switch (pc)");
        line("{");
        for (u32 l = 0; l < kVpInstructions; l++) {
            if (!leader[l] || !w.reach[l]) continue;
            line("case " + std::to_string(l) + ":");
            line("{");
            indent++;
            for (u32 i = l;; i++) {
                const Insn in(ucode + i * 4);
                emit_alu(in, i);
                const std::string next = (!in.end && i + 1 < kVpInstructions) ? std::to_string(i + 1) : "-1";
                const std::string tgt = in.target < kVpInstructions ? std::to_string(in.target) : "-1";
                if (in.is_flow()) {
                    switch (in.sca_op) {
                    case S_BRA:
                    case S_BRI:
                        line("pc = (" + branch_cond(in) + ") ? " + tgt + " : " + next + ";");
                        break;
                    case S_BRB:
                        line("pc = (" + brb_cond(in) + ") ? " + tgt + " : " + next + ";");
                        break;
                    case S_CAL:
                    case S_CLI:
                    case S_CLB: {
                        const std::string c = in.sca_op == S_CLB ? brb_cond(in) : branch_cond(in);
                        const std::string ret = i + 1 < kVpInstructions ? std::to_string(i + 1) : "-1";
                        line("if (" + c + ")");
                        line("{");
                        line("    if (csp < 8) { cstack[csp] = " + ret + "; csp++; }");
                        line("    pc = " + tgt + ";");
                        line("}");
                        line("else pc = " + next + ";");
                        break;
                    }
                    case S_RET:
                        line("if (" + branch_cond(in) + ")");
                        line("{");
                        line("    if (csp > 0) { csp--; pc = cstack[csp]; } else pc = -1;");
                        line("}");
                        line("else pc = " + next + ";");
                        break;
                    }
                    break;
                }
                if (in.end || i + 1 >= kVpInstructions) {
                    line("pc = -1;");
                    break;
                }
                if (leader[i + 1]) {
                    line("pc = " + next + ";");
                    break;
                }
            }
            line("break;");
            indent--;
            line("}");
        }
        line("default:");
        line("    pc = -1;");
        line("    break;");
        line("}");
        indent--;
        line("}");
        return body;
    }

    std::string run() {
        walk(ucode, start, w);
        std::string code = info.has_flow_control || w.flow ? gen_dispatch() : gen_straight();

        std::string o;
        o += "#version 450\n\n";
        if (pull) {
            o += kVertexPullGlsl;
            for (int i = 0; i < 16; i++) o += "vec4 in_attr" + std::to_string(i) + ";\n";
        } else {
            for (int i = 0; i < 16; i++)
                o += "layout(location = " + std::to_string(i) + ") in vec4 in_attr" + std::to_string(i) + ";\n";
        }
        o += "\n";
        static const char* outs[15] = {"diff_color", "spec_color", "diff_color1", "spec_color1", "fog_c",
                                       "tc0", "tc1", "tc2", "tc3", "tc4", "tc5", "tc6", "tc7", "tc8", "tc9"};
        for (int i = 0; i < 15; i++)
            o += "layout(location = " + std::to_string(i) + ") out vec4 " + outs[i] + ";\n";
        o += "\nlayout(std140, set = 0, binding = " + std::to_string(kVertexConstBinding) +
             ") uniform VertexConstants { vec4 vc[" + std::to_string(kVpConstants) + "]; };\n";
        o += kDrawStateGlsl;
        o += "\n";
        for (u32 t = 0; t < 4; t++)
            if (info.vertex_textures & (1u << t))
                o += "layout(set = 0, binding = " + std::to_string(kVertexTextureBindingBase + t) +
                     ") uniform sampler2D vtex" + std::to_string(t) + ";\n";
        if (use_idx)
            o += "vec4 vc_idx(int i) { i &= 511; return i < " + std::to_string(kVpConstants) +
                 " ? vc[i] : vec4(0.0); }\n";
        if (use_lit)
            o += "vec4 vp_lit(vec4 v)\n{\n"
                 "    vec4 t = vec4(max(v.xy, vec2(0.0, 1e-10)), v.zw);\n"
                 "    return vec4(1.0, t.x, t.x > 0.0 ? exp2(t.w * log2(t.y)) : 0.0, 1.0);\n}\n";
        if (use_exp)
            o += "vec4 vp_exp(float x)\n{\n"
                 "    float f = floor(x);\n"
                 "    return vec4(exp2(f), x - f, exp2(x), 1.0);\n}\n";
        if (use_log)
            o += "vec4 vp_log(float x)\n{\n"
                 "    float a = abs(x);\n"
                 "    if (a == 0.0) return vec4(uintBitsToFloat(0xff800000u), 1.0, uintBitsToFloat(0xff800000u), 1.0);\n"
                 "    float e = floor(log2(a));\n"
                 "    return vec4(e, a * exp2(-e), log2(a), 1.0);\n}\n";
        o += "\nvoid main()\n{\n";
        if (pull)
            for (int i = 0; i < 16; i++)
                o += "    in_attr" + std::to_string(i) + (((info.inputs_read | used_in) >> i) & 1 ? " = fetch_attr(" + std::to_string(i) + "u);\n" : " = vec4(0.0, 0.0, 0.0, 1.0);\n");
        for (int i = 0; i < 16; i++)
            o += "    vec4 dst_reg" + std::to_string(i) + " = vec4(0.0, 0.0, 0.0, 1.0);\n";
        for (int i = 0; i < 64; i++)
            if (used_r[i]) o += "    vec4 r" + std::to_string(i) + " = vec4(0.0);\n";
        for (int i = 0; i < 2; i++)
            if (used_a[i]) o += "    ivec4 a" + std::to_string(i) + " = ivec4(0);\n";
        for (int i = 0; i < 2; i++)
            if (used_cc[i]) o += "    vec4 cc" + std::to_string(i) + " = vec4(0.0);\n";
        if (use_astack) o += "    ivec4 astack[8];\n    int asp = 0;\n";
        o += "\n";
        o += code;
        o += "\n";
        for (int i = 0; i < 4; i++)
            o += std::string("    ") + outs[i] + " = dst_reg" + std::to_string(i + 1) + ";\n";
        o += "    fog_c = dst_reg5.xxxx;\n";
        for (int i = 0; i < 9; i++)
            o += "    tc" + std::to_string(i) + " = dst_reg" + std::to_string(i + 7) + ";\n";
        o += "    tc9 = dst_reg6;\n";
        o += (info.outputs_written & (1u << 6)) ? "    gl_PointSize = dst_reg6.x;\n"
                                                : "    gl_PointSize = 1.0;\n";
        o += "    gl_Position = apply_viewport(dst_reg0);\n";
        o += "}\n";
        return o;
    }
};

}  // namespace

VpInfo vp_analyze(const u32* ucode, u32 start) {
    VpInfo info;
    if (start >= kVpInstructions) return info;
    Walk w;
    walk(ucode, start, w);
    info.has_flow_control = w.flow;
    u64 h = mix64(0x5250435333565055ull, start);
    for (u32 i = 0; i < kVpInstructions; i++) {
        if (!w.reach[i]) continue;
        const u32* p = ucode + i * 4;
        h = mix64(h, i);
        h = mix64(h, (u64(p[0]) << 32) | p[1]);
        h = mix64(h, (u64(p[2]) << 32) | p[3]);
        info.instruction_count++;

        const Insn in(p);
        const bool vec = in.vec_op != V_NOP && in.vec_valid();
        const bool sca = in.sca_is_alu();
        const u32 srcs = (vec ? in.vec_srcs() : 0) | in.sca_srcs();
        for (int n = 0; n < 3; n++)
            if ((srcs & (1u << n)) && (in.src[n] & 3) == REG_INPUT) info.inputs_read |= 1u << in.input_src;
        if (vec && in.vec_op == V_TXL) info.vertex_textures |= 1u << in.tex_num;
        if (in.cond != 0 && in.dst != 0x1f && in.dst < 16) {
            if ((vec && in.vec_op != V_ARL && in.vec_result && in.vec_mask) ||
                (sca && !in.vec_result && in.sca_mask))
                info.outputs_written |= 1u << in.dst;
        }
    }
    info.uses_vertex_textures = info.vertex_textures != 0;
    h ^= h >> 31;
    info.hash = h ? h : 1;
    return info;
}

std::string vp_to_glsl(const u32* ucode, u32 start, const VpInfo& /*info*/, bool pull_vertices) {
    if (start >= kVpInstructions) start = 0;
    Gen g(ucode, start);
    g.pull = pull_vertices;
    g.info = vp_analyze(ucode, start);
    std::string r = g.run();
    if (g.used_in & ~g.info.inputs_read)
        LOG_NOTE("rsx: vertex program %016llx reads inputs %04x that analysis missed",
                 (unsigned long long)g.info.hash, g.used_in & ~g.info.inputs_read);
    return r;
}

}  // namespace rsx
