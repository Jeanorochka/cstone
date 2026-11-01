#ifndef CSTONE_INTERNAL_H
#define CSTONE_INTERNAL_H
#include "cstone.h"
#include <stdlib.h>

#ifdef CSTONE_TEST_ALLOCATOR
void *cstone_test_malloc(size_t size);
void *cstone_test_calloc(size_t count, size_t size);
void *cstone_test_realloc(void *pointer, size_t size);
void cstone_test_free(void *pointer);
#define malloc cstone_test_malloc
#define calloc cstone_test_calloc
#define realloc cstone_test_realloc
#define free cstone_test_free
#endif

typedef struct cstone_i_span {
    const char *p;
    size_t n;
    size_t col;
} cstone_i_span;
typedef struct cstone_i_num {
    uint64_t magnitude;
    int negative;
} cstone_i_num;
typedef struct cstone_i_insn {
    cstone_i_span name;
    cstone_i_span op[5];
    size_t count;
} cstone_i_insn;
typedef struct cstone_i_symbol {
    char *name;
    size_t length;
    size_t offset;
    size_t order;
    int defined;
} cstone_i_symbol;
typedef struct cstone_i_fixup {
    cstone_relocation reloc;
    size_t symbol;
    uint64_t target;
} cstone_i_fixup;
typedef struct cstone_i_context {
    cstone_options options;
    cstone_result *out;
    size_t line, statements, byte_capacity;
    cstone_i_symbol *symbols;
    size_t symbol_count, symbol_capacity, defined_count;
    size_t *hash;
    size_t hash_capacity;
    cstone_i_fixup *fixups;
    size_t fixup_count, fixup_capacity;
} cstone_i_context;

int cstone_i_space(char c);
int cstone_i_equal(cstone_i_span s, const char *text);
int cstone_i_identifier(cstone_i_span s);
cstone_i_span cstone_i_trim(cstone_i_span s);
cstone_i_span cstone_i_slice(cstone_i_span s, size_t start, size_t length);
int cstone_i_fail(cstone_i_context *c, cstone_status status, size_t column, const char *format, ...);
int cstone_i_number(cstone_i_context *c, cstone_i_span s, cstone_i_num *number);
int cstone_i_unsigned(cstone_i_context *c, cstone_i_span s, unsigned bits, uint64_t *value);
int cstone_i_signed(cstone_i_context *c, cstone_i_span s, unsigned bits, int64_t *value);
uint64_t cstone_i_bits(cstone_i_num n);
int cstone_i_emit(cstone_i_context *c, const uint8_t *data, size_t size, size_t column);
int cstone_i_emit_word(cstone_i_context *c, uint32_t word, size_t column);
int cstone_i_emit_half(cstone_i_context *c, uint16_t word, size_t column);
void cstone_i_put(uint8_t *data, uint64_t value, unsigned size, int big_endian);
uint32_t cstone_i_get32(const uint8_t *data, int big_endian);
int cstone_i_big_endian(cstone_arch arch);
int cstone_i_arch32(cstone_arch arch);
int cstone_i_reference(cstone_i_context *c, cstone_i_span expression, cstone_reloc_kind kind, size_t offset,
                       int32_t pc_bias, uint64_t anchor);
int cstone_i_expression(cstone_i_context *c, cstone_i_span expression, cstone_i_span *symbol,
                        uint64_t *target, int64_t *addend);
int cstone_i_count(cstone_i_context *c, const cstone_i_insn *i, size_t count);
int cstone_i_x86(cstone_i_context *c, const cstone_i_insn *i);
int cstone_i_x86_is_register(cstone_i_span name);
int cstone_i_aarch64(cstone_i_context *c, const cstone_i_insn *i);
int cstone_i_arm(cstone_i_context *c, const cstone_i_insn *i);
int cstone_i_thumb(cstone_i_context *c, const cstone_i_insn *i);
int cstone_i_riscv(cstone_i_context *c, const cstone_i_insn *i);
int cstone_i_mips(cstone_i_context *c, const cstone_i_insn *i);
void *cstone_i_reserve(void *old, size_t *capacity, size_t width, size_t need);
char *cstone_i_copy(cstone_i_span s);
int cstone_i_define_label(cstone_i_context *c, cstone_i_span name);
int cstone_i_finish(cstone_i_context *c);
void cstone_i_cleanup(cstone_i_context *c);
int cstone_i_patch(uint8_t *bytes, size_t size, cstone_arch arch, const cstone_relocation *r,
                   uint64_t target);
#endif
