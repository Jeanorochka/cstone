#include "internal.h"
#include <string.h>

static int reg_name(cstone_i_span s, unsigned *reg) {
    unsigned n = 0;
    size_t k;
    if (cstone_i_equal(s, "sp")) {
        *reg = 13;
        return 1;
    }
    if (cstone_i_equal(s, "lr")) {
        *reg = 14;
        return 1;
    }
    if (cstone_i_equal(s, "pc")) {
        *reg = 15;
        return 1;
    }
    if (cstone_i_equal(s, "ip")) {
        *reg = 12;
        return 1;
    }
    if (cstone_i_equal(s, "fp")) {
        *reg = 11;
        return 1;
    }
    if (s.n < 2 || (s.p[0] != 'r' && s.p[0] != 'R'))
        return 0;
    for (k = 1; k < s.n; ++k) {
        if (s.p[k] < '0' || s.p[k] > '9')
            return 0;
        n = n * 10u + (unsigned)(s.p[k] - '0');
        if (n > 15)
            return 0;
    }
    *reg = n;
    return 1;
}
static int reg_get(cstone_i_context *c, cstone_i_span s, unsigned *reg) {
    return reg_name(s, reg) ||
           cstone_i_fail(c, CSTONE_INVALID_OPERAND, s.col, "Expected R0-R15, SP, LR, or PC.");
}
static int cc_name(cstone_i_span s) {
    static const char *const names[] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                        "hi", "ls", "ge", "lt", "gt", "le", "al"};
    unsigned k;
    for (k = 0; k < 15; ++k)
        if (cstone_i_equal(s, names[k]))
            return (int)k;
    if (cstone_i_equal(s, "hs"))
        return 2;
    if (cstone_i_equal(s, "lo"))
        return 3;
    return -1;
}
static int dp_name(cstone_i_span s) {
    static const char *const names[] = {"and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
                                        "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"};
    unsigned k;
    for (k = 0; k < 16; ++k)
        if (cstone_i_equal(s, names[k]))
            return (int)k;
    return -1;
}
static int known(cstone_i_span s) {
    static const char *const names[] = {"b",    "bl",  "bx",   "blx",  "ldr", "str", "ldrb", "strb",
                                        "push", "pop", "movw", "movt", "mul", "nop", "svc",  "bkpt"};
    unsigned k;
    if (dp_name(s) >= 0)
        return 1;
    if (s.n > 1 && cstone_i_equal(cstone_i_slice(s, s.n - 1, 1), "s") &&
        dp_name(cstone_i_slice(s, 0, s.n - 1)) >= 0)
        return 1;
    for (k = 0; k < sizeof(names) / sizeof(names[0]); ++k)
        if (cstone_i_equal(s, names[k]))
            return 1;
    return 0;
}
static uint32_t ror32(uint32_t n, unsigned shift) {
    return shift ? (n >> shift) | (n << (32 - shift)) : n;
}
static int arm_imm(uint32_t value, uint32_t *encoding) {
    unsigned rot;
    for (rot = 0; rot < 16; ++rot) {
        uint32_t v = ror32(value, (32 - rot * 2) % 32);
        if (v <= 255) {
            *encoding = (rot << 8) | v;
            return 1;
        }
    }
    return 0;
}
static int shift_operand(cstone_i_context *c, cstone_i_span s, uint32_t *encoding) {
    static const char *const names[] = {"lsl", "lsr", "asr", "ror"};
    size_t end = 0;
    unsigned k;
    uint64_t value;
    while (end < s.n && !cstone_i_space(s.p[end]))
        ++end;
    for (k = 0; k < 4; ++k)
        if (cstone_i_equal(cstone_i_slice(s, 0, end), names[k]))
            break;
    if (k == 4)
        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, s.col, "Unsupported register shift.");
    if (!cstone_i_unsigned(c, cstone_i_trim(cstone_i_slice(s, end, s.n - end)), 6, &value))
        return 0;
    if (value > 32 || (k == 0 && value == 32) || (k == 3 && (value == 0 || value == 32)))
        return cstone_i_fail(c, CSTONE_RANGE_ERROR, s.col, "Invalid ARM shift amount.");
    *encoding = ((uint32_t)(value & 31u) << 7) | (k << 5);
    return 1;
}
static int reg_list(cstone_i_context *c, cstone_i_span s, uint32_t *mask) {
    size_t start = 1, end;
    *mask = 0;
    if (s.n < 3 || s.p[0] != '{' || s.p[s.n - 1] != '}')
        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, s.col, "Expected a nonempty register list.");
    while (start < s.n - 1) {
        cstone_i_span term;
        size_t dash;
        unsigned first = 0, last = 0, k;
        end = start;
        while (end < s.n - 1 && s.p[end] != ',')
            ++end;
        term = cstone_i_trim(cstone_i_slice(s, start, end - start));
        dash = 0;
        while (dash < term.n && term.p[dash] != '-')
            ++dash;
        if (!reg_get(c, cstone_i_trim(cstone_i_slice(term, 0, dash)), &first))
            return 0;
        last = first;
        if (dash < term.n &&
            !reg_get(c, cstone_i_trim(cstone_i_slice(term, dash + 1, term.n - dash - 1)), &last))
            return 0;
        if (last < first)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, term.col, "Descending register range.");
        for (k = first; k <= last; ++k) {
            if (*mask & (1u << k))
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, term.col, "Duplicate register in list.");
            *mask |= 1u << k;
        }
        if (end == s.n - 1)
            break;
        start = end + 1;
        if (start == s.n - 1)
            return cstone_i_fail(c, CSTONE_SYNTAX_ERROR, s.col + start, "Empty register-list entry.");
    }
    return *mask != 0 || cstone_i_fail(c, CSTONE_INVALID_OPERAND, s.col, "Empty register list.");
}
static int mem(cstone_i_context *c, cstone_i_span s, unsigned *base, int64_t *offset, int *pre) {
    size_t close = 0, comma = 0;
    cstone_i_span inner, tail;
    if (s.n < 3 || s.p[0] != '[')
        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, s.col, "Expected [base, #offset].");
    while (close < s.n && s.p[close] != ']')
        ++close;
    if (close == s.n)
        return cstone_i_fail(c, CSTONE_SYNTAX_ERROR, s.col, "Unclosed memory operand.");
    tail = cstone_i_trim(cstone_i_slice(s, close + 1, s.n - close - 1));
    *pre = tail.n != 0;
    if (tail.n && !cstone_i_equal(tail, "!"))
        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, tail.col, "Invalid memory suffix.");
    inner = cstone_i_trim(cstone_i_slice(s, 1, close - 1));
    while (comma < inner.n && inner.p[comma] != ',')
        ++comma;
    if (!reg_get(c, cstone_i_trim(cstone_i_slice(inner, 0, comma)), base))
        return 0;
    *offset = 0;
    if (comma < inner.n &&
        !cstone_i_signed(c, cstone_i_trim(cstone_i_slice(inner, comma + 1, inner.n - comma - 1)), 32, offset))
        return 0;
    return 1;
}
int cstone_i_arm(cstone_i_context *c, const cstone_i_insn *i) {
    cstone_i_span name = i->name;
    unsigned cc = 14, a = 0, b = 0, d = 0;
    int flags = 0, dp;
    uint64_t value = 0;
    uint32_t word = 0, operand2 = 0;
    size_t source = 0;
    if (!known(name) && name.n > 2) {
        int condition = cc_name(cstone_i_slice(name, name.n - 2, 2));
        if (condition >= 0 && known(cstone_i_slice(name, 0, name.n - 2))) {
            cc = (unsigned)condition;
            name = cstone_i_slice(name, 0, name.n - 2);
        }
    }
    if (name.n > 1 && cstone_i_equal(cstone_i_slice(name, name.n - 1, 1), "s") &&
        dp_name(cstone_i_slice(name, 0, name.n - 1)) >= 0) {
        flags = 1;
        name = cstone_i_slice(name, 0, name.n - 1);
    }
    if (cstone_i_equal(name, "nop"))
        return cstone_i_count(c, i, 0) && cstone_i_emit_word(c, (cc << 28) | 0x0320f000u, i->name.col);
    if (cstone_i_equal(name, "b") || cstone_i_equal(name, "bl")) {
        size_t offset = c->out->size;
        if (!cstone_i_count(c, i, 1))
            return 0;
        return cstone_i_emit_word(c, (cc << 28) | (cstone_i_equal(name, "bl") ? 0x0b000000u : 0x0a000000u),
                                  i->name.col) &&
               cstone_i_reference(c, i->op[0], CSTONE_ARM_BRANCH24, offset, 8, 0);
    }
    if (cstone_i_equal(name, "bx") || cstone_i_equal(name, "blx")) {
        if (!cstone_i_count(c, i, 1) || !reg_get(c, i->op[0], &a))
            return 0;
        if (a == 15)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col,
                                 "PC is not a supported exchange-branch operand.");
        return cstone_i_emit_word(
            c, (cc << 28) | (cstone_i_equal(name, "bx") ? 0x012fff10u : 0x012fff30u) | a, i->name.col);
    }
    if (cstone_i_equal(name, "movw") || cstone_i_equal(name, "movt")) {
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a) ||
            !cstone_i_unsigned(c, i->op[1], 16, &value))
            return 0;
        if (a == 15)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col,
                                 "Move-wide destination cannot be PC.");
        return cstone_i_emit_word(c,
                                  (cc << 28) | (cstone_i_equal(name, "movw") ? 0x03000000u : 0x03400000u) |
                                      (a << 12) | (((uint32_t)value & 0xf000u) << 4) |
                                      ((uint32_t)value & 0xfffu),
                                  i->name.col);
    }
    if (cstone_i_equal(name, "svc")) {
        if (!cstone_i_count(c, i, 1) || !cstone_i_unsigned(c, i->op[0], 24, &value))
            return 0;
        return cstone_i_emit_word(c, (cc << 28) | 0x0f000000u | (uint32_t)value, i->name.col);
    }
    if (cstone_i_equal(name, "bkpt")) {
        if (cc != 14)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "BKPT cannot be conditional.");
        if (!cstone_i_count(c, i, 1) || !cstone_i_unsigned(c, i->op[0], 16, &value))
            return 0;
        return cstone_i_emit_word(
            c, 0xe1200070u | (((uint32_t)value & 0xfff0u) << 4) | ((uint32_t)value & 15u), i->name.col);
    }
    if (cstone_i_equal(name, "push") || cstone_i_equal(name, "pop")) {
        uint32_t mask;
        int pop = cstone_i_equal(name, "pop");
        if (!cstone_i_count(c, i, 1) || !reg_list(c, i->op[0], &mask))
            return 0;
        if (mask & (1u << 13))
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col,
                                 "SP cannot be in a stack register list.");
        return cstone_i_emit_word(c, (cc << 28) | (pop ? 0x08bd0000u : 0x092d0000u) | mask, i->name.col);
    }
    if (cstone_i_equal(name, "mul")) {
        if (!cstone_i_count(c, i, 3) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b) ||
            !reg_get(c, i->op[2], &d))
            return 0;
        if (a == 15 || b == 15 || d == 15)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "MUL cannot use PC.");
        return cstone_i_emit_word(c, (cc << 28) | 0x90u | (a << 16) | (d << 8) | b, i->name.col);
    }
    if (cstone_i_equal(name, "ldr") || cstone_i_equal(name, "str") || cstone_i_equal(name, "ldrb") ||
        cstone_i_equal(name, "strb")) {
        int64_t displacement = 0;
        int pre = 0, load = cstone_i_equal(name, "ldr") || cstone_i_equal(name, "ldrb"),
            byte = cstone_i_equal(name, "ldrb") || cstone_i_equal(name, "strb");
        if ((i->count != 2 && i->count != 3) || !reg_get(c, i->op[0], &a) ||
            !mem(c, i->op[1], &b, &displacement, &pre))
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Invalid ARM load/store operands.");
        if (i->count == 3) {
            if (pre || displacement)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Invalid post-index form.");
            if (!cstone_i_signed(c, i->op[2], 32, &displacement))
                return 0;
        }
        if (displacement < -4095 || displacement > 4095)
            return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[1].col, "Offset exceeds 12 bits.");
        if ((pre || i->count == 3) && (b == 15 || a == b))
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                 "Invalid writeback register overlap.");
        if (byte && a == 15)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col, "Byte transfer cannot use PC.");
        word = (cc << 28) | 0x04000000u | (i->count == 2 ? 1u << 24 : 0u) |
               (displacement >= 0 ? 1u << 23 : 0u) | (byte ? 1u << 22 : 0u) | (pre ? 1u << 21 : 0u) |
               (load ? 1u << 20 : 0u) | (b << 16) | (a << 12) |
               (uint32_t)(displacement < 0 ? -displacement : displacement);
        return cstone_i_emit_word(c, word, i->name.col);
    }
    dp = dp_name(name);
    if (dp >= 0) {
        int compare = dp >= 8 && dp <= 11, move = dp == 13 || dp == 15;
        size_t minimum = move || compare ? 2u : 3u;
        unsigned rm;
        if (i->count < minimum || i->count > minimum + 1)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                 "Invalid data-processing operand count.");
        if (!reg_get(c, i->op[0], &a))
            return 0;
        if (compare) {
            b = a;
            a = 0;
            flags = 1;
            source = 1;
        } else if (move) {
            b = 0;
            source = 1;
        } else {
            if (!reg_get(c, i->op[1], &b))
                return 0;
            source = 2;
        }
        if (reg_name(i->op[source], &rm)) {
            operand2 = rm;
            if (i->count == minimum + 1) {
                uint32_t sh = 0;
                if (!shift_operand(c, i->op[source + 1], &sh))
                    return 0;
                operand2 |= sh;
            }
        } else {
            cstone_i_num num;
            if (i->count != minimum || !cstone_i_number(c, i->op[source], &num))
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Invalid immediate operand.");
            if (num.magnitude > (num.negative ? UINT64_C(0x80000000) : UINT64_C(0xffffffff)) ||
                !arm_imm((uint32_t)cstone_i_bits(num), &operand2))
                return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[source].col,
                                     "Immediate is not encodable as ARM rotated imm8.");
            operand2 |= 1u << 25;
        }
        return cstone_i_emit_word(
            c, (cc << 28) | ((unsigned)dp << 21) | (flags ? 1u << 20 : 0u) | (b << 16) | (a << 12) | operand2,
            i->name.col);
    }
    return cstone_i_fail(c, CSTONE_UNSUPPORTED_INSTRUCTION, i->name.col,
                         "Unsupported ARM instruction '%.*s'.", (int)(i->name.n > 48 ? 48 : i->name.n),
                         i->name.p);
}
int cstone_i_thumb(cstone_i_context *c, const cstone_i_insn *i) {
    unsigned a = 0, b = 0, d = 0, k;
    uint64_t value = 0;
    uint32_t word = 0;
    int condition = -1;
    if (i->name.n > 1 && cstone_i_equal(cstone_i_slice(i->name, 0, 1), "b"))
        condition = cc_name(cstone_i_slice(i->name, 1, i->name.n - 1));
    if (cstone_i_equal(i->name, "nop"))
        return cstone_i_count(c, i, 0) && cstone_i_emit_half(c, 0xbf00, i->name.col);
    if (cstone_i_equal(i->name, "b") || cstone_i_equal(i->name, "b.w") || cstone_i_equal(i->name, "bl") ||
        condition >= 0) {
        size_t offset = c->out->size;
        cstone_reloc_kind kind;
        if (!cstone_i_count(c, i, 1))
            return 0;
        if (cstone_i_equal(i->name, "bl") || cstone_i_equal(i->name, "b.w")) {
            if (!cstone_i_emit_half(c, 0xf000, i->name.col) ||
                !cstone_i_emit_half(c, cstone_i_equal(i->name, "bl") ? 0xd000 : 0x9000, i->name.col))
                return 0;
            kind = CSTONE_THUMB_BRANCH24;
        } else {
            if (condition >= 14)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Unsupported Thumb condition.");
            if (!cstone_i_emit_half(
                    c, (uint16_t)(condition >= 0 ? 0xd000u | ((unsigned)condition << 8) : 0xe000u),
                    i->name.col))
                return 0;
            kind = condition >= 0 ? CSTONE_THUMB_BRANCH8 : CSTONE_THUMB_BRANCH11;
        }
        return cstone_i_reference(c, i->op[0], kind, offset, 4, 0);
    }
    if (cstone_i_equal(i->name, "bx") || cstone_i_equal(i->name, "blx")) {
        if (!cstone_i_count(c, i, 1) || !reg_get(c, i->op[0], &a))
            return 0;
        if (a == 15)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col, "Exchange branch cannot use PC.");
        return cstone_i_emit_half(
            c, (uint16_t)((cstone_i_equal(i->name, "bx") ? 0x4700u : 0x4780u) | (a << 3)), i->name.col);
    }
    if (cstone_i_equal(i->name, "svc") || cstone_i_equal(i->name, "bkpt") || cstone_i_equal(i->name, "udf")) {
        if (!cstone_i_count(c, i, 1) || !cstone_i_unsigned(c, i->op[0], 8, &value))
            return 0;
        word = cstone_i_equal(i->name, "svc") ? 0xdf00u : cstone_i_equal(i->name, "bkpt") ? 0xbe00u : 0xde00u;
        return cstone_i_emit_half(c, (uint16_t)(word | (uint32_t)value), i->name.col);
    }
    if (cstone_i_equal(i->name, "push") || cstone_i_equal(i->name, "pop")) {
        uint32_t mask;
        int pop = cstone_i_equal(i->name, "pop");
        if (!cstone_i_count(c, i, 1) || !reg_list(c, i->op[0], &mask))
            return 0;
        if (mask & ~(0xffu | (pop ? 1u << 15 : 1u << 14)))
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col,
                                 "Thumb stack list supports R0-R7 and LR/PC.");
        return cstone_i_emit_half(c,
                                  (uint16_t)((pop ? 0xbc00u : 0xb400u) | (mask & 255u) |
                                             ((mask & (pop ? 1u << 15 : 1u << 14)) ? 0x100u : 0u)),
                                  i->name.col);
    }
    if (cstone_i_equal(i->name, "movw") || cstone_i_equal(i->name, "movt")) {
        uint16_t first, second;
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a) ||
            !cstone_i_unsigned(c, i->op[1], 16, &value))
            return 0;
        if (a == 13 || a == 15)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col,
                                 "Thumb move-wide cannot use SP or PC.");
        first = (uint16_t)((cstone_i_equal(i->name, "movw") ? 0xf240u : 0xf2c0u) |
                           (((uint32_t)value >> 12) & 15u) | (((uint32_t)value & 0x800u) >> 1));
        second = (uint16_t)((((uint32_t)value >> 8) & 7u) << 12 | (a << 8) | ((uint32_t)value & 255u));
        return cstone_i_emit_half(c, first, i->name.col) && cstone_i_emit_half(c, second, i->name.col);
    }
    if (cstone_i_equal(i->name, "mov") || cstone_i_equal(i->name, "movs") || cstone_i_equal(i->name, "cmp")) {
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a))
            return 0;
        if (reg_name(i->op[1], &b)) {
            if (cstone_i_equal(i->name, "movs")) {
                if (a > 7 || b > 7)
                    return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "MOVS uses low registers.");
                word = (b << 3) | a;
            } else if (cstone_i_equal(i->name, "cmp"))
                word = (a < 8 && b < 8) ? 0x4280u | (b << 3) | a
                                        : 0x4500u | ((a & 8u) << 4) | (b << 3) | (a & 7u);
            else
                word = 0x4600u | ((a & 8u) << 4) | (b << 3) | (a & 7u);
        } else {
            if (cstone_i_equal(i->name, "mov"))
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                     "Use MOVS or MOVW for a Thumb immediate.");
            if (a > 7 || !cstone_i_unsigned(c, i->op[1], 8, &value))
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col,
                                     "Immediate form requires a low register.");
            word = (cstone_i_equal(i->name, "cmp") ? 0x2800u : 0x2000u) | (a << 8) | (uint32_t)value;
        }
        return cstone_i_emit_half(c, (uint16_t)word, i->name.col);
    }
    if (cstone_i_equal(i->name, "add") || cstone_i_equal(i->name, "sub")) {
        int sub = cstone_i_equal(i->name, "sub");
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a))
            return 0;
        if (reg_name(i->op[1], &b) && !sub)
            return cstone_i_emit_half(c, (uint16_t)(0x4400u | ((a & 8u) << 4) | (b << 3) | (a & 7u)),
                                      i->name.col);
        if (a != 13 || !cstone_i_unsigned(c, i->op[1], 9, &value) || value % 4 || value > 508)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                 "Supported immediate ADD/SUB form is SP, #multiple_of_4.");
        return cstone_i_emit_half(c, (uint16_t)(0xb000u | (sub ? 0x80u : 0u) | (uint32_t)(value / 4)),
                                  i->name.col);
    }
    if (cstone_i_equal(i->name, "adds") || cstone_i_equal(i->name, "subs")) {
        int sub = cstone_i_equal(i->name, "subs");
        if ((i->count != 2 && i->count != 3) || !reg_get(c, i->op[0], &a))
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                 "Invalid Thumb arithmetic operands.");
        if (a > 7)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col,
                                 "Flag-setting arithmetic requires low registers.");
        if (i->count == 2) {
            if (!cstone_i_unsigned(c, i->op[1], 8, &value))
                return 0;
            word = (sub ? 0x3800u : 0x3000u) | (a << 8) | (uint32_t)value;
        } else {
            if (!reg_get(c, i->op[1], &b) || b > 7)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[1].col, "Expected a low register.");
            if (reg_name(i->op[2], &d)) {
                if (d > 7)
                    return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[2].col, "Expected a low register.");
                word = (sub ? 0x1a00u : 0x1800u) | (d << 6) | (b << 3) | a;
            } else {
                if (!cstone_i_unsigned(c, i->op[2], 3, &value))
                    return 0;
                word = (sub ? 0x1e00u : 0x1c00u) | ((uint32_t)value << 6) | (b << 3) | a;
            }
        }
        return cstone_i_emit_half(c, (uint16_t)word, i->name.col);
    }
    {
        static const char *const names[] = {"ands", "eors", "lsls", "lsrs", "asrs", "adcs", "sbcs", "rors",
                                            "tst",  "rsbs", "cmp",  "cmn",  "orrs", "muls", "bics", "mvns"};
        for (k = 0; k < 16; ++k)
            if (cstone_i_equal(i->name, names[k]) && i->count == 2) {
                if (!reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b))
                    return 0;
                if (a > 7 || b > 7)
                    return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                         "This Thumb form requires low registers.");
                if (k == 9)
                    return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                         "Use NEGS for two-operand negate.");
                return cstone_i_emit_half(c, (uint16_t)(0x4000u | (k << 6) | (b << 3) | a), i->name.col);
            }
    }
    if (cstone_i_equal(i->name, "negs")) {
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b))
            return 0;
        if (a > 7 || b > 7)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "NEGS requires low registers.");
        return cstone_i_emit_half(c, (uint16_t)(0x4240u | (b << 3) | a), i->name.col);
    }
    if (cstone_i_equal(i->name, "lsls") || cstone_i_equal(i->name, "lsrs") ||
        cstone_i_equal(i->name, "asrs")) {
        if (!cstone_i_count(c, i, 3) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b) ||
            !cstone_i_unsigned(c, i->op[2], 6, &value))
            return 0;
        if (a > 7 || b > 7 || value > 32 || (cstone_i_equal(i->name, "lsls") ? value == 32 : value == 0))
            return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->name.col, "Invalid Thumb shift operands.");
        word = cstone_i_equal(i->name, "lsls") ? 0u : cstone_i_equal(i->name, "lsrs") ? 0x0800u : 0x1000u;
        return cstone_i_emit_half(c, (uint16_t)(word | ((uint32_t)(value & 31u) << 6) | (b << 3) | a),
                                  i->name.col);
    }
    if (cstone_i_equal(i->name, "adr")) {
        size_t offset = c->out->size;
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a))
            return 0;
        if (a > 7)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col,
                                 "Thumb ADR requires a low register.");
        return cstone_i_emit_half(c, (uint16_t)(0xa000u | (a << 8)), i->name.col) &&
               cstone_i_reference(c, i->op[1], CSTONE_THUMB_LITERAL8, offset, 4, 0);
    }
    if (cstone_i_equal(i->name, "ldr") || cstone_i_equal(i->name, "str") || cstone_i_equal(i->name, "ldrb") ||
        cstone_i_equal(i->name, "strb") || cstone_i_equal(i->name, "ldrh") ||
        cstone_i_equal(i->name, "strh")) {
        int load = cstone_i_equal(i->name, "ldr") || cstone_i_equal(i->name, "ldrb") ||
                   cstone_i_equal(i->name, "ldrh"),
            pre = 0;
        int64_t off;
        unsigned scale;
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a))
            return 0;
        if (a > 7)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col,
                                 "Thumb load/store requires a low data register.");
        if (i->op[1].p[0] != '[' && cstone_i_equal(i->name, "ldr")) {
            size_t offset = c->out->size;
            return cstone_i_emit_half(c, (uint16_t)(0x4800u | (a << 8)), i->name.col) &&
                   cstone_i_reference(c, i->op[1], CSTONE_THUMB_LITERAL8, offset, 4, 0);
        }
        if (!mem(c, i->op[1], &b, &off, &pre))
            return 0;
        scale = cstone_i_equal(i->name, "ldrb") || cstone_i_equal(i->name, "strb")   ? 1u
                : cstone_i_equal(i->name, "ldrh") || cstone_i_equal(i->name, "strh") ? 2u
                                                                                     : 4u;
        if (pre || off < 0 || off % scale)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[1].col,
                                 "Thumb memory form requires a nonnegative aligned offset.");
        if (b == 13 && scale == 4) {
            if (off > 1020)
                return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[1].col, "SP-relative offset exceeds 1020.");
            word = (load ? 0x9800u : 0x9000u) | (a << 8) | (uint32_t)(off / 4);
        } else {
            if (b > 7 || off / scale > 31)
                return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[1].col,
                                     "Thumb memory offset/base exceeds this form.");
            word = scale == 1 ? 0x7000u : scale == 2 ? 0x8000u : 0x6000u;
            word |= (load ? 0x0800u : 0u) | ((uint32_t)(off / scale) << 6) | (b << 3) | a;
        }
        return cstone_i_emit_half(c, (uint16_t)word, i->name.col);
    }
    return cstone_i_fail(c, CSTONE_UNSUPPORTED_INSTRUCTION, i->name.col,
                         "Unsupported Thumb instruction '%.*s'.", (int)(i->name.n > 48 ? 48 : i->name.n),
                         i->name.p);
}
