#include "internal.h"
#include <string.h>

static int reg_name(cstone_i_span s, unsigned *reg) {
    static const char *const names[] = {"zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0",
                                        "a1",   "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
                                        "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
    unsigned k, n = 0;
    size_t p;
    for (k = 0; k < 32; ++k)
        if (cstone_i_equal(s, names[k])) {
            *reg = k;
            return 1;
        }
    if (cstone_i_equal(s, "fp")) {
        *reg = 8;
        return 1;
    }
    if (s.n < 2 || (s.p[0] != 'x' && s.p[0] != 'X'))
        return 0;
    for (p = 1; p < s.n; ++p) {
        if (s.p[p] < '0' || s.p[p] > '9')
            return 0;
        n = n * 10u + (unsigned)(s.p[p] - '0');
        if (n > 31)
            return 0;
    }
    *reg = n;
    return 1;
}
static int reg_get(cstone_i_context *c, cstone_i_span s, unsigned *reg) {
    return reg_name(s, reg) || cstone_i_fail(c, CSTONE_INVALID_OPERAND, s.col, "Expected a RISC-V register.");
}
static uint32_t itype(unsigned opcode, unsigned rd, unsigned rs, unsigned funct, int64_t value) {
    return opcode | (rd << 7) | (funct << 12) | (rs << 15) | (((uint32_t)value & 4095u) << 20);
}
static int mem(cstone_i_context *c, cstone_i_span s, unsigned *base, int64_t *offset) {
    size_t open = 0;
    while (open < s.n && s.p[open] != '(')
        ++open;
    if (open == s.n || s.p[s.n - 1] != ')')
        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, s.col, "Expected offset(base).");
    if (!reg_get(c, cstone_i_trim(cstone_i_slice(s, open + 1, s.n - open - 2)), base))
        return 0;
    *offset = 0;
    return open == 0 || cstone_i_signed(c, cstone_i_trim(cstone_i_slice(s, 0, open)), 12, offset);
}
static int emit_li(cstone_i_context *c, unsigned rd, int64_t value, int wide, size_t column) {
    int64_t low = (int64_t)((uint64_t)value & 4095u);
    int64_t hi;
    if (low >= 2048)
        low -= 4096;
    if (value >= -2048 && value <= 2047)
        return cstone_i_emit_word(c, itype(0x13, rd, 0, 0, value), column);
    hi = value / 4096 + (value % 4096 - low) / 4096;
    if (value >= INT32_MIN && value <= INT32_MAX) {
        if (!cstone_i_emit_word(c, 0x37u | (rd << 7) | (((uint32_t)hi & 0xfffffu) << 12), column))
            return 0;
        return !low || cstone_i_emit_word(c, itype(wide ? 0x1bu : 0x13u, rd, rd, 0, low), column);
    }
    if (!emit_li(c, rd, hi, wide, column) || !cstone_i_emit_word(c, itype(0x13, rd, rd, 1, 12), column))
        return 0;
    return !low || cstone_i_emit_word(c, itype(0x13, rd, rd, 0, low), column);
}
static int address_pair(cstone_i_context *c, const cstone_i_insn *i, unsigned temporary, unsigned result,
                        cstone_i_span target, int call) {
    size_t offset = c->out->size;
    uint64_t anchor = c->options.base_address + (uint64_t)offset;
    if (!cstone_i_emit_word(c, 0x17u | (temporary << 7), i->name.col) ||
        !cstone_i_emit_word(c, itype(call ? 0x67u : 0x13u, result, temporary, 0, 0), i->name.col))
        return 0;
    return cstone_i_reference(c, target, CSTONE_RV_PCREL_HI20, offset, 0, anchor) &&
           cstone_i_reference(c, target, CSTONE_RV_PCREL_LO12, offset + 4, 0, anchor);
}
static int csr(cstone_i_context *c, cstone_i_span s, uint64_t *value) {
    static const struct {
        const char *name;
        unsigned value;
    } names[] = {{"fflags", 1},       {"frm", 2},         {"fcsr", 3},         {"cycle", 0xc00},
                 {"time", 0xc01},     {"instret", 0xc02}, {"cycleh", 0xc80},   {"timeh", 0xc81},
                 {"instreth", 0xc82}, {"sstatus", 0x100}, {"sie", 0x104},      {"stvec", 0x105},
                 {"sscratch", 0x140}, {"sepc", 0x141},    {"scause", 0x142},   {"stval", 0x143},
                 {"sip", 0x144},      {"satp", 0x180},    {"mstatus", 0x300},  {"misa", 0x301},
                 {"mie", 0x304},      {"mtvec", 0x305},   {"mscratch", 0x340}, {"mepc", 0x341},
                 {"mcause", 0x342},   {"mtval", 0x343},   {"mip", 0x344}};
    unsigned k;
    for (k = 0; k < sizeof(names) / sizeof(names[0]); ++k)
        if (cstone_i_equal(s, names[k].name)) {
            *value = names[k].value;
            return 1;
        }
    return cstone_i_unsigned(c, s, 12, value);
}
int cstone_i_riscv(cstone_i_context *c, const cstone_i_insn *i) {
    static const struct {
        const char *name;
        unsigned funct, top;
        int word;
    } rtypes[] = {{"add", 0, 0, 0},    {"sub", 0, 32, 0},  {"sll", 1, 0, 0},   {"slt", 2, 0, 0},
                  {"sltu", 3, 0, 0},   {"xor", 4, 0, 0},   {"srl", 5, 0, 0},   {"sra", 5, 32, 0},
                  {"or", 6, 0, 0},     {"and", 7, 0, 0},   {"mul", 0, 1, 0},   {"mulh", 1, 1, 0},
                  {"mulhsu", 2, 1, 0}, {"mulhu", 3, 1, 0}, {"div", 4, 1, 0},   {"divu", 5, 1, 0},
                  {"rem", 6, 1, 0},    {"remu", 7, 1, 0},  {"addw", 0, 0, 1},  {"subw", 0, 32, 1},
                  {"sllw", 1, 0, 1},   {"srlw", 5, 0, 1},  {"sraw", 5, 32, 1}, {"mulw", 0, 1, 1},
                  {"divw", 4, 1, 1},   {"divuw", 5, 1, 1}, {"remw", 6, 1, 1},  {"remuw", 7, 1, 1}};
    static const struct {
        const char *name;
        unsigned funct;
    } itypes[] = {{"addi", 0}, {"slti", 2}, {"sltiu", 3}, {"xori", 4}, {"ori", 6}, {"andi", 7}};
    unsigned a = 0, b = 0, d = 0, k;
    uint64_t value = 0;
    int64_t signed_value = 0;
    uint32_t word;
    int wide = c->options.arch == CSTONE_RISCV64;
    if (cstone_i_equal(i->name, "nop"))
        return cstone_i_count(c, i, 0) && cstone_i_emit_word(c, 0x13, i->name.col);
    if (cstone_i_equal(i->name, "ret"))
        return cstone_i_count(c, i, 0) && cstone_i_emit_word(c, 0x8067, i->name.col);
    if (cstone_i_equal(i->name, "ecall") || cstone_i_equal(i->name, "ebreak") ||
        cstone_i_equal(i->name, "fence.i"))
        return cstone_i_count(c, i, 0) && cstone_i_emit_word(c,
                                                             cstone_i_equal(i->name, "ecall")    ? 0x73u
                                                             : cstone_i_equal(i->name, "ebreak") ? 0x100073u
                                                                                                 : 0x100fu,
                                                             i->name.col);
    if (cstone_i_equal(i->name, "fence")) {
        if (i->count)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                 "MVP FENCE uses the default IORW,IORW form.");
        return cstone_i_emit_word(c, 0x0ff0000f, i->name.col);
    }
    if (cstone_i_equal(i->name, "li")) {
        cstone_i_num number;
        int64_t v;
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a) || !cstone_i_number(c, i->op[1], &number))
            return 0;
        if (!wide && number.magnitude > (number.negative ? UINT64_C(0x80000000) : UINT64_C(0xffffffff)))
            return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[1].col, "RV32 immediate exceeds 32 bits.");
        value = cstone_i_bits(number);
        if (!wide) {
            value &= UINT32_MAX;
            if (value & UINT64_C(0x80000000))
                value |= UINT64_C(0xffffffff00000000);
        }
        v = value <= INT64_MAX ? (int64_t)value : -(int64_t)(UINT64_MAX - value) - 1;
        return emit_li(c, a, v, wide, i->name.col);
    }
    if (cstone_i_equal(i->name, "mv") || cstone_i_equal(i->name, "not") || cstone_i_equal(i->name, "neg") ||
        cstone_i_equal(i->name, "seqz") || cstone_i_equal(i->name, "snez") ||
        cstone_i_equal(i->name, "sext.w")) {
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b))
            return 0;
        if (cstone_i_equal(i->name, "mv"))
            word = itype(0x13, a, b, 0, 0);
        else if (cstone_i_equal(i->name, "not"))
            word = itype(0x13, a, b, 4, -1);
        else if (cstone_i_equal(i->name, "seqz"))
            word = itype(0x13, a, b, 3, 1);
        else if (cstone_i_equal(i->name, "snez"))
            word = 0x3033u | (a << 7) | (b << 20);
        else if (cstone_i_equal(i->name, "sext.w")) {
            if (!wide)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "SEXT.W requires RV64.");
            word = itype(0x1b, a, b, 0, 0);
        } else
            word = 0x40000033u | (a << 7) | (b << 20);
        return cstone_i_emit_word(c, word, i->name.col);
    }
    if (cstone_i_equal(i->name, "la") || cstone_i_equal(i->name, "call") || cstone_i_equal(i->name, "tail")) {
        if (cstone_i_equal(i->name, "la")) {
            if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a))
                return 0;
            return address_pair(c, i, a, a, i->op[1], 0);
        }
        if (!cstone_i_count(c, i, 1))
            return 0;
        return address_pair(c, i, cstone_i_equal(i->name, "tail") ? 6u : 1u,
                            cstone_i_equal(i->name, "tail") ? 0u : 1u, i->op[0], 1);
    }
    if (cstone_i_equal(i->name, "j") || cstone_i_equal(i->name, "jal")) {
        size_t offset = c->out->size;
        cstone_i_span target;
        if (i->count == 1) {
            a = cstone_i_equal(i->name, "j") ? 0u : 1u;
            target = i->op[0];
        } else if (cstone_i_equal(i->name, "jal") && i->count == 2) {
            if (!reg_get(c, i->op[0], &a))
                return 0;
            target = i->op[1];
        } else
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Invalid JAL operands.");
        return cstone_i_emit_word(c, 0x6fu | (a << 7), i->name.col) &&
               cstone_i_reference(c, target, CSTONE_RV_JAL20, offset, 0, 0);
    }
    if (cstone_i_equal(i->name, "jr") || cstone_i_equal(i->name, "jalr")) {
        if (i->count == 1) {
            if (!reg_get(c, i->op[0], &b))
                return 0;
            a = cstone_i_equal(i->name, "jr") ? 0u : 1u;
        } else if (cstone_i_equal(i->name, "jalr") && i->count == 2) {
            if (!reg_get(c, i->op[0], &a) || !mem(c, i->op[1], &b, &signed_value))
                return 0;
        } else if (cstone_i_equal(i->name, "jalr") && i->count == 3) {
            if (!reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b) ||
                !cstone_i_signed(c, i->op[2], 12, &signed_value))
                return 0;
        } else
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Invalid JALR operands.");
        return cstone_i_emit_word(c, itype(0x67, a, b, 0, signed_value), i->name.col);
    }
    {
        static const struct {
            const char *name;
            unsigned funct;
        } branches[] = {{"beq", 0}, {"bne", 1}, {"blt", 4}, {"bge", 5}, {"bltu", 6}, {"bgeu", 7}};
        for (k = 0; k < 6; ++k)
            if (cstone_i_equal(i->name, branches[k].name)) {
                size_t offset = c->out->size;
                if (!cstone_i_count(c, i, 3) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b))
                    return 0;
                return cstone_i_emit_word(c, 0x63u | (branches[k].funct << 12) | (a << 15) | (b << 20),
                                          i->name.col) &&
                       cstone_i_reference(c, i->op[2], CSTONE_RV_BRANCH12, offset, 0, 0);
            }
    }
    if (cstone_i_equal(i->name, "beqz") || cstone_i_equal(i->name, "bnez") ||
        cstone_i_equal(i->name, "bltz") || cstone_i_equal(i->name, "bgez") ||
        cstone_i_equal(i->name, "blez") || cstone_i_equal(i->name, "bgtz")) {
        size_t offset = c->out->size;
        unsigned funct = cstone_i_equal(i->name, "beqz")                                      ? 0u
                         : cstone_i_equal(i->name, "bnez")                                    ? 1u
                         : cstone_i_equal(i->name, "bltz") || cstone_i_equal(i->name, "bgtz") ? 4u
                                                                                              : 5u;
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a))
            return 0;
        b = 0;
        if (cstone_i_equal(i->name, "blez") || cstone_i_equal(i->name, "bgtz")) {
            b = a;
            a = 0;
        }
        return cstone_i_emit_word(c, 0x63u | (funct << 12) | (a << 15) | (b << 20), i->name.col) &&
               cstone_i_reference(c, i->op[1], CSTONE_RV_BRANCH12, offset, 0, 0);
    }
    for (k = 0; k < sizeof(rtypes) / sizeof(rtypes[0]); ++k)
        if (cstone_i_equal(i->name, rtypes[k].name)) {
            if (rtypes[k].word && !wide)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                     "Word arithmetic requires RV64.");
            if (!cstone_i_count(c, i, 3) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b) ||
                !reg_get(c, i->op[2], &d))
                return 0;
            return cstone_i_emit_word(c,
                                      (rtypes[k].word ? 0x3bu : 0x33u) | (a << 7) | (rtypes[k].funct << 12) |
                                          (b << 15) | (d << 20) | (rtypes[k].top << 25),
                                      i->name.col);
        }
    for (k = 0; k < sizeof(itypes) / sizeof(itypes[0]); ++k)
        if (cstone_i_equal(i->name, itypes[k].name)) {
            if (!cstone_i_count(c, i, 3) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b) ||
                !cstone_i_signed(c, i->op[2], 12, &signed_value))
                return 0;
            return cstone_i_emit_word(c, itype(0x13, a, b, itypes[k].funct, signed_value), i->name.col);
        }
    if (cstone_i_equal(i->name, "addiw")) {
        if (!wide)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "ADDIW requires RV64.");
        if (!cstone_i_count(c, i, 3) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b) ||
            !cstone_i_signed(c, i->op[2], 12, &signed_value))
            return 0;
        return cstone_i_emit_word(c, itype(0x1b, a, b, 0, signed_value), i->name.col);
    }
    if (cstone_i_equal(i->name, "slli") || cstone_i_equal(i->name, "srli") ||
        cstone_i_equal(i->name, "srai") || cstone_i_equal(i->name, "slliw") ||
        cstone_i_equal(i->name, "srliw") || cstone_i_equal(i->name, "sraiw")) {
        int word_form = i->name.p[i->name.n - 1] == 'w' || i->name.p[i->name.n - 1] == 'W',
            left = cstone_i_equal(i->name, "slli") || cstone_i_equal(i->name, "slliw"),
            arithmetic = cstone_i_equal(i->name, "srai") || cstone_i_equal(i->name, "sraiw");
        if (word_form && !wide)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Word shift requires RV64.");
        if (!cstone_i_count(c, i, 3) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b) ||
            !cstone_i_unsigned(c, i->op[2], wide && !word_form ? 6u : 5u, &value))
            return 0;
        return cstone_i_emit_word(
            c,
            itype(word_form ? 0x1bu : 0x13u, a, b, left ? 1u : 5u, (int64_t)value | (arithmetic ? 0x400 : 0)),
            i->name.col);
    }
    if (cstone_i_equal(i->name, "lui") || cstone_i_equal(i->name, "auipc")) {
        cstone_i_num number;
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a) || !cstone_i_number(c, i->op[1], &number))
            return 0;
        if (number.magnitude > (number.negative ? 524288u : 1048575u))
            return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[1].col, "Upper immediate exceeds 20 bits.");
        return cstone_i_emit_word(c,
                                  (cstone_i_equal(i->name, "lui") ? 0x37u : 0x17u) | (a << 7) |
                                      (((uint32_t)cstone_i_bits(number) & 0xfffffu) << 12),
                                  i->name.col);
    }
    {
        static const struct {
            const char *name;
            unsigned funct;
            int store, rv64;
        } loads[] = {{"lb", 0, 0, 0},  {"lh", 1, 0, 0},  {"lw", 2, 0, 0},  {"ld", 3, 0, 1},
                     {"lbu", 4, 0, 0}, {"lhu", 5, 0, 0}, {"lwu", 6, 0, 1}, {"sb", 0, 1, 0},
                     {"sh", 1, 1, 0},  {"sw", 2, 1, 0},  {"sd", 3, 1, 1}};
        for (k = 0; k < sizeof(loads) / sizeof(loads[0]); ++k)
            if (cstone_i_equal(i->name, loads[k].name)) {
                if (loads[k].rv64 && !wide)
                    return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                         "This transfer requires RV64.");
                if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a) ||
                    !mem(c, i->op[1], &b, &signed_value))
                    return 0;
                if (loads[k].store) {
                    uint32_t u = (uint32_t)signed_value & 4095u;
                    word = 0x23u | ((u & 31u) << 7) | (loads[k].funct << 12) | (b << 15) | (a << 20) |
                           ((u & 0xfe0u) << 20);
                } else
                    word = itype(3, a, b, loads[k].funct, signed_value);
                return cstone_i_emit_word(c, word, i->name.col);
            }
    }
    {
        static const char *const names[] = {"csrrw", "csrrs", "csrrc", "csrrwi", "csrrsi", "csrrci"};
        for (k = 0; k < 6; ++k)
            if (cstone_i_equal(i->name, names[k])) {
                uint64_t source;
                if (!cstone_i_count(c, i, 3) || !reg_get(c, i->op[0], &a) || !csr(c, i->op[1], &value))
                    return 0;
                if (k >= 3) {
                    if (!cstone_i_unsigned(c, i->op[2], 5, &source))
                        return 0;
                    b = (unsigned)source;
                } else if (!reg_get(c, i->op[2], &b))
                    return 0;
                return cstone_i_emit_word(c,
                                          0x73u | (a << 7) | ((k < 3 ? k + 1 : k + 2) << 12) | (b << 15) |
                                              ((uint32_t)value << 20),
                                          i->name.col);
            }
    }
    if (cstone_i_equal(i->name, "csrr") || cstone_i_equal(i->name, "csrw")) {
        if (!cstone_i_count(c, i, 2))
            return 0;
        if (cstone_i_equal(i->name, "csrr")) {
            if (!reg_get(c, i->op[0], &a) || !csr(c, i->op[1], &value))
                return 0;
            word = 0x2073u | (a << 7) | ((uint32_t)value << 20);
        } else {
            if (!csr(c, i->op[0], &value) || !reg_get(c, i->op[1], &b))
                return 0;
            word = 0x1073u | (b << 15) | ((uint32_t)value << 20);
        }
        return cstone_i_emit_word(c, word, i->name.col);
    }
    return cstone_i_fail(c, CSTONE_UNSUPPORTED_INSTRUCTION, i->name.col,
                         "Unsupported RISC-V instruction '%.*s'.", (int)(i->name.n > 48 ? 48 : i->name.n),
                         i->name.p);
}
