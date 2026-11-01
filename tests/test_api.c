#include "cstone.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned passed, failed;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (x)                                                                                               \
            ++passed;                                                                                        \
        else {                                                                                               \
            ++failed;                                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                          \
        }                                                                                                    \
    } while (0)

static void limits_and_inputs(void) {
    cstone_options o;
    cstone_result r;
    cstone_status status;
    char embedded[] = {'n', 'o', 'p', 0, 'r', 'e', 't'};
    cstone_options_init(&o);
    o.arch = CSTONE_X86_32;
    o.base_address = UINT64_C(0x100000000);
    CHECK(cstone_assemble_ex("nop", &o, &r) == CSTONE_RANGE_ERROR);
    cstone_result_free(&r);
    o.base_address = UINT32_MAX - 1u;
    CHECK(cstone_assemble_ex("db 1,2", &o, &r) == CSTONE_RANGE_ERROR);
    cstone_result_free(&r);
    cstone_options_init(&o);
    CHECK(o.struct_size == sizeof(o));
    CHECK(cstone_abi_version() == CSTONE_ABI_VERSION);
    o.max_source_size = 2;
    CHECK(cstone_assemble_n("nop", 3, &o, &r) == CSTONE_LIMIT_ERROR);
    CHECK(!r.bytes && !r.labels);
    cstone_result_free(&r);
    cstone_options_init(&o);
    o.max_output_size = 1;
    CHECK(cstone_assemble_ex("nop\nret", &o, &r) == CSTONE_LIMIT_ERROR);
    CHECK(!r.bytes && !r.size);
    cstone_result_free(&r);
    cstone_options_init(&o);
    o.max_symbols = 1;
    CHECK(cstone_assemble_ex("a: nop\nb: ret", &o, &r) == CSTONE_LIMIT_ERROR);
    cstone_result_free(&r);
    cstone_options_init(&o);
    o.max_statements = 1;
    CHECK(cstone_assemble_ex("nop\nret", &o, &r) == CSTONE_LIMIT_ERROR);
    cstone_result_free(&r);
    cstone_options_init(&o);
    CHECK(cstone_assemble_n(embedded, sizeof(embedded), &o, &r) == CSTONE_SYNTAX_ERROR);
    CHECK(r.diagnostic.column == 4);
    cstone_result_free(&r);
    o.arch = (cstone_arch)-1;
    CHECK(cstone_assemble_ex("nop", &o, &r) == CSTONE_UNSUPPORTED_ARCH);
    cstone_result_free(&r);
    cstone_options_init(&o);
    o.struct_size = 0;
    CHECK(cstone_assemble_ex("nop", &o, &r) == CSTONE_INVALID_ARGUMENT);
    cstone_result_free(&r);
    cstone_options_init(&o);
    o.flags = 0x80000000u;
    CHECK(cstone_assemble_ex("nop", &o, &r) == CSTONE_INVALID_ARGUMENT);
    cstone_result_free(&r);
    cstone_options_init(&o);
    o.arch = CSTONE_AARCH64;
    o.base_address = 2;
    CHECK(cstone_assemble_ex("nop", &o, &r) == CSTONE_UNALIGNED_ADDRESS);
    cstone_result_free(&r);
    cstone_options_init(&o);
    o.arch = CSTONE_THUMB;
    CHECK(cstone_assemble_ex("db 0\nnop", &o, &r) == CSTONE_UNALIGNED_ADDRESS);
    cstone_result_free(&r);
    cstone_options_init(&o);
    o.max_output_size = 64;
    CHECK(cstone_assemble_ex(".space 18446744073709551615", &o, &r) == CSTONE_LIMIT_ERROR);
    cstone_result_free(&r);
    CHECK(cstone_assemble_ex(".align 3", NULL, &r) == CSTONE_RANGE_ERROR);
    cstone_result_free(&r);
    CHECK(cstone_assemble_ex("dq target+8\ntarget: db 1", NULL, &r) == CSTONE_OK);
    if (r.size == 9)
        CHECK(r.bytes[0] == 16);
    cstone_result_free(&r);
    CHECK(cstone_assemble_ex("a: nop\na: ret", NULL, &r) == CSTONE_DUPLICATE_LABEL);
    cstone_result_free(&r);
    status = cstone_assemble_n("retJUNK", 3, NULL, &r);
    CHECK(status == CSTONE_OK && r.size == 1 && r.bytes[0] == 0xc3);
    cstone_result_free(&r);
}
static void linking(void) {
    cstone_options o;
    cstone_result r;
    cstone_diagnostic d;
    cstone_binding bindings[] = {{"function", 0x2000}, {"data", 0x3000}};
    uint8_t old[32];
    cstone_options_init(&o);
    o.base_address = 0x1000;
    o.flags = CSTONE_ALLOW_UNRESOLVED;
    CHECK(cstone_assemble_ex("call function\ndq data+8", &o, &r) == CSTONE_OK);
    CHECK(r.relocation_count == 2 && r.size == 13);
    if (r.size == 13) {
        memcpy(old, r.bytes, r.size);
        CHECK(cstone_link(&r, bindings, 1, &d) == CSTONE_UNKNOWN_LABEL);
        CHECK(!memcmp(old, r.bytes, r.size));
        CHECK(!r.relocations[0].resolved);
        CHECK(cstone_link(&r, bindings, 2, &d) == CSTONE_OK);
        CHECK(r.bytes[0] == 0xe8 && r.bytes[1] == 0xfb && r.bytes[2] == 0x0f);
        CHECK(r.bytes[5] == 8 && r.bytes[6] == 0x30);
        CHECK(r.relocations[0].resolved && r.relocations[1].resolved);
    }
    cstone_result_free(&r);
    CHECK(cstone_assemble_ex("call function", &o, &r) == CSTONE_OK);
    memcpy(old, r.bytes, r.size);
    bindings[0].address = UINT64_MAX;
    CHECK(cstone_link(&r, bindings, 1, &d) == CSTONE_RELOCATION_ERROR);
    CHECK(!memcmp(old, r.bytes, r.size));
    cstone_result_free(&r);
    bindings[0].address = 0x2000;
    bindings[1].name = "function";
    CHECK(cstone_assemble_ex("call function", &o, &r) == CSTONE_OK);
    CHECK(cstone_link(&r, bindings, 2, &d) == CSTONE_INVALID_ARGUMENT);
    cstone_result_free(&r);
}
static void architecture_links(void) {
    static const struct {
        cstone_arch arch;
        const char *inside, *outside;
        unsigned target;
    } cases[] = {{CSTONE_X86_64, "call target\ntarget: nop", "call external\nnop", 5},
                 {CSTONE_X86_32, "call target\ntarget: nop", "call external\nnop", 5},
                 {CSTONE_AARCH64, "bl target\ntarget: nop", "bl external\nnop", 4},
                 {CSTONE_ARM, "bl target\ntarget: nop", "bl external\nnop", 4},
                 {CSTONE_THUMB, "bl target\ntarget: nop", "bl external\nnop", 4},
                 {CSTONE_RISCV32, "jal ra,target\ntarget: nop", "jal ra,external\nnop", 4},
                 {CSTONE_RISCV64, "jal ra,target\ntarget: nop", "jal ra,external\nnop", 4},
                 {CSTONE_MIPS32_LE, "beq $2,$3,target\ntarget: nop", "beq $2,$3,external\nnop", 4},
                 {CSTONE_MIPS32_BE, "beq $2,$3,target\ntarget: nop", "beq $2,$3,external\nnop", 4},
                 {CSTONE_MIPS64_LE, "j target\ntarget: nop", "j external\nnop", 4},
                 {CSTONE_MIPS64_BE, "j target\ntarget: nop", "j external\nnop", 4}};
    size_t k;
    for (k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
        cstone_options o;
        cstone_result a, b;
        cstone_diagnostic d;
        cstone_binding binding = {"external", 0x1000 + cases[k].target};
        cstone_arch parsed;
        cstone_options_init(&o);
        o.arch = cases[k].arch;
        o.base_address = 0x1000;
        CHECK(cstone_arch_parse(cstone_arch_name(o.arch), &parsed) == CSTONE_OK && parsed == o.arch);
        CHECK(cstone_assemble_ex(cases[k].inside, &o, &a) == CSTONE_OK);
        o.flags = CSTONE_ALLOW_UNRESOLVED;
        CHECK(cstone_assemble_ex(cases[k].outside, &o, &b) == CSTONE_OK);
        CHECK(b.relocation_count == 1);
        CHECK(cstone_link(&b, &binding, 1, &d) == CSTONE_OK);
        CHECK(a.size == b.size && a.size > 0);
        if (a.size == b.size && a.size)
            CHECK(!memcmp(a.bytes, b.bytes, a.size));
        cstone_result_free(&a);
        cstone_result_free(&b);
    }
}
static void hash_growth(void) {
    char *source = (char *)malloc(200000);
    size_t used = 0, k;
    cstone_result r;
    cstone_options o;
    CHECK(source != NULL);
    if (!source)
        return;
    for (k = 0; k < 4096; ++k) {
        int n = snprintf(source + used, 200000 - used, "label_%zu: nop\n", k);
        if (n < 0 || (size_t)n >= 200000 - used) {
            free(source);
            CHECK(0);
            return;
        }
        used += (size_t)n;
    }
    cstone_options_init(&o);
    CHECK(cstone_assemble_n(source, used, &o, &r) == CSTONE_OK);
    CHECK(r.label_count == 4096 && r.size == 4096);
    if (r.label_count == 4096)
        CHECK(r.labels[4095].offset == 4095);
    cstone_result_free(&r);
    free(source);
}
static void architecture_rejections(void) {
    static const struct {
        cstone_arch arch;
        const char *source;
    } cases[] = {{CSTONE_X86_32, "mov rax,1"},           {CSTONE_X86_64, "mov ah,r8b"},
                 {CSTONE_X86_64, "mov eax,[rax+rsp*2]"}, {CSTONE_AARCH64, "add x0,sp,x1"},
                 {CSTONE_AARCH64, "ldr x0,[x0],#8"},     {CSTONE_AARCH64, "and x0,x1,#0"},
                 {CSTONE_AARCH64, "and xzr,x0,#1"},      {CSTONE_AARCH64, "and w0,w1,#-4294967296"},
                 {CSTONE_ARM, "mov r0,#0x12345678"},     {CSTONE_THUMB, "push {r8}"},
                 {CSTONE_THUMB, "movs r0,#256"},         {CSTONE_RISCV32, "addw a0,a1,a2"},
                 {CSTONE_RISCV64, "slli a0,a1,64"},      {CSTONE_MIPS32_LE, "daddu $2,$3,$4"},
                 {CSTONE_MIPS64_BE, "lw $2,32768($3)"}};
    size_t k;
    for (k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
        cstone_options o;
        cstone_result r;
        cstone_options_init(&o);
        o.arch = cases[k].arch;
        CHECK(cstone_assemble_ex(cases[k].source, &o, &r) != CSTONE_OK);
        CHECK(!r.bytes && !r.labels && !r.relocations);
        cstone_result_free(&r);
    }
}
int main(void) {
    limits_and_inputs();
    linking();
    architecture_links();
    hash_growth();
    architecture_rejections();
    printf("API: %u checks passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
