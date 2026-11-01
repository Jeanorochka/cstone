#include "internal.h"
#include <string.h>

static int wide_arch(cstone_i_context *c) {
    return c->options.arch == CSTONE_MIPS64_LE || c->options.arch == CSTONE_MIPS64_BE;
}
static int reg_get(cstone_i_context *c, cstone_i_span s, unsigned *reg) {
    static const char *const names32[] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                          "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                          "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    static const char *const names64[] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "a4", "a5", "a6",
                                          "a7",   "t0", "t1", "t2", "t3", "s0", "s1", "s2", "s3", "s4", "s5",
                                          "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    const char *const *names = wide_arch(c) ? names64 : names32;
    unsigned k, n = 0;
    size_t p;
    if (s.n && s.p[0] == '$')
        s = cstone_i_slice(s, 1, s.n - 1);
    for (k = 0; k < 32; ++k)
        if (cstone_i_equal(s, names[k])) {
            *reg = k;
            return 1;
        }
    if (cstone_i_equal(s, "s8")) {
        *reg = 30;
        return 1;
    }
    if (!s.n)
        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, s.col, "Empty MIPS register.");
    for (p = 0; p < s.n; ++p) {
        if (s.p[p] < '0' || s.p[p] > '9')
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, s.col, "Unknown MIPS register.");
        n = n * 10u + (unsigned)(s.p[p] - '0');
        if (n > 31)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, s.col, "MIPS register exceeds 31.");
    }
    *reg = n;
    return 1;
}
static int memory(cstone_i_context *c, cstone_i_span s, unsigned *base, int64_t *offset) {
    size_t open = 0;
    while (open < s.n && s.p[open] != '(')
        ++open;
    if (open == s.n || s.p[s.n - 1] != ')')
        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, s.col, "Expected offset(base).");
    if (!reg_get(c, cstone_i_trim(cstone_i_slice(s, open + 1, s.n - open - 2)), base))
        return 0;
    *offset = 0;
    return open == 0 || cstone_i_signed(c, cstone_i_trim(cstone_i_slice(s, 0, open)), 16, offset);
}
static int emit_li(cstone_i_context *c, unsigned rd, cstone_i_span text, size_t column) {
    cstone_i_num number;
    uint64_t value;
    unsigned k, first = 1;
    if (!cstone_i_number(c, text, &number))
        return 0;
    if (!wide_arch(c) && number.magnitude > (number.negative ? UINT64_C(0x80000000) : UINT64_C(0xffffffff)))
        return cstone_i_fail(c, CSTONE_RANGE_ERROR, text.col, "MIPS32 immediate exceeds 32 bits.");
    value = cstone_i_bits(number);
    if (!wide_arch(c)) {
        uint32_t v = (uint32_t)value;
        if (v <= 32767u || v >= 0xffff8000u)
            return cstone_i_emit_word(c, 0x24000000u | (rd << 16) | (v & 65535u), column);
        if (v <= 65535)
            return cstone_i_emit_word(c, 0x34000000u | (rd << 16) | v, column);
        if (!cstone_i_emit_word(c, 0x3c000000u | (rd << 16) | (v >> 16), column))
            return 0;
        return !(v & 65535u) ||
               cstone_i_emit_word(c, 0x34000000u | (rd << 21) | (rd << 16) | (v & 65535u), column);
    }
    for (k = 4; k > 0; --k) {
        uint32_t part = (uint32_t)(value >> ((k - 1) * 16)) & 65535u;
        if (first && !part && k > 1)
            continue;
        if (!first && !cstone_i_emit_word(c, (rd << 16) | (rd << 11) | (16u << 6) | 0x38u, column))
            return 0;
        if (part || first) {
            if (!cstone_i_emit_word(c, 0x34000000u | (first ? 0u : rd << 21) | (rd << 16) | part, column))
                return 0;
        }
        first = 0;
    }
    return 1;
}
int cstone_i_mips(cstone_i_context *c, const cstone_i_insn *i) {
    static const struct {
        const char *name;
        unsigned funct;
        int wide;
    } rtypes[] = {{"add", 32, 0},  {"addu", 33, 0},  {"sub", 34, 0},  {"subu", 35, 0}, {"and", 36, 0},
                  {"or", 37, 0},   {"xor", 38, 0},   {"nor", 39, 0},  {"slt", 42, 0},  {"sltu", 43, 0},
                  {"dadd", 44, 1}, {"daddu", 45, 1}, {"dsub", 46, 1}, {"dsubu", 47, 1}};
    static const struct {
        const char *name;
        unsigned opcode;
        int logical, wide;
    } immediates[] = {{"addi", 8, 0, 0},   {"addiu", 9, 0, 0},  {"slti", 10, 0, 0},
                      {"sltiu", 11, 0, 0}, {"andi", 12, 1, 0},  {"ori", 13, 1, 0},
                      {"xori", 14, 1, 0},  {"daddi", 24, 0, 1}, {"daddiu", 25, 0, 1}};
    unsigned a = 0, b = 0, d = 0, k;
    uint64_t value = 0;
    int64_t signed_value = 0;
    uint32_t word;
    if (cstone_i_equal(i->name, "nop"))
        return cstone_i_count(c, i, 0) && cstone_i_emit_word(c, 0, i->name.col);
    if (cstone_i_equal(i->name, "li") || cstone_i_equal(i->name, "dli")) {
        if (cstone_i_equal(i->name, "dli") && !wide_arch(c))
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "DLI requires MIPS64.");
        return cstone_i_count(c, i, 2) && reg_get(c, i->op[0], &a) && emit_li(c, a, i->op[1], i->name.col);
    }
    if (cstone_i_equal(i->name, "move") || cstone_i_equal(i->name, "not") || cstone_i_equal(i->name, "neg") ||
        cstone_i_equal(i->name, "negu")) {
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b))
            return 0;
        word = cstone_i_equal(i->name, "move") ? 0x25u | (b << 21)
               : cstone_i_equal(i->name, "not")
                   ? 0x27u | (b << 21)
                   : (cstone_i_equal(i->name, "neg") ? 0x22u : 0x23u) | (b << 16);
        return cstone_i_emit_word(c, word | (a << 11), i->name.col);
    }
    if (cstone_i_equal(i->name, "jr") || cstone_i_equal(i->name, "jalr")) {
        if (cstone_i_equal(i->name, "jr")) {
            if (!cstone_i_count(c, i, 1) || !reg_get(c, i->op[0], &b))
                return 0;
            word = 8;
        } else {
            a = 31;
            if (i->count == 1) {
                if (!reg_get(c, i->op[0], &b))
                    return 0;
            } else if (i->count == 2) {
                if (!reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b))
                    return 0;
            } else
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Invalid JALR operands.");
            word = 9u | (a << 11);
        }
        return cstone_i_emit_word(c, word | (b << 21), i->name.col);
    }
    if (cstone_i_equal(i->name, "j") || cstone_i_equal(i->name, "jal")) {
        size_t offset = c->out->size;
        return cstone_i_count(c, i, 1) &&
               cstone_i_emit_word(c, cstone_i_equal(i->name, "jal") ? 0x0c000000u : 0x08000000u,
                                  i->name.col) &&
               cstone_i_reference(c, i->op[0], CSTONE_MIPS_JUMP26, offset, 4, 0);
    }
    if (cstone_i_equal(i->name, "beq") || cstone_i_equal(i->name, "bne") || cstone_i_equal(i->name, "b") ||
        cstone_i_equal(i->name, "bal")) {
        size_t offset = c->out->size;
        cstone_i_span target;
        if (cstone_i_equal(i->name, "b") || cstone_i_equal(i->name, "bal")) {
            if (!cstone_i_count(c, i, 1))
                return 0;
            target = i->op[0];
            word = cstone_i_equal(i->name, "bal") ? 0x04110000u : 0x10000000u;
        } else {
            if (!cstone_i_count(c, i, 3) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b))
                return 0;
            target = i->op[2];
            word = (cstone_i_equal(i->name, "beq") ? 0x10000000u : 0x14000000u) | (a << 21) | (b << 16);
        }
        return cstone_i_emit_word(c, word, i->name.col) &&
               cstone_i_reference(c, target, CSTONE_MIPS_BRANCH16, offset, 4, 0);
    }
    if (cstone_i_equal(i->name, "bltz") || cstone_i_equal(i->name, "bgez") ||
        cstone_i_equal(i->name, "bltzal") || cstone_i_equal(i->name, "bgezal") ||
        cstone_i_equal(i->name, "blez") || cstone_i_equal(i->name, "bgtz")) {
        size_t offset = c->out->size;
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a))
            return 0;
        word = cstone_i_equal(i->name, "blez")   ? 0x18000000u
               : cstone_i_equal(i->name, "bgtz") ? 0x1c000000u
                                                 : 0x04000000u;
        if (cstone_i_equal(i->name, "bgez") || cstone_i_equal(i->name, "bgezal"))
            word |= 1u << 16;
        if (cstone_i_equal(i->name, "bltzal") || cstone_i_equal(i->name, "bgezal"))
            word |= 16u << 16;
        return cstone_i_emit_word(c, word | (a << 21), i->name.col) &&
               cstone_i_reference(c, i->op[1], CSTONE_MIPS_BRANCH16, offset, 4, 0);
    }
    if (cstone_i_equal(i->name, "syscall")) {
        if (i->count > 1)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "SYSCALL takes an optional code.");
        if (i->count && !cstone_i_unsigned(c, i->op[0], 20, &value))
            return 0;
        return cstone_i_emit_word(c, 0xcu | ((uint32_t)value << 6), i->name.col);
    }
    if (cstone_i_equal(i->name, "break")) {
        uint64_t second = 0;
        if (i->count > 2)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                 "BREAK takes up to two 10-bit codes.");
        if (i->count && !cstone_i_unsigned(c, i->op[0], 10, &value))
            return 0;
        if (i->count == 2 && !cstone_i_unsigned(c, i->op[1], 10, &second))
            return 0;
        return cstone_i_emit_word(c, 0xdu | ((uint32_t)value << 16) | ((uint32_t)second << 6), i->name.col);
    }
    if (cstone_i_equal(i->name, "sync")) {
        if (i->count > 1)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                 "SYNC takes an optional 5-bit type.");
        if (i->count && !cstone_i_unsigned(c, i->op[0], 5, &value))
            return 0;
        return cstone_i_emit_word(c, 0xfu | ((uint32_t)value << 6), i->name.col);
    }
    if (cstone_i_equal(i->name, "mfhi") || cstone_i_equal(i->name, "mflo") ||
        cstone_i_equal(i->name, "mthi") || cstone_i_equal(i->name, "mtlo")) {
        if (!cstone_i_count(c, i, 1) || !reg_get(c, i->op[0], &a))
            return 0;
        word = cstone_i_equal(i->name, "mfhi")   ? 0x10u | (a << 11)
               : cstone_i_equal(i->name, "mflo") ? 0x12u | (a << 11)
               : cstone_i_equal(i->name, "mthi") ? 0x11u | (a << 21)
                                                 : 0x13u | (a << 21);
        return cstone_i_emit_word(c, word, i->name.col);
    }
    for (k = 0; k < sizeof(rtypes) / sizeof(rtypes[0]); ++k)
        if (cstone_i_equal(i->name, rtypes[k].name)) {
            if (rtypes[k].wide && !wide_arch(c))
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Instruction requires MIPS64.");
            if (!cstone_i_count(c, i, 3) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b) ||
                !reg_get(c, i->op[2], &d))
                return 0;
            return cstone_i_emit_word(c, rtypes[k].funct | (a << 11) | (b << 21) | (d << 16), i->name.col);
        }
    for (k = 0; k < sizeof(immediates) / sizeof(immediates[0]); ++k)
        if (cstone_i_equal(i->name, immediates[k].name)) {
            if (immediates[k].wide && !wide_arch(c))
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Instruction requires MIPS64.");
            if (!cstone_i_count(c, i, 3) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b))
                return 0;
            if (immediates[k].logical) {
                if (!cstone_i_unsigned(c, i->op[2], 16, &value))
                    return 0;
                signed_value = (int64_t)value;
            } else if (!cstone_i_signed(c, i->op[2], 16, &signed_value))
                return 0;
            return cstone_i_emit_word(
                c, (immediates[k].opcode << 26) | (b << 21) | (a << 16) | ((uint32_t)signed_value & 65535u),
                i->name.col);
        }
    if (cstone_i_equal(i->name, "lui")) {
        cstone_i_num number;
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a) || !cstone_i_number(c, i->op[1], &number))
            return 0;
        if (number.magnitude > (number.negative ? 32768u : 65535u))
            return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[1].col, "LUI immediate exceeds 16 bits.");
        return cstone_i_emit_word(c, 0x3c000000u | (a << 16) | ((uint32_t)cstone_i_bits(number) & 65535u),
                                  i->name.col);
    }
    {
        static const struct {
            const char *name;
            unsigned funct;
            int variable, wide;
        } shifts[] = {{"sll", 0, 0, 0},    {"srl", 2, 0, 0},     {"sra", 3, 0, 0},     {"sllv", 4, 1, 0},
                      {"srlv", 6, 1, 0},   {"srav", 7, 1, 0},    {"dsll", 56, 0, 1},   {"dsrl", 58, 0, 1},
                      {"dsra", 59, 0, 1},  {"dsll32", 60, 0, 1}, {"dsrl32", 62, 0, 1}, {"dsra32", 63, 0, 1},
                      {"dsllv", 20, 1, 1}, {"dsrlv", 22, 1, 1},  {"dsrav", 23, 1, 1}};
        for (k = 0; k < sizeof(shifts) / sizeof(shifts[0]); ++k)
            if (cstone_i_equal(i->name, shifts[k].name)) {
                if (shifts[k].wide && !wide_arch(c))
                    return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Shift requires MIPS64.");
                if (!cstone_i_count(c, i, 3) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b))
                    return 0;
                if (shifts[k].variable) {
                    if (!reg_get(c, i->op[2], &d))
                        return 0;
                    word = d << 21;
                } else {
                    if (!cstone_i_unsigned(c, i->op[2], 5, &value))
                        return 0;
                    word = (uint32_t)value << 6;
                }
                return cstone_i_emit_word(c, word | shifts[k].funct | (a << 11) | (b << 16), i->name.col);
            }
    }
    {
        static const struct {
            const char *name;
            unsigned opcode;
            int wide;
        } loads[] = {{"lb", 32, 0},  {"lh", 33, 0}, {"lw", 35, 0}, {"lbu", 36, 0}, {"lhu", 37, 0},
                     {"lwu", 39, 1}, {"ld", 55, 1}, {"sb", 40, 0}, {"sh", 41, 0},  {"sw", 43, 0},
                     {"sd", 63, 1},  {"ll", 48, 0}, {"sc", 56, 0}, {"lld", 52, 1}, {"scd", 60, 1}};
        for (k = 0; k < sizeof(loads) / sizeof(loads[0]); ++k)
            if (cstone_i_equal(i->name, loads[k].name)) {
                if (loads[k].wide && !wide_arch(c))
                    return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Transfer requires MIPS64.");
                if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a) ||
                    !memory(c, i->op[1], &b, &signed_value))
                    return 0;
                return cstone_i_emit_word(
                    c, (loads[k].opcode << 26) | (b << 21) | (a << 16) | ((uint32_t)signed_value & 65535u),
                    i->name.col);
            }
    }
    if (cstone_i_equal(i->name, "mul")) {
        if (!cstone_i_count(c, i, 3) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b) ||
            !reg_get(c, i->op[2], &d))
            return 0;
        return cstone_i_emit_word(c, 0x70000002u | (a << 11) | (b << 21) | (d << 16), i->name.col);
    }
    {
        static const char *const names[] = {"mult",  "multu",  "div",  "divu",
                                            "dmult", "dmultu", "ddiv", "ddivu"};
        for (k = 0; k < 8; ++k)
            if (cstone_i_equal(i->name, names[k])) {
                size_t first = 0;
                if (k >= 4 && !wide_arch(c))
                    return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                         "Instruction requires MIPS64.");
                if (i->count == 3 && (k == 2 || k == 3 || k == 6 || k == 7)) {
                    if (!reg_get(c, i->op[0], &a))
                        return 0;
                    if (a)
                        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col,
                                             "Architectural DIV destination is $zero; result is HI/LO.");
                    first = 1;
                } else if (!cstone_i_count(c, i, 2))
                    return 0;
                if (!reg_get(c, i->op[first], &a) || !reg_get(c, i->op[first + 1], &b))
                    return 0;
                return cstone_i_emit_word(c, (24u + k) | (a << 21) | (b << 16), i->name.col);
            }
    }
    return cstone_i_fail(c, CSTONE_UNSUPPORTED_INSTRUCTION, i->name.col,
                         "Unsupported MIPS instruction '%.*s'.", (int)(i->name.n > 48 ? 48 : i->name.n),
                         i->name.p);
}
