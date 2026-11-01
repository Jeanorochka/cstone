#include "internal.h"
#include <string.h>

typedef struct a64reg {
    unsigned id, width;
    int sp;
} a64reg;
static int reg_name(cstone_i_span s, a64reg *r) {
    unsigned value = 0;
    size_t k;
    r->sp = 0;
    if (cstone_i_equal(s, "sp") || cstone_i_equal(s, "wsp")) {
        r->id = 31;
        r->width = cstone_i_equal(s, "sp") ? 64u : 32u;
        r->sp = 1;
        return 1;
    }
    if (cstone_i_equal(s, "xzr") || cstone_i_equal(s, "wzr")) {
        r->id = 31;
        r->width = cstone_i_equal(s, "xzr") ? 64u : 32u;
        return 1;
    }
    if (cstone_i_equal(s, "fp")) {
        r->id = 29;
        r->width = 64;
        return 1;
    }
    if (cstone_i_equal(s, "lr")) {
        r->id = 30;
        r->width = 64;
        return 1;
    }
    if (s.n < 2 || (s.p[0] != 'x' && s.p[0] != 'X' && s.p[0] != 'w' && s.p[0] != 'W'))
        return 0;
    for (k = 1; k < s.n; ++k) {
        if (s.p[k] < '0' || s.p[k] > '9')
            return 0;
        value = value * 10u + (unsigned)(s.p[k] - '0');
        if (value > 30)
            return 0;
    }
    r->id = value;
    r->width = s.p[0] == 'x' || s.p[0] == 'X' ? 64u : 32u;
    return 1;
}
static int reg_get(cstone_i_context *c, cstone_i_span s, a64reg *r) {
    return reg_name(s, r) ||
           cstone_i_fail(c, CSTONE_INVALID_OPERAND, s.col, "Expected an AArch64 general-purpose register.");
}
static int condition(cstone_i_span s) {
    static const char *const names[] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                        "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
    unsigned k;
    for (k = 0; k < 16; ++k)
        if (cstone_i_equal(s, names[k]))
            return (int)k;
    if (cstone_i_equal(s, "hs"))
        return 2;
    if (cstone_i_equal(s, "lo"))
        return 3;
    return -1;
}
static int shift(cstone_i_context *c, cstone_i_span s, const char *name, unsigned maximum, uint64_t *value) {
    size_t k = 0;
    while (k < s.n && !cstone_i_space(s.p[k]))
        ++k;
    if (!cstone_i_equal(cstone_i_slice(s, 0, k), name))
        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, s.col, "Unsupported shift kind.");
    s = cstone_i_trim(cstone_i_slice(s, k, s.n - k));
    if (!cstone_i_unsigned(c, s, 6, value))
        return 0;
    return *value <= maximum || cstone_i_fail(c, CSTONE_RANGE_ERROR, s.col, "Shift exceeds register width.");
}
static int mem(cstone_i_context *c, cstone_i_span s, a64reg *base, int64_t *offset, int *pre) {
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
    if (base->width != 64 || (!base->sp && base->id == 31))
        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, inner.col, "Memory base must be X0-X30 or SP.");
    *offset = 0;
    if (comma < inner.n &&
        !cstone_i_signed(c, cstone_i_trim(cstone_i_slice(inner, comma + 1, inner.n - comma - 1)), 32, offset))
        return 0;
    return 1;
}
static int branch(cstone_i_context *c, const cstone_i_insn *i, uint32_t word, cstone_i_span target,
                  cstone_reloc_kind kind) {
    size_t offset = c->out->size;
    return cstone_i_emit_word(c, word, i->name.col) && cstone_i_reference(c, target, kind, offset, 0, 0);
}
static int mov_immediate(cstone_i_context *c, a64reg rd, cstone_i_span text, size_t column) {
    cstone_i_num n;
    uint64_t value;
    unsigned chunks = rd.width / 16, k, zeros = 0, ones = 0, first = 1;
    int invert;
    if (!cstone_i_number(c, text, &n))
        return 0;
    if (rd.sp || (rd.width == 32 && n.magnitude > (n.negative ? UINT64_C(0x80000000) : UINT64_C(0xffffffff))))
        return cstone_i_fail(c, CSTONE_RANGE_ERROR, text.col, "Immediate does not fit the destination.");
    value = cstone_i_bits(n);
    if (rd.width == 32)
        value &= UINT64_C(0xffffffff);
    for (k = 0; k < chunks; ++k) {
        unsigned part = (unsigned)((value >> (k * 16)) & 65535u);
        zeros += part == 0;
        ones += part == 65535;
    }
    invert = ones > zeros;
    for (k = 0; k < chunks; ++k) {
        unsigned part = (unsigned)((value >> (k * 16)) & 65535u);
        uint32_t word;
        if (part == (invert ? 65535u : 0u) && !(first && k + 1 == chunks))
            continue;
        word = (rd.width == 64 ? 0x80000000u : 0u) |
               (first ? (invert ? 0x12800000u : 0x52800000u) : 0x72800000u);
        word |= (k << 21) | ((first && invert ? part ^ 65535u : part) << 5) | rd.id;
        if (!cstone_i_emit_word(c, word, column))
            return 0;
        first = 0;
    }
    return 1;
}
static int logical_immediate(uint64_t value, unsigned width, uint32_t *encoding) {
    unsigned size, ones, rotation, k;
    if (width == 32 && value > UINT32_MAX)
        return 0;
    for (size = 2; size <= width; size *= 2) {
        uint64_t mask = size == 64 ? UINT64_MAX : (UINT64_C(1) << size) - 1;
        for (ones = 1; ones < size; ++ones) {
            uint64_t pattern = (UINT64_C(1) << ones) - 1;
            for (rotation = 0; rotation < size; ++rotation) {
                uint64_t element =
                    rotation ? ((pattern >> rotation) | (pattern << (size - rotation))) & mask : pattern;
                uint64_t repeated = 0;
                for (k = 0; k < width; k += size)
                    repeated |= element << k;
                if (repeated == value) {
                    *encoding = (size == 64 ? 1u << 22 : 0u) | (rotation << 16) |
                                ((((~(size - 1u) << 1) | (ones - 1u)) & 63u) << 10);
                    return 1;
                }
            }
        }
    }
    return 0;
}
int cstone_i_aarch64(cstone_i_context *c, const cstone_i_insn *i) {
    a64reg a = {0, 0, 0}, b = {0, 0, 0}, d = {0, 0, 0};
    uint32_t word = 0;
    uint64_t value = 0, amount = 0;
    int64_t signed_value = 0;
    unsigned k;
    if (cstone_i_equal(i->name, "nop"))
        return cstone_i_count(c, i, 0) && cstone_i_emit_word(c, 0xd503201f, i->name.col);
    if (cstone_i_equal(i->name, "ret") || cstone_i_equal(i->name, "br") || cstone_i_equal(i->name, "blr")) {
        if (cstone_i_equal(i->name, "ret") && !i->count) {
            a.id = 30;
            a.width = 64;
        } else if (!cstone_i_count(c, i, 1) || !reg_get(c, i->op[0], &a))
            return 0;
        if (a.width != 64 || a.sp)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                 "Branch register must be a 64-bit X register.");
        return cstone_i_emit_word(c,
                                  (cstone_i_equal(i->name, "ret")  ? 0xd65f0000u
                                   : cstone_i_equal(i->name, "br") ? 0xd61f0000u
                                                                   : 0xd63f0000u) |
                                      (a.id << 5),
                                  i->name.col);
    }
    if (cstone_i_equal(i->name, "b") || cstone_i_equal(i->name, "bl"))
        return cstone_i_count(c, i, 1) &&
               branch(c, i, cstone_i_equal(i->name, "bl") ? 0x94000000u : 0x14000000u, i->op[0],
                      CSTONE_A64_BRANCH26);
    if (i->name.n > 2 && cstone_i_equal(cstone_i_slice(i->name, 0, 2), "b.")) {
        int cc = condition(cstone_i_slice(i->name, 2, i->name.n - 2));
        if (cc < 0)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Unknown condition code.");
        return cstone_i_count(c, i, 1) &&
               branch(c, i, 0x54000000u | (unsigned)cc, i->op[0], CSTONE_A64_BRANCH19);
    }
    if (cstone_i_equal(i->name, "cbz") || cstone_i_equal(i->name, "cbnz")) {
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a))
            return 0;
        if (a.sp)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col, "SP is not a CBZ operand.");
        return branch(c, i,
                      0x34000000u | (a.width == 64 ? 0x80000000u : 0u) |
                          (cstone_i_equal(i->name, "cbnz") ? 0x01000000u : 0u) | a.id,
                      i->op[1], CSTONE_A64_BRANCH19);
    }
    if (cstone_i_equal(i->name, "tbz") || cstone_i_equal(i->name, "tbnz")) {
        if (!cstone_i_count(c, i, 3) || !reg_get(c, i->op[0], &a) ||
            !cstone_i_unsigned(c, i->op[1], 6, &value))
            return 0;
        if (a.sp || value >= a.width)
            return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[1].col,
                                 "Bit index exceeds the register width.");
        return branch(c, i,
                      0x36000000u | ((uint32_t)(value & 32u) << 26) | ((uint32_t)(value & 31u) << 19) |
                          (cstone_i_equal(i->name, "tbnz") ? 0x01000000u : 0u) | a.id,
                      i->op[2], CSTONE_A64_BRANCH14);
    }
    if (cstone_i_equal(i->name, "adr") || cstone_i_equal(i->name, "adrp")) {
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a))
            return 0;
        if (a.width != 64 || a.sp)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col,
                                 "ADR destination must be an X register.");
        return branch(c, i, (cstone_i_equal(i->name, "adrp") ? 0x90000000u : 0x10000000u) | a.id, i->op[1],
                      cstone_i_equal(i->name, "adrp") ? CSTONE_A64_ADRP21 : CSTONE_A64_ADR21);
    }
    if (cstone_i_equal(i->name, "svc") || cstone_i_equal(i->name, "hvc") || cstone_i_equal(i->name, "smc") ||
        cstone_i_equal(i->name, "brk") || cstone_i_equal(i->name, "hlt")) {
        if (!cstone_i_count(c, i, 1) || !cstone_i_unsigned(c, i->op[0], 16, &value))
            return 0;
        word = cstone_i_equal(i->name, "svc")   ? 0xd4000001u
               : cstone_i_equal(i->name, "hvc") ? 0xd4000002u
               : cstone_i_equal(i->name, "smc") ? 0xd4000003u
               : cstone_i_equal(i->name, "brk") ? 0xd4200000u
                                                : 0xd4400000u;
        return cstone_i_emit_word(c, word | ((uint32_t)value << 5), i->name.col);
    }
    if (cstone_i_equal(i->name, "movz") || cstone_i_equal(i->name, "movn") ||
        cstone_i_equal(i->name, "movk")) {
        if ((i->count != 2 && i->count != 3) || !reg_get(c, i->op[0], &a) ||
            !cstone_i_unsigned(c, i->op[1], 16, &value))
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                 "Expected register, imm16, optional LSL.");
        if (i->count == 3 && !shift(c, i->op[2], "lsl", a.width - 16, &amount))
            return 0;
        if (a.sp || amount % 16)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                 "Move-wide shift must be a multiple of 16.");
        word = cstone_i_equal(i->name, "movz")   ? 0x52800000u
               : cstone_i_equal(i->name, "movn") ? 0x12800000u
                                                 : 0x72800000u;
        return cstone_i_emit_word(c,
                                  word | (a.width == 64 ? 0x80000000u : 0u) |
                                      ((uint32_t)(amount / 16) << 21) | ((uint32_t)value << 5) | a.id,
                                  i->name.col);
    }
    if (cstone_i_equal(i->name, "mov")) {
        if (!cstone_i_count(c, i, 2) || !reg_get(c, i->op[0], &a))
            return 0;
        if (!reg_name(i->op[1], &b))
            return mov_immediate(c, a, i->op[1], i->name.col);
        if (a.width != b.width)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[1].col, "Register widths must match.");
        if (a.sp || b.sp) {
            if ((a.id == 31 && !a.sp) || (b.id == 31 && !b.sp))
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                     "Cannot move between SP and ZR.");
            word = 0x11000000u | (a.width == 64 ? 0x80000000u : 0u) | (b.id << 5) | a.id;
        } else
            word = 0x2a0003e0u | (a.width == 64 ? 0x80000000u : 0u) | (b.id << 16) | a.id;
        return cstone_i_emit_word(c, word, i->name.col);
    }
    if (cstone_i_equal(i->name, "add") || cstone_i_equal(i->name, "sub") || cstone_i_equal(i->name, "adds") ||
        cstone_i_equal(i->name, "subs") || cstone_i_equal(i->name, "cmp") || cstone_i_equal(i->name, "cmn")) {
        int compare = cstone_i_equal(i->name, "cmp") || cstone_i_equal(i->name, "cmn"),
            subtract = cstone_i_equal(i->name, "sub") || cstone_i_equal(i->name, "subs") ||
                       cstone_i_equal(i->name, "cmp");
        int flags = compare || cstone_i_equal(i->name, "adds") || cstone_i_equal(i->name, "subs");
        size_t src = compare ? 0u : 1u;
        if (i->count < 2 + src || i->count > 3 + src)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Invalid arithmetic operand count.");
        if (!reg_get(c, i->op[src], &b))
            return 0;
        if (compare) {
            a.id = 31;
            a.width = b.width;
        } else if (!reg_get(c, i->op[0], &a))
            return 0;
        if (a.width != b.width)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Register widths must match.");
        if (reg_name(i->op[src + 1], &d)) {
            if (a.sp || b.sp || d.sp || d.width != a.width)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                     "Shifted-register arithmetic cannot use SP.");
            if (i->count == 3 + src && !shift(c, i->op[src + 2], "lsl", a.width - 1, &amount))
                return 0;
            word = 0x0b000000u | (d.id << 16) | ((uint32_t)amount << 10);
        } else {
            if ((a.id == 31 && !flags && !a.sp) || (b.id == 31 && !b.sp))
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                     "Immediate arithmetic uses SP, not ZR.");
            if (i->op[src + 1].n > 6 && cstone_i_equal(cstone_i_slice(i->op[src + 1], 0, 6), ":lo12:")) {
                size_t offset = c->out->size;
                cstone_i_span target = cstone_i_slice(i->op[src + 1], 6, i->op[src + 1].n - 6);
                if (subtract || flags || i->count != 3)
                    return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                         ":lo12: is supported by plain ADD only.");
                word = 0x11000000u | (a.width == 64 ? 0x80000000u : 0u) | (b.id << 5) | a.id;
                return cstone_i_emit_word(c, word, i->name.col) &&
                       cstone_i_reference(c, target, CSTONE_A64_LO12, offset, 0, 0);
            }
            if (!cstone_i_unsigned(c, i->op[src + 1], 12, &value))
                return 0;
            if (i->count == 3 + src &&
                (!shift(c, i->op[src + 2], "lsl", 12, &amount) || (amount != 0 && amount != 12)))
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                     "Immediate shift must be 0 or 12.");
            word = 0x11000000u | ((uint32_t)value << 10) | (amount ? 1u << 22 : 0u);
        }
        word |= (a.width == 64 ? 0x80000000u : 0u) | (subtract ? 0x40000000u : 0u) |
                (flags ? 0x20000000u : 0u) | (b.id << 5) | a.id;
        if (flags && a.sp)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col,
                                 "Flag-setting destination cannot be SP.");
        return cstone_i_emit_word(c, word, i->name.col);
    }
    {
        static const char *const names[] = {"and", "orr", "eor", "ands"};
        for (k = 0; k < 4; ++k)
            if (cstone_i_equal(i->name, names[k])) {
                if ((i->count != 3 && i->count != 4) || !reg_get(c, i->op[0], &a) ||
                    !reg_get(c, i->op[1], &b))
                    return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Invalid logical operands.");
                if (a.width != b.width || b.sp)
                    return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                         "Logical registers must have equal width and cannot be SP.");
                if (reg_name(i->op[2], &d)) {
                    if (d.width != a.width || d.sp || a.sp)
                        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[2].col,
                                             "Invalid source register.");
                    if (i->count == 4 && !shift(c, i->op[3], "lsl", a.width - 1, &amount))
                        return 0;
                    word = 0x0a000000u | (d.id << 16) | ((uint32_t)amount << 10);
                } else {
                    cstone_i_num num;
                    uint32_t encoding;
                    if (k == 3 ? a.sp : a.id == 31 && !a.sp)
                        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col,
                                             "Invalid SP/ZR role for logical-immediate destination.");
                    if (i->count != 3 || !cstone_i_number(c, i->op[2], &num))
                        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                             "Invalid logical immediate.");
                    value = cstone_i_bits(num);
                    if (a.width == 32) {
                        if (num.magnitude > (num.negative ? UINT64_C(0x80000000) : UINT64_C(0xffffffff)))
                            return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[2].col,
                                                 "Immediate exceeds W register.");
                        value &= UINT32_MAX;
                    }
                    if (!logical_immediate(value, a.width, &encoding))
                        return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[2].col,
                                             "Immediate is not an AArch64 bitmask.");
                    word = 0x12000000u | encoding;
                }
                return cstone_i_emit_word(
                    c, word | (a.width == 64 ? 0x80000000u : 0u) | (k << 29) | (b.id << 5) | a.id,
                    i->name.col);
            }
    }
    if (cstone_i_equal(i->name, "lsl") || cstone_i_equal(i->name, "lsr") || cstone_i_equal(i->name, "asr")) {
        if (!cstone_i_count(c, i, 3) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &b) ||
            !cstone_i_unsigned(c, i->op[2], 6, &value))
            return 0;
        if (a.sp || b.sp || a.width != b.width || value >= a.width)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Invalid shift operands.");
        word = cstone_i_equal(i->name, "asr") ? 0x13000000u : 0x53000000u;
        word |= a.width == 64 ? 0x80400000u : 0u;
        amount = cstone_i_equal(i->name, "lsl") ? (a.width - value) % a.width : value;
        value = cstone_i_equal(i->name, "lsl") ? a.width - 1 - value : a.width - 1;
        return cstone_i_emit_word(
            c, word | ((uint32_t)amount << 16) | ((uint32_t)value << 10) | (b.id << 5) | a.id, i->name.col);
    }
    if (cstone_i_equal(i->name, "ldr") || cstone_i_equal(i->name, "str") || cstone_i_equal(i->name, "ldrb") ||
        cstone_i_equal(i->name, "strb") || cstone_i_equal(i->name, "ldrh") ||
        cstone_i_equal(i->name, "strh") || cstone_i_equal(i->name, "ldur") ||
        cstone_i_equal(i->name, "stur")) {
        int load = cstone_i_equal(i->name, "ldr") || cstone_i_equal(i->name, "ldrb") ||
                   cstone_i_equal(i->name, "ldrh") || cstone_i_equal(i->name, "ldur"),
            pre = 0, unscaled = cstone_i_equal(i->name, "ldur") || cstone_i_equal(i->name, "stur");
        unsigned size;
        if ((i->count != 2 && i->count != 3) || !reg_get(c, i->op[0], &a))
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Invalid load/store operands.");
        if (a.sp)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col,
                                 "Load/store data register cannot be SP.");
        if (i->count == 2 && i->op[1].p[0] != '[' && cstone_i_equal(i->name, "ldr"))
            return branch(c, i, (a.width == 64 ? 0x58000000u : 0x18000000u) | a.id, i->op[1],
                          CSTONE_A64_BRANCH19);
        if (!mem(c, i->op[1], &b, &signed_value, &pre))
            return 0;
        size = cstone_i_equal(i->name, "ldrb") || cstone_i_equal(i->name, "strb")   ? 0u
               : cstone_i_equal(i->name, "ldrh") || cstone_i_equal(i->name, "strh") ? 1u
               : a.width == 64                                                      ? 3u
                                                                                    : 2u;
        if (size < 2 && a.width != 32)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col,
                                 "Byte/halfword loads require a W register.");
        if (i->count == 3) {
            if (pre || signed_value)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                     "Post-index offset must follow [base].");
            if (!cstone_i_signed(c, i->op[2], 9, &signed_value))
                return 0;
        }
        if (pre || i->count == 3 || unscaled) {
            if (signed_value < -256 || signed_value > 255)
                return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[1].col,
                                     "Unscaled offset exceeds signed 9 bits.");
            word = 0x38000000u | (((uint32_t)signed_value & 511u) << 12) |
                   (pre             ? 0xc00u
                    : i->count == 3 ? 0x400u
                                    : 0u);
            if ((pre || i->count == 3) && !b.sp && a.id == b.id)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                     "Writeback base overlaps the data register.");
        } else {
            unsigned scale = 1u << size;
            if (signed_value < 0 || (uint64_t)signed_value > 4095u * scale || signed_value % scale)
                return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[1].col,
                                     "Unsigned offset is out of range or unaligned.");
            word = 0x39000000u | ((uint32_t)(signed_value / scale) << 10);
        }
        return cstone_i_emit_word(c, word | (size << 30) | (load ? 0x00400000u : 0u) | (b.id << 5) | a.id,
                                  i->name.col);
    }
    if (cstone_i_equal(i->name, "ldp") || cstone_i_equal(i->name, "stp")) {
        int pre = 0, load = cstone_i_equal(i->name, "ldp");
        unsigned scale;
        if ((i->count != 3 && i->count != 4) || !reg_get(c, i->op[0], &a) || !reg_get(c, i->op[1], &d) ||
            !mem(c, i->op[2], &b, &signed_value, &pre))
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Invalid pair load/store operands.");
        if (a.width != d.width || a.sp || d.sp)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                 "Pair data registers must have equal width.");
        if (i->count == 4) {
            if (pre || signed_value)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Invalid pair post-index form.");
            if (!cstone_i_signed(c, i->op[3], 32, &signed_value))
                return 0;
        }
        scale = a.width / 8;
        if (signed_value % scale || signed_value / scale < -64 || signed_value / scale > 63)
            return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[2].col,
                                 "Pair offset is out of range or unaligned.");
        if ((pre || i->count == 4) && !b.sp && (a.id == b.id || d.id == b.id))
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                 "Writeback base overlaps a data register.");
        if (load && a.id == d.id)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Load-pair registers must differ.");
        word = i->count == 4 ? 0x28800000u : pre ? 0x29800000u : 0x29000000u;
        return cstone_i_emit_word(c,
                                  word | (a.width == 64 ? 0x80000000u : 0u) | (load ? 0x00400000u : 0u) |
                                      (((uint32_t)(signed_value / scale) & 127u) << 15) | (d.id << 10) |
                                      (b.id << 5) | a.id,
                                  i->name.col);
    }
    if (cstone_i_equal(i->name, "mul") || cstone_i_equal(i->name, "mneg") ||
        cstone_i_equal(i->name, "madd") || cstone_i_equal(i->name, "msub") ||
        cstone_i_equal(i->name, "sdiv") || cstone_i_equal(i->name, "udiv")) {
        a64reg accumulator = {31, 0, 0};
        int multiply_add = cstone_i_equal(i->name, "madd") || cstone_i_equal(i->name, "msub");
        if (!cstone_i_count(c, i, multiply_add ? 4u : 3u) || !reg_get(c, i->op[0], &a) ||
            !reg_get(c, i->op[1], &b) || !reg_get(c, i->op[2], &d))
            return 0;
        accumulator.width = a.width;
        if (multiply_add && !reg_get(c, i->op[3], &accumulator))
            return 0;
        if (a.sp || b.sp || d.sp || accumulator.sp || a.width != b.width || a.width != d.width ||
            a.width != accumulator.width)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                 "Multiply/divide registers must match and cannot be SP.");
        if (cstone_i_equal(i->name, "sdiv") || cstone_i_equal(i->name, "udiv"))
            word = cstone_i_equal(i->name, "sdiv") ? 0x1ac00c00u : 0x1ac00800u;
        else
            word = 0x1b000000u | (accumulator.id << 10) |
                   (cstone_i_equal(i->name, "msub") || cstone_i_equal(i->name, "mneg") ? 0x8000u : 0u);
        return cstone_i_emit_word(
            c, word | (a.width == 64 ? 0x80000000u : 0u) | (d.id << 16) | (b.id << 5) | a.id, i->name.col);
    }
    return cstone_i_fail(c, CSTONE_UNSUPPORTED_INSTRUCTION, i->name.col,
                         "Unsupported AArch64 instruction '%.*s'.", (int)(i->name.n > 48 ? 48 : i->name.n),
                         i->name.p);
}
