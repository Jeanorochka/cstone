#include "internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cstone_i_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}
static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}
static int id_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '$';
}
static int id_part(char c) {
    return id_start(c) || (c >= '0' && c <= '9');
}
int cstone_i_equal(cstone_i_span s, const char *text) {
    size_t k;
    if (s.n != strlen(text))
        return 0;
    for (k = 0; k < s.n; ++k)
        if (lower(s.p[k]) != text[k])
            return 0;
    return 1;
}
int cstone_i_identifier(cstone_i_span s) {
    size_t k;
    if (!s.n || !id_start(s.p[0]))
        return 0;
    for (k = 1; k < s.n; ++k)
        if (!id_part(s.p[k]))
            return 0;
    return 1;
}
cstone_i_span cstone_i_trim(cstone_i_span s) {
    while (s.n && cstone_i_space(*s.p)) {
        ++s.p;
        --s.n;
        ++s.col;
    }
    while (s.n && cstone_i_space(s.p[s.n - 1]))
        --s.n;
    return s;
}
cstone_i_span cstone_i_slice(cstone_i_span s, size_t start, size_t length) {
    cstone_i_span result = {s.p + start, length, s.col + start};
    return result;
}
int cstone_i_fail(cstone_i_context *c, cstone_status status, size_t column, const char *format, ...) {
    va_list args;
    cstone_diagnostic *d = &c->out->diagnostic;
    if (d->status != CSTONE_OK)
        return 0;
    d->status = status;
    d->line = c->line;
    d->column = column;
    va_start(args, format);
    (void)vsnprintf(d->message, sizeof(d->message), format, args);
    va_end(args);
    return 0;
}
void *cstone_i_reserve(void *old, size_t *capacity, size_t width, size_t need) {
    size_t n = *capacity ? *capacity : 16;
    void *p;
    if (need <= *capacity)
        return old;
    if (need > SIZE_MAX / width)
        return NULL;
    while (n < need) {
        if (n > SIZE_MAX / 2) {
            n = need;
            break;
        }
        n *= 2;
    }
    if (n > SIZE_MAX / width)
        n = need;
    p = realloc(old, n * width);
    if (p)
        *capacity = n;
    return p;
}
char *cstone_i_copy(cstone_i_span s) {
    char *p;
    if (s.n == SIZE_MAX)
        return NULL;
    p = (char *)malloc(s.n + 1);
    if (p) {
        memcpy(p, s.p, s.n);
        p[s.n] = 0;
    }
    return p;
}
int cstone_i_number(cstone_i_context *c, cstone_i_span s, cstone_i_num *out) {
    size_t k = 0;
    unsigned radix = 10;
    uint64_t value = 0;
    int negative = 0;
    out->magnitude = 0;
    out->negative = 0;
    s = cstone_i_trim(s);
    if (s.n && s.p[0] == '#')
        s = cstone_i_trim(cstone_i_slice(s, 1, s.n - 1));
    if (k < s.n && (s.p[k] == '+' || s.p[k] == '-')) {
        negative = s.p[k] == '-';
        ++k;
    }
    if (k + 1 < s.n && s.p[k] == '0' && lower(s.p[k + 1]) == 'x') {
        radix = 16;
        k += 2;
    } else if (k + 1 < s.n && s.p[k] == '0' && lower(s.p[k + 1]) == 'b') {
        radix = 2;
        k += 2;
    }
    if (k == s.n)
        return cstone_i_fail(c, CSTONE_SYNTAX_ERROR, s.col, "Expected integer digits.");
    for (; k < s.n; ++k) {
        unsigned digit;
        char ch = lower(s.p[k]);
        if (ch >= '0' && ch <= '9')
            digit = (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f')
            digit = (unsigned)(ch - 'a') + 10u;
        else
            return cstone_i_fail(c, CSTONE_SYNTAX_ERROR, s.col + k, "Invalid character in integer.");
        if (digit >= radix)
            return cstone_i_fail(c, CSTONE_SYNTAX_ERROR, s.col + k, "Invalid digit for integer base.");
        if (value > (UINT64_MAX - digit) / radix)
            return cstone_i_fail(c, CSTONE_RANGE_ERROR, s.col, "Integer exceeds uint64_t.");
        value = value * radix + digit;
    }
    if (negative && value > UINT64_C(0x8000000000000000))
        return cstone_i_fail(c, CSTONE_RANGE_ERROR, s.col, "Integer is below INT64_MIN.");
    out->magnitude = value;
    out->negative = negative && value != 0;
    return 1;
}
uint64_t cstone_i_bits(cstone_i_num n) {
    return n.negative ? UINT64_C(0) - n.magnitude : n.magnitude;
}
int cstone_i_unsigned(cstone_i_context *c, cstone_i_span s, unsigned width, uint64_t *out) {
    cstone_i_num n;
    if (!cstone_i_number(c, s, &n))
        return 0;
    if (n.negative || (width < 64 && n.magnitude >= (UINT64_C(1) << width)))
        return cstone_i_fail(c, CSTONE_RANGE_ERROR, s.col, "Expected an unsigned %u-bit integer.", width);
    *out = n.magnitude;
    return 1;
}
int cstone_i_signed(cstone_i_context *c, cstone_i_span s, unsigned width, int64_t *out) {
    cstone_i_num n;
    uint64_t limit = UINT64_C(1) << (width - 1);
    if (!cstone_i_number(c, s, &n))
        return 0;
    if (n.magnitude > (n.negative ? limit : limit - 1))
        return cstone_i_fail(c, CSTONE_RANGE_ERROR, s.col, "Expected a signed %u-bit integer.", width);
    *out = n.negative ? -(int64_t)(n.magnitude - 1) - 1 : (int64_t)n.magnitude;
    return 1;
}
int cstone_i_big_endian(cstone_arch arch) {
    return arch == CSTONE_MIPS32_BE || arch == CSTONE_MIPS64_BE;
}
int cstone_i_arch32(cstone_arch arch) {
    return arch == CSTONE_X86_32 || arch == CSTONE_ARM || arch == CSTONE_THUMB || arch == CSTONE_RISCV32 ||
           arch == CSTONE_MIPS32_LE || arch == CSTONE_MIPS32_BE;
}
void cstone_i_put(uint8_t *data, uint64_t value, unsigned size, int big) {
    unsigned k;
    for (k = 0; k < size; ++k) {
        data[big ? size - k - 1 : k] = (uint8_t)value;
        value >>= 8;
    }
}
uint32_t cstone_i_get32(const uint8_t *data, int big) {
    uint32_t result = 0;
    unsigned k;
    for (k = 0; k < 4; ++k)
        result |= (uint32_t)data[big ? 3 - k : k] << (k * 8);
    return result;
}
int cstone_i_emit(cstone_i_context *c, const uint8_t *data, size_t size, size_t column) {
    size_t need;
    uint8_t *p;
    if (size > SIZE_MAX - c->out->size)
        return cstone_i_fail(c, CSTONE_RANGE_ERROR, column, "Output size overflows size_t.");
    need = c->out->size + size;
    if (need > c->options.max_output_size)
        return cstone_i_fail(c, CSTONE_LIMIT_ERROR, column, "Output byte limit exceeded.");
    if ((uint64_t)need > UINT64_MAX - c->options.base_address)
        return cstone_i_fail(c, CSTONE_RANGE_ERROR, column,
                             "Base address plus output size overflows uint64_t.");
    if (cstone_i_arch32(c->options.arch) && (uint64_t)need > UINT32_MAX - c->options.base_address)
        return cstone_i_fail(c, CSTONE_RANGE_ERROR, column, "Output exceeds the 32-bit address space.");
    p = (uint8_t *)cstone_i_reserve(c->out->bytes, &c->byte_capacity, 1, need);
    if (!p && need)
        return cstone_i_fail(c, CSTONE_OUT_OF_MEMORY, column, "Cannot allocate output bytes.");
    c->out->bytes = p;
    if (size)
        memcpy(p + c->out->size, data, size);
    c->out->size = need;
    return 1;
}
int cstone_i_emit_word(cstone_i_context *c, uint32_t word, size_t column) {
    uint8_t bytes[4];
    cstone_i_put(bytes, word, 4, cstone_i_big_endian(c->options.arch));
    return cstone_i_emit(c, bytes, 4, column);
}
int cstone_i_emit_half(cstone_i_context *c, uint16_t word, size_t column) {
    uint8_t bytes[2];
    cstone_i_put(bytes, word, 2, 0);
    return cstone_i_emit(c, bytes, 2, column);
}
int cstone_i_count(cstone_i_context *c, const cstone_i_insn *i, size_t count) {
    return i->count == count ||
           cstone_i_fail(c, CSTONE_INVALID_OPERAND, i->count > count ? i->op[count].col : i->name.col,
                         "Expected %zu operand(s).", count);
}
static size_t hash_span(cstone_i_span s) {
    uint64_t h = UINT64_C(14695981039346656037);
    size_t k;
    for (k = 0; k < s.n; ++k) {
        h ^= (unsigned char)s.p[k];
        h *= UINT64_C(1099511628211);
    }
    return (size_t)h;
}
static int rehash(cstone_i_context *c, size_t capacity) {
    size_t *table;
    size_t k, slot;
    if (capacity > SIZE_MAX / sizeof(*table))
        return cstone_i_fail(c, CSTONE_LIMIT_ERROR, 1, "Symbol index is too large.");
    table = (size_t *)calloc(capacity, sizeof(*table));
    if (!table)
        return cstone_i_fail(c, CSTONE_OUT_OF_MEMORY, 1, "Cannot allocate symbol index.");
    for (k = 0; k < c->symbol_count; ++k) {
        cstone_i_span s = {c->symbols[k].name, c->symbols[k].length, 1};
        slot = hash_span(s) & (capacity - 1);
        while (table[slot])
            slot = (slot + 1) & (capacity - 1);
        table[slot] = k + 1;
    }
    free(c->hash);
    c->hash = table;
    c->hash_capacity = capacity;
    return 1;
}
static int symbol_index(cstone_i_context *c, cstone_i_span name, size_t *index) {
    size_t slot;
    cstone_i_symbol *p, *s;
    if (!c->hash_capacity && !rehash(c, 32))
        return 0;
    slot = hash_span(name) & (c->hash_capacity - 1);
    while (c->hash[slot]) {
        size_t k = c->hash[slot] - 1;
        if (c->symbols[k].length == name.n && !memcmp(c->symbols[k].name, name.p, name.n)) {
            *index = k;
            return 1;
        }
        slot = (slot + 1) & (c->hash_capacity - 1);
    }
    if (c->symbol_count >= c->options.max_symbols)
        return cstone_i_fail(c, CSTONE_LIMIT_ERROR, name.col, "Symbol limit exceeded.");
    if (c->symbol_count >= c->hash_capacity / 2) {
        if (c->hash_capacity > SIZE_MAX / 2)
            return cstone_i_fail(c, CSTONE_LIMIT_ERROR, name.col, "Symbol index limit exceeded.");
        if (!rehash(c, c->hash_capacity * 2))
            return 0;
        slot = hash_span(name) & (c->hash_capacity - 1);
        while (c->hash[slot])
            slot = (slot + 1) & (c->hash_capacity - 1);
    }
    p = (cstone_i_symbol *)cstone_i_reserve(c->symbols, &c->symbol_capacity, sizeof(*p), c->symbol_count + 1);
    if (!p)
        return cstone_i_fail(c, CSTONE_OUT_OF_MEMORY, name.col, "Cannot allocate symbols.");
    c->symbols = p;
    s = &p[c->symbol_count];
    memset(s, 0, sizeof(*s));
    s->name = cstone_i_copy(name);
    s->length = name.n;
    if (!s->name)
        return cstone_i_fail(c, CSTONE_OUT_OF_MEMORY, name.col, "Cannot allocate symbol name.");
    *index = c->symbol_count;
    c->hash[slot] = ++c->symbol_count;
    return 1;
}
int cstone_i_define_label(cstone_i_context *c, cstone_i_span name) {
    size_t index;
    cstone_i_symbol *s;
    if (c->options.arch <= CSTONE_X86_32 && cstone_i_x86_is_register(name))
        return cstone_i_fail(c, CSTONE_INVALID_OPERAND, name.col,
                             "A register name cannot be used as a label.");
    if (!symbol_index(c, name, &index))
        return 0;
    s = &c->symbols[index];
    if (s->defined)
        return cstone_i_fail(c, CSTONE_DUPLICATE_LABEL, name.col, "Label is already defined.");
    s->defined = 1;
    s->offset = c->out->size;
    s->order = c->defined_count++;
    return 1;
}
int cstone_i_expression(cstone_i_context *c, cstone_i_span expression, cstone_i_span *symbol,
                        uint64_t *target, int64_t *addend) {
    size_t k;
    cstone_i_num n;
    cstone_i_span rest;
    expression = cstone_i_trim(expression);
    if (expression.n && expression.p[0] == '#')
        expression = cstone_i_trim(cstone_i_slice(expression, 1, expression.n - 1));
    symbol->p = NULL;
    symbol->n = 0;
    symbol->col = expression.col;
    *target = 0;
    *addend = 0;
    if (!expression.n)
        return cstone_i_fail(c, CSTONE_SYNTAX_ERROR, expression.col, "Empty address expression.");
    if (!id_start(expression.p[0])) {
        if (!cstone_i_number(c, expression, &n))
            return 0;
        if (n.negative)
            return cstone_i_fail(c, CSTONE_RANGE_ERROR, expression.col,
                                 "An absolute address cannot be negative.");
        *target = n.magnitude;
        return 1;
    }
    k = 1;
    while (k < expression.n && id_part(expression.p[k]))
        ++k;
    *symbol = cstone_i_slice(expression, 0, k);
    rest = cstone_i_trim(cstone_i_slice(expression, k, expression.n - k));
    if (!rest.n)
        return 1;
    if (rest.p[0] != '+' && rest.p[0] != '-')
        return cstone_i_fail(c, CSTONE_SYNTAX_ERROR, rest.col, "Expected +integer or -integer after symbol.");
    {
        int negative = rest.p[0] == '-';
        rest = cstone_i_trim(cstone_i_slice(rest, 1, rest.n - 1));
        if (!cstone_i_number(c, rest, &n))
            return 0;
        if (n.negative ||
            n.magnitude > (negative ? UINT64_C(0x8000000000000000) : UINT64_C(0x7fffffffffffffff)))
            return cstone_i_fail(c, CSTONE_RANGE_ERROR, rest.col, "Address addend is outside int64_t.");
        *addend = negative && n.magnitude ? -(int64_t)(n.magnitude - 1) - 1 : (int64_t)n.magnitude;
    }
    return 1;
}
int cstone_i_reference(cstone_i_context *c, cstone_i_span expression, cstone_reloc_kind kind, size_t offset,
                       int32_t bias, uint64_t anchor) {
    cstone_i_span symbol;
    uint64_t target;
    int64_t addend;
    size_t index = SIZE_MAX;
    cstone_i_fixup *p, *f;
    if (!cstone_i_expression(c, expression, &symbol, &target, &addend))
        return 0;
    if (symbol.n && !symbol_index(c, symbol, &index))
        return 0;
    if (c->fixup_count >= c->options.max_output_size)
        return cstone_i_fail(c, CSTONE_LIMIT_ERROR, expression.col, "Relocation limit exceeded.");
    p = (cstone_i_fixup *)cstone_i_reserve(c->fixups, &c->fixup_capacity, sizeof(*p), c->fixup_count + 1);
    if (!p)
        return cstone_i_fail(c, CSTONE_OUT_OF_MEMORY, expression.col, "Cannot allocate relocation.");
    c->fixups = p;
    f = &p[c->fixup_count++];
    memset(f, 0, sizeof(*f));
    f->symbol = index;
    f->target = target;
    f->reloc.kind = kind;
    f->reloc.offset = offset;
    f->reloc.addend = addend;
    f->reloc.pc_bias = bias;
    f->reloc.place = c->options.base_address + (uint64_t)offset;
    f->reloc.anchor = anchor;
    f->reloc.line = c->line;
    f->reloc.column = expression.col;
    return 1;
}
int cstone_i_finish(cstone_i_context *c) {
    size_t k, external_count = 0;
    if (c->defined_count) {
        c->out->labels = (cstone_label *)calloc(c->defined_count, sizeof(*c->out->labels));
        if (!c->out->labels)
            return cstone_i_fail(c, CSTONE_OUT_OF_MEMORY, 1, "Cannot allocate exported labels.");
        c->out->label_count = c->defined_count;
        for (k = 0; k < c->symbol_count; ++k)
            if (c->symbols[k].defined) {
                cstone_i_symbol *s = &c->symbols[k];
                cstone_label *label = &c->out->labels[s->order];
                cstone_i_span name = {s->name, s->length, 1};
                label->name = cstone_i_copy(name);
                if (!label->name)
                    return cstone_i_fail(c, CSTONE_OUT_OF_MEMORY, 1, "Cannot export label name.");
                label->offset = s->offset;
                label->address = c->options.base_address + (uint64_t)s->offset;
            }
    }
    for (k = 0; k < c->fixup_count; ++k)
        if (c->fixups[k].symbol != SIZE_MAX && !c->symbols[c->fixups[k].symbol].defined)
            ++external_count;
    if (external_count && (c->options.flags & CSTONE_ALLOW_UNRESOLVED)) {
        c->out->relocations = (cstone_relocation *)calloc(external_count, sizeof(*c->out->relocations));
        if (!c->out->relocations)
            return cstone_i_fail(c, CSTONE_OUT_OF_MEMORY, 1, "Cannot export relocations.");
    }
    for (k = 0; k < c->fixup_count; ++k) {
        cstone_i_fixup *f = &c->fixups[k];
        uint64_t target = f->target;
        if (f->symbol != SIZE_MAX) {
            cstone_i_symbol *s = &c->symbols[f->symbol];
            if (!s->defined) {
                cstone_relocation *r;
                cstone_i_span name = {s->name, s->length, f->reloc.column};
                c->line = f->reloc.line;
                if (!(c->options.flags & CSTONE_ALLOW_UNRESOLVED))
                    return cstone_i_fail(c, CSTONE_UNKNOWN_LABEL, f->reloc.column, "Undefined label '%.*s'.",
                                         (int)(s->length > 96 ? 96 : s->length), s->name);
                r = &c->out->relocations[c->out->relocation_count];
                *r = f->reloc;
                r->symbol = cstone_i_copy(name);
                if (!r->symbol)
                    return cstone_i_fail(c, CSTONE_OUT_OF_MEMORY, f->reloc.column,
                                         "Cannot export relocation symbol.");
                ++c->out->relocation_count;
                continue;
            }
            target = c->options.base_address + (uint64_t)s->offset;
        }
        if (!cstone_i_patch(c->out->bytes, c->out->size, c->options.arch, &f->reloc, target)) {
            c->line = f->reloc.line;
            return cstone_i_fail(c, CSTONE_RANGE_ERROR, f->reloc.column,
                                 "Address reference is out of range or has invalid alignment.");
        }
    }
    return 1;
}
void cstone_i_cleanup(cstone_i_context *c) {
    size_t k;
    for (k = 0; k < c->symbol_count; ++k)
        free(c->symbols[k].name);
    free(c->symbols);
    free(c->hash);
    free(c->fixups);
}
