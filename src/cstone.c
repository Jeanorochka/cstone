#include "internal.h"
#include <stdlib.h>
#include <string.h>

static int id_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '.' || ch == '$';
}
static int id_part(char ch) {
    return id_start(ch) || (ch >= '0' && ch <= '9');
}
static int split(cstone_i_context *c, cstone_i_span rest, cstone_i_insn *insn) {
    size_t start = 0, k, depth = 0;
    char stack[32];
    if (!rest.n)
        return 1;
    for (k = 0; k <= rest.n; ++k) {
        char ch = k < rest.n ? rest.p[k] : ',';
        if (ch == '[' || ch == '(' || ch == '{') {
            if (depth == sizeof(stack))
                return cstone_i_fail(c, CSTONE_LIMIT_ERROR, rest.col + k, "Operand nesting limit exceeded.");
            stack[depth++] = ch;
        } else if (ch == ']' || ch == ')' || ch == '}') {
            char expected = ch == ']' ? '[' : ch == ')' ? '(' : '{';
            if (!depth || stack[depth - 1] != expected)
                return cstone_i_fail(c, CSTONE_SYNTAX_ERROR, rest.col + k, "Mismatched operand delimiter.");
            --depth;
        }
        if (ch == ',' && !depth) {
            cstone_i_span op = cstone_i_trim(cstone_i_slice(rest, start, k - start));
            if (!op.n)
                return cstone_i_fail(c, CSTONE_SYNTAX_ERROR, op.col, "Empty operand.");
            if (insn->count == sizeof(insn->op) / sizeof(insn->op[0]))
                return cstone_i_fail(c, CSTONE_INVALID_OPERAND, op.col, "Too many operands.");
            insn->op[insn->count++] = op;
            start = k + 1;
        }
    }
    return !depth || cstone_i_fail(c, CSTONE_SYNTAX_ERROR, rest.col, "Unclosed operand delimiter.");
}
static int directive(cstone_i_context *c, cstone_i_span name, cstone_i_span rest) {
    unsigned width = 0;
    size_t position = 0;
    if (cstone_i_equal(name, "db") || cstone_i_equal(name, ".byte"))
        width = 1;
    else if (cstone_i_equal(name, "dw") || cstone_i_equal(name, ".short") || cstone_i_equal(name, ".hword"))
        width = 2;
    else if (cstone_i_equal(name, "dd") || cstone_i_equal(name, ".long") || cstone_i_equal(name, ".word"))
        width = 4;
    else if (cstone_i_equal(name, "dq") || cstone_i_equal(name, ".quad"))
        width = 8;
    if (width) {
        if (!rest.n)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, rest.col, "Data directive requires an operand.");
        while (position <= rest.n) {
            size_t end = position, offset = c->out->size;
            cstone_i_span part;
            uint8_t data[8] = {0};
            while (end < rest.n && rest.p[end] != ',')
                ++end;
            part = cstone_i_trim(cstone_i_slice(rest, position, end - position));
            if (!part.n)
                return cstone_i_fail(c, CSTONE_SYNTAX_ERROR, part.col, "Empty data operand.");
            if (id_start(part.p[0])) {
                cstone_reloc_kind kind = width == 1   ? CSTONE_ABS8
                                         : width == 2 ? CSTONE_ABS16
                                         : width == 4 ? CSTONE_ABS32
                                                      : CSTONE_ABS64;
                if (!cstone_i_emit(c, data, width, part.col) ||
                    !cstone_i_reference(c, part, kind, offset, 0, 0))
                    return 0;
            } else {
                cstone_i_num n;
                if (!cstone_i_number(c, part, &n))
                    return 0;
                if (width < 8 && n.magnitude > (n.negative ? (UINT64_C(1) << (width * 8 - 1))
                                                           : (UINT64_C(1) << (width * 8)) - 1))
                    return cstone_i_fail(c, CSTONE_RANGE_ERROR, part.col,
                                         "Data operand does not fit %u bits.", width * 8);
                cstone_i_put(data, cstone_i_bits(n), width, cstone_i_big_endian(c->options.arch));
                if (!cstone_i_emit(c, data, width, part.col))
                    return 0;
            }
            if (end == rest.n)
                break;
            position = end + 1;
        }
        return 1;
    }
    if (cstone_i_equal(name, ".align") || cstone_i_equal(name, ".balign") || cstone_i_equal(name, ".zero") ||
        cstone_i_equal(name, ".space")) {
        cstone_i_insn parts;
        uint64_t count, fill = 0;
        uint8_t block[256];
        memset(&parts, 0, sizeof(parts));
        if (!split(c, rest, &parts))
            return 0;
        if (parts.count < 1 || parts.count > 2)
            return cstone_i_fail(c, CSTONE_INVALID_OPERAND, name.col,
                                 "Expected size and optional fill byte.");
        if (!cstone_i_unsigned(c, parts.op[0], 64, &count))
            return 0;
        if (parts.count == 2 && !cstone_i_unsigned(c, parts.op[1], 8, &fill))
            return 0;
        if (cstone_i_equal(name, ".align") || cstone_i_equal(name, ".balign")) {
            if (!count || count > 4096 || (count & (count - 1)))
                return cstone_i_fail(c, CSTONE_RANGE_ERROR, parts.op[0].col,
                                     "Alignment must be a power of two in [1,4096].");
            count =
                (count - ((c->options.base_address + (uint64_t)c->out->size) & (count - 1))) & (count - 1);
        }
        if (count > c->options.max_output_size - c->out->size)
            return cstone_i_fail(c, CSTONE_LIMIT_ERROR, name.col, "Output byte limit exceeded.");
        memset(block, (int)fill, sizeof(block));
        while (count) {
            size_t n = count > sizeof(block) ? sizeof(block) : (size_t)count;
            if (!cstone_i_emit(c, block, n, name.col))
                return 0;
            count -= n;
        }
        return 1;
    }
    if (cstone_i_equal(name, ".text"))
        return !rest.n || cstone_i_fail(c, CSTONE_INVALID_OPERAND, rest.col, ".text takes no operands.");
    return -1;
}
static int parse_line(cstone_i_context *c, const char *text, size_t length) {
    cstone_i_span rest = {text, length, 1}, name = {NULL, 0, 0};
    cstone_i_insn insn;
    size_t k, end;
    int result;
    unsigned alignment;
    int hash_comment = c->options.arch <= CSTONE_X86_32 || c->options.arch >= CSTONE_RISCV32;
    for (k = 0; k < rest.n; ++k)
        if (rest.p[k] == ';' || (hash_comment && rest.p[k] == '#') ||
            (rest.p[k] == '/' && k + 1 < rest.n && rest.p[k + 1] == '/')) {
            rest.n = k;
            break;
        }
    rest = cstone_i_trim(rest);
    if (!rest.n)
        return 1;
    if (c->statements++ >= c->options.max_statements)
        return cstone_i_fail(c, CSTONE_LIMIT_ERROR, rest.col, "Statement limit exceeded.");
    while (rest.n) {
        size_t colon;
        if (!id_start(rest.p[0]))
            return cstone_i_fail(c, CSTONE_SYNTAX_ERROR, rest.col,
                                 "Expected instruction, directive, or label.");
        end = 1;
        while (end < rest.n && id_part(rest.p[end]))
            ++end;
        name = cstone_i_slice(rest, 0, end);
        colon = end;
        while (colon < rest.n && cstone_i_space(rest.p[colon]))
            ++colon;
        if (colon == rest.n || rest.p[colon] != ':')
            break;
        if (!cstone_i_define_label(c, name))
            return 0;
        rest = cstone_i_trim(cstone_i_slice(rest, colon + 1, rest.n - colon - 1));
    }
    if (!rest.n)
        return 1;
    if (name.n < rest.n && !cstone_i_space(rest.p[name.n]))
        return cstone_i_fail(c, CSTONE_SYNTAX_ERROR, rest.col + name.n,
                             "Expected whitespace after instruction.");
    rest = cstone_i_trim(cstone_i_slice(rest, name.n, rest.n - name.n));
    result = directive(c, name, rest);
    if (result >= 0)
        return result;
    memset(&insn, 0, sizeof(insn));
    insn.name = name;
    if (!split(c, rest, &insn))
        return 0;
    alignment = c->options.arch <= CSTONE_X86_32 ? 1u : c->options.arch == CSTONE_THUMB ? 2u : 4u;
    if ((c->options.base_address + (uint64_t)c->out->size) % alignment)
        return cstone_i_fail(c, CSTONE_UNALIGNED_ADDRESS, name.col, "Instruction requires %u-byte alignment.",
                             alignment);
    switch (c->options.arch) {
    case CSTONE_X86_64:
    case CSTONE_X86_32:
        return cstone_i_x86(c, &insn);
    case CSTONE_AARCH64:
        return cstone_i_aarch64(c, &insn);
    case CSTONE_ARM:
        return cstone_i_arm(c, &insn);
    case CSTONE_THUMB:
        return cstone_i_thumb(c, &insn);
    case CSTONE_RISCV32:
    case CSTONE_RISCV64:
        return cstone_i_riscv(c, &insn);
    default:
        return cstone_i_mips(c, &insn);
    }
}
void cstone_options_init(cstone_options *options) {
    if (!options)
        return;
    memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
    options->max_source_size = 1024u * 1024u;
    options->max_output_size = 16u * 1024u * 1024u;
    options->max_symbols = 8192;
    options->max_statements = 100000;
}
void cstone_result_free(cstone_result *result) {
    size_t k;
    if (!result)
        return;
    free(result->bytes);
    for (k = 0; k < result->label_count; ++k)
        free(result->labels[k].name);
    free(result->labels);
    for (k = 0; k < result->relocation_count; ++k)
        free(result->relocations[k].symbol);
    free(result->relocations);
    memset(result, 0, sizeof(*result));
}
cstone_status cstone_assemble_n(const char *source, size_t length, const cstone_options *options,
                                cstone_result *result) {
    cstone_i_context c;
    size_t start = 0, k;
    int ok = 1;
    if (!result)
        return CSTONE_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));
    memset(&c, 0, sizeof(c));
    c.out = result;
    cstone_options_init(&c.options);
    if (options) {
        if (options->struct_size < sizeof(*options)) {
            cstone_i_fail(&c, CSTONE_INVALID_ARGUMENT, 0, "Invalid options struct_size.");
            return result->diagnostic.status;
        }
        c.options = *options;
    }
    if (!source || length == SIZE_MAX || (c.options.flags & ~CSTONE_ALLOW_UNRESOLVED)) {
        cstone_i_fail(&c, CSTONE_INVALID_ARGUMENT, 0, "Invalid source, length, or option flags.");
        return result->diagnostic.status;
    }
    if ((unsigned)c.options.arch >= CSTONE_ARCH_COUNT) {
        cstone_i_fail(&c, CSTONE_UNSUPPORTED_ARCH, 0, "Unsupported architecture.");
        return result->diagnostic.status;
    }
    if (cstone_i_arch32(c.options.arch) && c.options.base_address > UINT32_MAX) {
        cstone_i_fail(&c, CSTONE_RANGE_ERROR, 0, "Base address exceeds the 32-bit address space.");
        return result->diagnostic.status;
    }
    if (length > c.options.max_source_size) {
        cstone_i_fail(&c, CSTONE_LIMIT_ERROR, 0, "Source byte limit exceeded.");
        return result->diagnostic.status;
    }
    result->arch = c.options.arch;
    result->base_address = c.options.base_address;
    c.line = 1;
    for (k = 0; k <= length; ++k) {
        if (k < length && source[k] == 0) {
            ok = cstone_i_fail(&c, CSTONE_SYNTAX_ERROR, k - start + 1, "Embedded NUL byte in source.");
            break;
        }
        if (k == length || source[k] == '\n') {
            if (!parse_line(&c, source + start, k - start)) {
                ok = 0;
                break;
            }
            start = k + 1;
            ++c.line;
        }
    }
    if (ok)
        ok = cstone_i_finish(&c);
    if (!ok && result->diagnostic.status == CSTONE_OK)
        cstone_i_fail(&c, CSTONE_INVALID_OPERAND, 1, "Invalid operand form.");
    if (result->diagnostic.status != CSTONE_OK)
        ok = 0;
    cstone_i_cleanup(&c);
    if (!ok) {
        cstone_diagnostic d = result->diagnostic;
        cstone_result_free(result);
        result->diagnostic = d;
        result->arch = c.options.arch;
        result->base_address = c.options.base_address;
    }
    return result->diagnostic.status;
}
cstone_status cstone_assemble_ex(const char *source, const cstone_options *options, cstone_result *result) {
    cstone_options defaults;
    size_t length = 0, limit;
    cstone_options_init(&defaults);
    if (!source || (options && options->struct_size < sizeof(*options)))
        return cstone_assemble_n(NULL, 0, options, result);
    limit = options ? options->max_source_size : defaults.max_source_size;
    while (length < limit && source[length])
        ++length;
    if (length == limit && source[length]) {
        if (limit == SIZE_MAX)
            return cstone_assemble_n(NULL, 0, options, result);
        return cstone_assemble_n(source, limit + 1, options, result);
    }
    return cstone_assemble_n(source, length, options, result);
}
cstone_status cstone_assemble(const char *source, uint64_t base, cstone_result *result) {
    cstone_options options;
    cstone_options_init(&options);
    options.base_address = base;
    return cstone_assemble_ex(source, &options, result);
}
const char *cstone_status_string(cstone_status status) {
    static const char *const names[] = {"ok",
                                        "invalid argument",
                                        "syntax error",
                                        "unsupported instruction",
                                        "invalid operand",
                                        "unknown label",
                                        "duplicate label",
                                        "range error",
                                        "out of memory",
                                        "limit exceeded",
                                        "unaligned address",
                                        "unsupported architecture",
                                        "relocation error"};
    return (unsigned)status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown status";
}
const char *cstone_arch_name(cstone_arch arch) {
    static const char *const names[] = {"x86_64",  "x86",      "aarch64",  "arm",      "thumb",   "riscv32",
                                        "riscv64", "mips32le", "mips32be", "mips64le", "mips64be"};
    return (unsigned)arch < CSTONE_ARCH_COUNT ? names[arch] : "unknown";
}
cstone_status cstone_arch_parse(const char *name, cstone_arch *arch) {
    unsigned k;
    cstone_i_span s;
    if (!name || !arch)
        return CSTONE_INVALID_ARGUMENT;
    s.p = name;
    s.n = strlen(name);
    s.col = 1;
    if (cstone_i_equal(s, "x64") || cstone_i_equal(s, "amd64")) {
        *arch = CSTONE_X86_64;
        return CSTONE_OK;
    }
    if (cstone_i_equal(s, "x86_32") || cstone_i_equal(s, "i386")) {
        *arch = CSTONE_X86_32;
        return CSTONE_OK;
    }
    if (cstone_i_equal(s, "arm64")) {
        *arch = CSTONE_AARCH64;
        return CSTONE_OK;
    }
    if (cstone_i_equal(s, "arm32")) {
        *arch = CSTONE_ARM;
        return CSTONE_OK;
    }
    if (cstone_i_equal(s, "rv32")) {
        *arch = CSTONE_RISCV32;
        return CSTONE_OK;
    }
    if (cstone_i_equal(s, "rv64")) {
        *arch = CSTONE_RISCV64;
        return CSTONE_OK;
    }
    for (k = 0; k < CSTONE_ARCH_COUNT; ++k)
        if (cstone_i_equal(s, cstone_arch_name((cstone_arch)k))) {
            *arch = (cstone_arch)k;
            return CSTONE_OK;
        }
    return CSTONE_UNSUPPORTED_ARCH;
}
const char *cstone_version(void) {
    return CSTONE_VERSION;
}
unsigned cstone_abi_version(void) {
    return CSTONE_ABI_VERSION;
}
