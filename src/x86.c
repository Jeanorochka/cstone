#include "internal.h"
#include <string.h>

typedef struct reg {
    unsigned id, width;
    int high, force;
} reg;
typedef enum x86_kind { REG, IMM, SYMBOL, MEMORY } x86_kind;
typedef struct operand {
    x86_kind type;
    cstone_i_span text;
    reg r;
    cstone_i_num number;
    int base, index, rip;
    unsigned scale, address_width, width;
    int64_t displacement;
    cstone_i_span reference;
} operand;

static int get_reg(cstone_i_span s, reg *r) {
    static const char *const names[4][16] = {{"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b",
                                              "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"},
                                             {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w",
                                              "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"},
                                             {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d",
                                              "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"},
                                             {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8",
                                              "r9", "r10", "r11", "r12", "r13", "r14", "r15"}};
    static const char *const high[] = {"ah", "ch", "dh", "bh"};
    unsigned k, w;
    memset(r, 0, sizeof(*r));
    for (w = 0; w < 4; ++w)
        for (k = 0; k < 16; ++k)
            if (cstone_i_equal(s, names[w][k])) {
                r->id = k;
                r->width = 8u << w;
                r->force = w == 0 && k >= 4;
                return 1;
            }
    for (k = 0; k < 4; ++k)
        if (cstone_i_equal(s, high[k])) {
            r->id = k + 4;
            r->width = 8;
            r->high = 1;
            return 1;
        }
    return 0;
}
static int valid_reg(cstone_i_context *c, reg r, size_t column) {
    return c->options.arch == CSTONE_X86_64 || (r.width != 64 && r.id < 8 && !r.force) ||
           cstone_i_fail(c, CSTONE_INVALID_OPERAND, column, "Register is unavailable in 32-bit mode.");
}
int cstone_i_x86_is_register(cstone_i_span name) {
    reg r;
    return get_reg(name, &r) || cstone_i_equal(name, "rip") || cstone_i_equal(name, "eip");
}
static int signed_fits(cstone_i_num n, unsigned width) {
    return n.magnitude <= ((UINT64_C(1) << (width - 1)) - (n.negative ? 0u : 1u));
}
static int immediate_fits(cstone_i_num n, unsigned width) {
    return width == 64 ||
           n.magnitude <= (n.negative ? UINT64_C(1) << (width - 1) : (UINT64_C(1) << width) - 1);
}
static int short_imm(cstone_i_num n, unsigned width) {
    uint64_t value = cstone_i_bits(n);
    if (width < 8 || width > 64)
        return 0;
    if (width < 64)
        value &= (UINT64_C(1) << width) - 1;
    return value <= 127 || value >= (width >= 64 ? UINT64_MAX - 127 : (UINT64_C(1) << width) - 128);
}
static int memory(cstone_i_context *c, cstone_i_span s, operand *o) {
    size_t open = 0, position;
    while (open < s.n && s.p[open] != '[')
        ++open;
    if (open == s.n || s.p[s.n - 1] != ']')
        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, s.col, "Malformed memory operand.");
    if (open) {
        cstone_i_span prefix = cstone_i_trim(cstone_i_slice(s, 0, open));
        if (cstone_i_equal(prefix, "byte") || cstone_i_equal(prefix, "byte ptr"))
            o->width = 8;
        else if (cstone_i_equal(prefix, "word") || cstone_i_equal(prefix, "word ptr"))
            o->width = 16;
        else if (cstone_i_equal(prefix, "dword") || cstone_i_equal(prefix, "dword ptr"))
            o->width = 32;
        else if (cstone_i_equal(prefix, "qword") || cstone_i_equal(prefix, "qword ptr"))
            o->width = 64;
        else
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, prefix.col, "Unknown memory size prefix.");
    }
    s = cstone_i_trim(cstone_i_slice(s, open + 1, s.n - open - 2));
    o->base = -1;
    o->index = -1;
    o->scale = 1;
    if (!s.n)
        return cstone_i_fail(c, CSTONE_SYNTAX_ERROR, s.col, "Empty memory address.");
    position = 0;
    while (position < s.n) {
        size_t end, star;
        int negative = 0;
        cstone_i_span term;
        reg r;
        while (position < s.n && cstone_i_space(s.p[position]))
            ++position;
        if (position < s.n && (s.p[position] == '+' || s.p[position] == '-')) {
            negative = s.p[position] == '-';
            ++position;
        }
        end = position;
        while (end < s.n && s.p[end] != '+' && s.p[end] != '-')
            ++end;
        term = cstone_i_trim(cstone_i_slice(s, position, end - position));
        if (!term.n)
            return cstone_i_fail(c, CSTONE_SYNTAX_ERROR, term.col, "Empty memory address term.");
        star = 0;
        while (star < term.n && term.p[star] != '*')
            ++star;
        if (get_reg(cstone_i_trim(cstone_i_slice(term, 0, star)), &r)) {
            if (negative || r.width < 32 || !valid_reg(c, r, term.col))
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, term.col, "Invalid address register.");
            if (o->rip || (o->address_width && o->address_width != r.width))
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, term.col,
                                     "Address register widths must match.");
            o->address_width = r.width;
            if (star < term.n) {
                uint64_t scale;
                if (!cstone_i_unsigned(c, cstone_i_trim(cstone_i_slice(term, star + 1, term.n - star - 1)), 4,
                                       &scale))
                    return 0;
                if ((scale != 1 && scale != 2 && scale != 4 && scale != 8) || o->index >= 0 || r.id == 4)
                    return cstone_i_fail(c, CSTONE_INVALID_OPERAND, term.col, "Invalid SIB index or scale.");
                o->index = (int)r.id;
                o->scale = (unsigned)scale;
            } else if (o->base < 0)
                o->base = (int)r.id;
            else if (o->index < 0 && r.id != 4)
                o->index = (int)r.id;
            else
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, term.col, "Too many address registers.");
        } else if (cstone_i_equal(term, "rip")) {
            if (negative || c->options.arch != CSTONE_X86_64 || o->base >= 0 || o->index >= 0 || o->rip)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, term.col, "Invalid RIP-relative address.");
            o->rip = 1;
            o->address_width = 64;
        } else if ((term.p[0] >= '0' && term.p[0] <= '9')) {
            cstone_i_num n;
            int64_t v;
            if (!cstone_i_number(c, term, &n))
                return 0;
            if (n.magnitude > UINT64_C(0xffffffff))
                return cstone_i_fail(c, CSTONE_RANGE_ERROR, term.col, "Address displacement is too large.");
            v = negative ? -(int64_t)n.magnitude : (int64_t)n.magnitude;
            if (o->displacement > INT64_MAX - (v > 0 ? v : 0) ||
                o->displacement < INT64_MIN - (v < 0 ? v : 0))
                return cstone_i_fail(c, CSTONE_RANGE_ERROR, term.col, "Address displacement overflow.");
            o->displacement += v;
        } else {
            cstone_i_span symbol;
            uint64_t target;
            int64_t addend;
            if (negative || o->reference.n || o->displacement)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, term.col,
                                     "Use base + symbol + addend for symbolic memory.");
            o->reference = cstone_i_trim(cstone_i_slice(s, position, s.n - position));
            if (!cstone_i_expression(c, o->reference, &symbol, &target, &addend) || !symbol.n)
                return 0;
            break;
        }
        position = end;
    }
    if (o->rip && o->index >= 0)
        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, s.col, "RIP cannot have an index.");
    if (!o->address_width)
        o->address_width = c->options.arch == CSTONE_X86_64 ? 64u : 32u;
    if (o->displacement < INT32_MIN ||
        o->displacement >
            ((o->base < 0 && !o->rip && o->address_width == 32) ? (int64_t)UINT32_MAX : INT32_MAX))
        return cstone_i_fail(c, CSTONE_RANGE_ERROR, s.col,
                             "Memory displacement does not fit the address form.");
    return 1;
}
static int parse(cstone_i_context *c, cstone_i_span s, operand *o) {
    memset(o, 0, sizeof(*o));
    o->text = s;
    if (get_reg(s, &o->r)) {
        o->type = REG;
        o->width = o->r.width;
        return valid_reg(c, o->r, s.col);
    }
    if (memchr(s.p, '[', s.n)) {
        o->type = MEMORY;
        return memory(c, s, o);
    }
    if (s.n && ((s.p[0] >= '0' && s.p[0] <= '9') || s.p[0] == '-' || s.p[0] == '+')) {
        o->type = IMM;
        return cstone_i_number(c, s, &o->number);
    }
    {
        cstone_i_span symbol;
        uint64_t target;
        int64_t addend;
        o->type = SYMBOL;
        return cstone_i_expression(c, s, &symbol, &target, &addend);
    }
}
static int modrm(cstone_i_context *c, const uint8_t *opcodes, unsigned opcode_count, unsigned width,
                 unsigned field, reg field_reg, const operand *rm, uint64_t immediate,
                 unsigned immediate_size, cstone_i_span immediate_ref, size_t column) {
    uint8_t data[24];
    unsigned n = 0, rex = 0x40, mode = 3, low = 0, displacement_size = 0, displacement_offset = 0,
             immediate_offset;
    uint32_t displacement = (uint32_t)rm->displacement;
    int sib = 0;
    unsigned sib_byte = 0;
    int force = field_reg.force || (rm->type == REG && rm->r.force);
    int high = field_reg.high || (rm->type == REG && rm->r.high);
    if (rm->type != REG && rm->type != MEMORY)
        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, rm->text.col, "Expected register or memory operand.");
    if (rm->type == MEMORY && rm->address_width == 32 && c->options.arch == CSTONE_X86_64)
        data[n++] = 0x67;
    if (width == 16)
        data[n++] = 0x66;
    if (width == 64)
        rex |= 8;
    if (field >= 8)
        rex |= 4;
    if (rm->type == REG) {
        low = rm->r.id & 7u;
        if (rm->r.id >= 8)
            rex |= 1;
    } else {
        if (rm->base >= 8)
            rex |= 1;
        if (rm->index >= 8)
            rex |= 2;
        if (rm->rip) {
            mode = 0;
            low = 5;
            displacement_size = 4;
        } else {
            int base = rm->base;
            if (base < 0) {
                mode = 0;
                displacement_size = 4;
            } else if (!rm->reference.n && !rm->displacement && (base & 7) != 5)
                mode = 0;
            else if (!rm->reference.n && rm->displacement >= -128 && rm->displacement <= 127) {
                mode = 1;
                displacement_size = 1;
            } else {
                mode = 2;
                displacement_size = 4;
            }
            sib = rm->index >= 0 || (base >= 0 && (base & 7) == 4) ||
                  (base < 0 && c->options.arch == CSTONE_X86_64);
            if (sib) {
                unsigned scale = rm->scale == 8 ? 3u : rm->scale == 4 ? 2u : rm->scale == 2 ? 1u : 0u;
                low = 4;
                sib_byte = (scale << 6) | ((rm->index < 0 ? 4u : (unsigned)rm->index & 7u) << 3) |
                           (base < 0 ? 5u : (unsigned)base & 7u);
            } else
                low = base < 0 ? 5u : (unsigned)base & 7u;
        }
    }
    if (c->options.arch == CSTONE_X86_64 && (rex != 0x40 || force)) {
        if (high)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, column,
                                 "AH/CH/DH/BH cannot be used with a REX prefix.");
        data[n++] = (uint8_t)rex;
    }
    memcpy(data + n, opcodes, opcode_count);
    n += opcode_count;
    data[n++] = (uint8_t)((mode << 6) | ((field & 7u) << 3) | low);
    if (sib)
        data[n++] = (uint8_t)sib_byte;
    if (displacement_size) {
        displacement_offset = n;
        cstone_i_put(data + n, displacement, displacement_size, 0);
        n += displacement_size;
    }
    immediate_offset = n;
    cstone_i_put(data + n, immediate, immediate_size, 0);
    n += immediate_size;
    {
        size_t start = c->out->size;
        if (!cstone_i_emit(c, data, n, column))
            return 0;
        if (rm->reference.n) {
            cstone_reloc_kind kind = rm->rip ? CSTONE_X86_REL32
                                     : c->options.arch == CSTONE_X86_64 && rm->address_width == 64
                                         ? CSTONE_X86_ABS32S
                                         : CSTONE_ABS32;
            if (!cstone_i_reference(c, rm->reference, kind, start + displacement_offset,
                                    rm->rip ? (int32_t)(n - displacement_offset) : 0, 0))
                return 0;
        }
        if (immediate_ref.n) {
            cstone_reloc_kind kind = immediate_size == 1   ? CSTONE_ABS8
                                     : immediate_size == 2 ? CSTONE_ABS16
                                     : immediate_size == 4 ? CSTONE_ABS32
                                                           : CSTONE_ABS64;
            if (!cstone_i_reference(c, immediate_ref, kind, start + immediate_offset, 0, 0))
                return 0;
        }
    }
    return 1;
}
static int condition(cstone_i_span name, const char *prefix) {
    static const char *const suffix[] = {"o", "no", "b", "ae", "e", "ne", "be", "a",
                                         "s", "ns", "p", "np", "l", "ge", "le", "g"};
    size_t n = strlen(prefix);
    unsigned k;
    if (name.n <= n || !cstone_i_equal(cstone_i_slice(name, 0, n), prefix))
        return -1;
    name = cstone_i_slice(name, n, name.n - n);
    for (k = 0; k < 16; ++k)
        if (cstone_i_equal(name, suffix[k]))
            return (int)k;
    if (cstone_i_equal(name, "c") || cstone_i_equal(name, "nae"))
        return 2;
    if (cstone_i_equal(name, "nb") || cstone_i_equal(name, "nc"))
        return 3;
    if (cstone_i_equal(name, "z"))
        return 4;
    if (cstone_i_equal(name, "nz"))
        return 5;
    if (cstone_i_equal(name, "na"))
        return 6;
    if (cstone_i_equal(name, "nbe"))
        return 7;
    if (cstone_i_equal(name, "pe"))
        return 10;
    if (cstone_i_equal(name, "po"))
        return 11;
    if (cstone_i_equal(name, "nge"))
        return 12;
    if (cstone_i_equal(name, "nl"))
        return 13;
    if (cstone_i_equal(name, "ng"))
        return 14;
    if (cstone_i_equal(name, "nle"))
        return 15;
    return -1;
}
int cstone_i_x86(cstone_i_context *c, const cstone_i_insn *i) {
    static const struct {
        const char *name;
        unsigned size;
        uint8_t bytes[4];
    } fixed[] = {{"nop", 1, {0x90}},
                 {"ret", 1, {0xc3}},
                 {"leave", 1, {0xc9}},
                 {"int3", 1, {0xcc}},
                 {"ud2", 2, {0x0f, 0x0b}},
                 {"syscall", 2, {0x0f, 0x05}},
                 {"sysenter", 2, {0x0f, 0x34}},
                 {"cpuid", 2, {0x0f, 0xa2}},
                 {"rdtsc", 2, {0x0f, 0x31}},
                 {"clc", 1, {0xf8}},
                 {"stc", 1, {0xf9}},
                 {"cld", 1, {0xfc}},
                 {"std", 1, {0xfd}},
                 {"pause", 2, {0xf3, 0x90}},
                 {"endbr64", 4, {0xf3, 0x0f, 0x1e, 0xfa}},
                 {"endbr32", 4, {0xf3, 0x0f, 0x1e, 0xfb}}};
    static const char *const binary[] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
    static const char *const shifts[] = {"rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar"};
    operand op[5];
    reg empty = {0, 0, 0, 0};
    cstone_i_span no_ref = {NULL, 0, 0};
    uint8_t code[16], opcode[2];
    unsigned n, k, width = 0;
    int group = -1, cc;
    if (cstone_i_equal(i->name, "ret") && i->count == 1) {
        uint64_t value;
        if (!cstone_i_unsigned(c, i->op[0], 16, &value))
            return 0;
        code[0] = 0xc2;
        cstone_i_put(code + 1, value, 2, 0);
        return cstone_i_emit(c, code, 3, i->name.col);
    }
    for (k = 0; k < sizeof(fixed) / sizeof(fixed[0]); ++k)
        if (cstone_i_equal(i->name, fixed[k].name))
            return cstone_i_count(c, i, 0) && cstone_i_emit(c, fixed[k].bytes, fixed[k].size, i->name.col);
    if (cstone_i_equal(i->name, "int")) {
        uint64_t value;
        if (!cstone_i_count(c, i, 1) || !cstone_i_unsigned(c, i->op[0], 8, &value))
            return 0;
        code[0] = 0xcd;
        code[1] = (uint8_t)value;
        return cstone_i_emit(c, code, 2, i->name.col);
    }
    memset(op, 0, sizeof(op));
    for (k = 0; k < i->count; ++k)
        if (!parse(c, i->op[k], &op[k]))
            return 0;
    cc = condition(i->name, "j");
    if (cstone_i_equal(i->name, "jmp") || cstone_i_equal(i->name, "call") || cc >= 0) {
        size_t offset;
        if (!cstone_i_count(c, i, 1))
            return 0;
        if (op[0].type == REG || op[0].type == MEMORY) {
            unsigned expected = c->options.arch == CSTONE_X86_64 ? 64u : 32u;
            if (cc >= 0 || (op[0].width && op[0].width != expected))
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, op[0].text.col,
                                     "Invalid indirect branch operand.");
            opcode[0] = 0xff;
            return modrm(c, opcode, 1, 32, cstone_i_equal(i->name, "call") ? 2u : 4u, empty, &op[0], 0, 0,
                         no_ref, i->name.col);
        }
        n = 0;
        if (cc >= 0) {
            code[n++] = 0x0f;
            code[n++] = (uint8_t)(0x80 + cc);
        } else
            code[n++] = cstone_i_equal(i->name, "call") ? 0xe8 : 0xe9;
        offset = c->out->size + n;
        memset(code + n, 0, 4);
        n += 4;
        return cstone_i_emit(c, code, n, i->name.col) &&
               cstone_i_reference(c, i->op[0], CSTONE_X86_REL32, offset, 4, 0);
    }
    if (cstone_i_equal(i->name, "push") || cstone_i_equal(i->name, "pop")) {
        int push = cstone_i_equal(i->name, "push");
        unsigned expected = c->options.arch == CSTONE_X86_64 ? 64u : 32u;
        if (!cstone_i_count(c, i, 1))
            return 0;
        if (op[0].type == REG) {
            if (op[0].width != expected && op[0].width != 16)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col, "Invalid stack operand width.");
            n = 0;
            if (op[0].width == 16)
                code[n++] = 0x66;
            if (op[0].r.id >= 8)
                code[n++] = 0x41;
            code[n++] = (uint8_t)((push ? 0x50u : 0x58u) + (op[0].r.id & 7u));
            return cstone_i_emit(c, code, n, i->name.col);
        }
        if (op[0].type == MEMORY) {
            if (op[0].width && op[0].width != expected && op[0].width != 16)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col, "Invalid stack memory width.");
            opcode[0] = push ? 0xff : 0x8f;
            return modrm(c, opcode, 1, op[0].width == 16 ? 16u : 32u, push ? 6u : 0u, empty, &op[0], 0, 0,
                         no_ref, i->name.col);
        }
        if (push && op[0].type == IMM && signed_fits(op[0].number, 32)) {
            unsigned size = short_imm(op[0].number, 32) ? 1u : 4u;
            code[0] = size == 1 ? 0x6a : 0x68;
            cstone_i_put(code + 1, cstone_i_bits(op[0].number), size, 0);
            return cstone_i_emit(c, code, size + 1, i->name.col);
        }
        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col, "Invalid PUSH/POP operand.");
    }
    cc = condition(i->name, "set");
    if (cc >= 0) {
        if (!cstone_i_count(c, i, 1))
            return 0;
        if (op[0].width != 8)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col, "SETcc requires an 8-bit operand.");
        opcode[0] = 0x0f;
        opcode[1] = (uint8_t)(0x90 + cc);
        return modrm(c, opcode, 2, 8, 0, empty, &op[0], 0, 0, no_ref, i->name.col);
    }
    cc = condition(i->name, "cmov");
    if (cc >= 0) {
        if (!cstone_i_count(c, i, 2) || op[0].type != REG || op[0].width == 8)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Invalid CMOV operands.");
        if (op[1].width && op[1].width != op[0].width)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[1].col, "Operand widths must match.");
        opcode[0] = 0x0f;
        opcode[1] = (uint8_t)(0x40 + cc);
        return modrm(c, opcode, 2, op[0].width, op[0].r.id, op[0].r, &op[1], 0, 0, no_ref, i->name.col);
    }
    if (cstone_i_equal(i->name, "lea")) {
        if (!cstone_i_count(c, i, 2) || op[0].type != REG || op[0].width == 8 || op[1].type != MEMORY)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "LEA requires register, memory.");
        opcode[0] = 0x8d;
        return modrm(c, opcode, 1, op[0].width, op[0].r.id, op[0].r, &op[1], 0, 0, no_ref, i->name.col);
    }
    if (cstone_i_equal(i->name, "movzx") || cstone_i_equal(i->name, "movsx") ||
        cstone_i_equal(i->name, "movsxd")) {
        if (!cstone_i_count(c, i, 2) || op[0].type != REG)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Invalid extending move.");
        if (cstone_i_equal(i->name, "movsxd")) {
            if (op[0].width != 64 || op[1].width != 32)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "MOVSXD requires r64, r/m32.");
            opcode[0] = 0x63;
            n = 1;
        } else {
            if ((op[1].width != 8 && op[1].width != 16) || op[0].width <= op[1].width)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                     "Source must have a smaller explicit width.");
            opcode[0] = 0x0f;
            opcode[1] = (uint8_t)((cstone_i_equal(i->name, "movzx") ? 0xb6 : 0xbe) + (op[1].width == 16));
            n = 2;
        }
        return modrm(c, opcode, n, op[0].width, op[0].r.id, op[0].r, &op[1], 0, 0, no_ref, i->name.col);
    }
    if (cstone_i_equal(i->name, "inc") || cstone_i_equal(i->name, "dec") || cstone_i_equal(i->name, "not") ||
        cstone_i_equal(i->name, "neg") || cstone_i_equal(i->name, "mul") || cstone_i_equal(i->name, "div") ||
        cstone_i_equal(i->name, "idiv") || (cstone_i_equal(i->name, "imul") && i->count == 1)) {
        unsigned field;
        if (!cstone_i_count(c, i, 1) || !op[0].width)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                 "Unary memory operand needs an explicit size.");
        if (cstone_i_equal(i->name, "inc") || cstone_i_equal(i->name, "dec")) {
            field = cstone_i_equal(i->name, "dec") ? 1u : 0u;
            opcode[0] = op[0].width == 8 ? 0xfe : 0xff;
        } else {
            field = cstone_i_equal(i->name, "not")    ? 2u
                    : cstone_i_equal(i->name, "neg")  ? 3u
                    : cstone_i_equal(i->name, "mul")  ? 4u
                    : cstone_i_equal(i->name, "imul") ? 5u
                    : cstone_i_equal(i->name, "div")  ? 6u
                                                      : 7u;
            opcode[0] = op[0].width == 8 ? 0xf6 : 0xf7;
        }
        return modrm(c, opcode, 1, op[0].width, field, empty, &op[0], 0, 0, no_ref, i->name.col);
    }
    if (cstone_i_equal(i->name, "imul")) {
        if ((i->count != 2 && i->count != 3) || op[0].type != REG || op[0].width == 8)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                 "IMUL requires a 16/32/64-bit destination.");
        if (op[1].width && op[1].width != op[0].width)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[1].col, "Operand widths must match.");
        if (i->count == 2) {
            opcode[0] = 0x0f;
            opcode[1] = 0xaf;
            return modrm(c, opcode, 2, op[0].width, op[0].r.id, op[0].r, &op[1], 0, 0, no_ref, i->name.col);
        }
        if (op[2].type != IMM || !signed_fits(op[2].number, op[0].width == 16 ? 16u : 32u))
            return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[2].col, "IMUL immediate is out of range.");
        n = short_imm(op[2].number, op[0].width) ? 1u : op[0].width == 16 ? 2u : 4u;
        opcode[0] = n == 1 ? 0x6b : 0x69;
        return modrm(c, opcode, 1, op[0].width, op[0].r.id, op[0].r, &op[1], cstone_i_bits(op[2].number), n,
                     no_ref, i->name.col);
    }
    for (k = 0; k < 8; ++k)
        if (cstone_i_equal(i->name, shifts[k])) {
            unsigned field = k == 6 ? 4u : k;
            if (!cstone_i_count(c, i, 2) || !op[0].width)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                     "Shift requires an explicitly sized operand.");
            if (op[1].type == REG && cstone_i_equal(i->op[1], "cl")) {
                opcode[0] = op[0].width == 8 ? 0xd2 : 0xd3;
                return modrm(c, opcode, 1, op[0].width, field, empty, &op[0], 0, 0, no_ref, i->name.col);
            }
            if (op[1].type != IMM || op[1].number.negative || op[1].number.magnitude > 255)
                return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[1].col,
                                     "Shift count must be CL or an unsigned byte.");
            n = op[1].number.magnitude == 1 ? 0u : 1u;
            opcode[0] = (uint8_t)((n ? 0xc0 : 0xd0) + (op[0].width != 8));
            return modrm(c, opcode, 1, op[0].width, field, empty, &op[0], op[1].number.magnitude, n, no_ref,
                         i->name.col);
        }
    for (k = 0; k < 8; ++k)
        if (cstone_i_equal(i->name, binary[k]))
            group = (int)k;
    if (group >= 0 || cstone_i_equal(i->name, "mov") || cstone_i_equal(i->name, "test") ||
        cstone_i_equal(i->name, "xchg")) {
        int mov = cstone_i_equal(i->name, "mov"), test = cstone_i_equal(i->name, "test"),
            exchange = cstone_i_equal(i->name, "xchg");
        if (!cstone_i_count(c, i, 2))
            return 0;
        if (op[0].type != REG && op[0].type != MEMORY)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[0].col,
                                 "Expected register or memory destination.");
        width = op[0].width ? op[0].width : op[1].width;
        if (!width || (op[0].width && op[0].width != width) || (op[1].width && op[1].width != width))
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[1].col,
                                 "Operand widths must match or memory needs a size.");
        if (width == 64 && c->options.arch != CSTONE_X86_64)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "64-bit operand in 32-bit mode.");
        if (op[1].type == IMM || op[1].type == SYMBOL) {
            if (exchange)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[1].col, "XCHG has no immediate form.");
            if (op[1].type == SYMBOL && !mov)
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->op[1].col,
                                     "Symbolic immediate is supported by MOV only.");
            if (op[1].type == IMM &&
                !(width == 64 && !mov ? signed_fits(op[1].number, 32) : immediate_fits(op[1].number, width)))
                return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[1].col,
                                     "Immediate does not fit this instruction.");
            if (mov && op[0].type == REG) {
                uint64_t value = cstone_i_bits(op[1].number);
                unsigned size = width / 8;
                size_t start = c->out->size;
                if (width == 64 && op[1].type == IMM && signed_fits(op[1].number, 32)) {
                    opcode[0] = 0xc7;
                    return modrm(c, opcode, 1, 64, 0, empty, &op[0], value, 4, no_ref, i->name.col);
                }
                n = 0;
                if (width == 16)
                    code[n++] = 0x66;
                if (c->options.arch == CSTONE_X86_64 && (width == 64 || op[0].r.id >= 8 || op[0].r.force))
                    code[n++] = (uint8_t)(0x40u | (width == 64 ? 8u : 0u) | (op[0].r.id >= 8 ? 1u : 0u));
                code[n++] = (uint8_t)((width == 8 ? 0xb0u : 0xb8u) + (op[0].r.id & 7u));
                k = n;
                cstone_i_put(code + n, value, size, 0);
                n += size;
                if (!cstone_i_emit(c, code, n, i->name.col))
                    return 0;
                if (op[1].type == SYMBOL)
                    return cstone_i_reference(c, i->op[1],
                                              size == 1   ? CSTONE_ABS8
                                              : size == 2 ? CSTONE_ABS16
                                              : size == 4 ? CSTONE_ABS32
                                                          : CSTONE_ABS64,
                                              start + k, 0, 0);
                return 1;
            }
            if (mov && width == 64 && (op[1].type == SYMBOL || !signed_fits(op[1].number, 32)))
                return cstone_i_fail(c, CSTONE_RANGE_ERROR, i->op[1].col,
                                     "MOV memory immediate must fit signed 32 bits.");
            if (mov) {
                opcode[0] = width == 8 ? 0xc6 : 0xc7;
                k = 0;
                n = width == 8 ? 1u : width == 16 ? 2u : 4u;
            } else if (test) {
                opcode[0] = width == 8 ? 0xf6 : 0xf7;
                k = 0;
                n = width == 8 ? 1u : width == 16 ? 2u : 4u;
            } else {
                k = (unsigned)group;
                n = width == 8 || short_imm(op[1].number, width) ? 1u : width == 16 ? 2u : 4u;
                opcode[0] = width == 8 ? 0x80 : n == 1 ? 0x83 : 0x81;
            }
            if (!mov && !test && n > 1 && op[0].type == REG && op[0].r.id == 0) {
                unsigned prefix = 0;
                if (width == 16)
                    code[prefix++] = 0x66;
                if (width == 64)
                    code[prefix++] = 0x48;
                code[prefix++] = (uint8_t)(group * 8 + 5);
                cstone_i_put(code + prefix, cstone_i_bits(op[1].number), n, 0);
                return cstone_i_emit(c, code, prefix + n, i->name.col);
            }
            return modrm(c, opcode, 1, width, k, empty, &op[0], cstone_i_bits(op[1].number), n,
                         op[1].type == SYMBOL ? i->op[1] : no_ref, i->name.col);
        }
        if (op[0].type == MEMORY && op[1].type == MEMORY)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col,
                                 "Memory-to-memory form is not supported.");
        if (op[1].type == REG) {
            opcode[0] = (uint8_t)((mov ? 0x88 : test ? 0x84 : exchange ? 0x86 : group * 8) + (width != 8));
            return modrm(c, opcode, 1, width, op[1].r.id, op[1].r, &op[0], 0, 0, no_ref, i->name.col);
        }
        if (op[0].type == REG && op[1].type == MEMORY) {
            opcode[0] = (uint8_t)((mov        ? 0x8a
                                   : test     ? 0x84
                                   : exchange ? 0x86
                                              : group * 8 + 2) +
                                  (width != 8));
            return modrm(c, opcode, 1, width, op[0].r.id, op[0].r, &op[1], 0, 0, no_ref, i->name.col);
        }
        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->name.col, "Invalid operand form.");
    }
    return cstone_i_fail(c, CSTONE_UNSUPPORTED_INSTRUCTION, i->name.col,
                         "Unsupported x86 instruction '%.*s'.", (int)(i->name.n > 48 ? 48 : i->name.n),
                         i->name.p);
}
