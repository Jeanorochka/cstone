#ifndef CSTONE_H
#define CSTONE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CSTONE_VERSION "0.2.0"
#define CSTONE_ABI_VERSION 1u
#define CSTONE_ALLOW_UNRESOLVED 1u

#if defined(_WIN32) && defined(CSTONE_SHARED)
#ifdef CSTONE_BUILD
#define CSTONE_API __declspec(dllexport)
#else
#define CSTONE_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(CSTONE_SHARED)
#define CSTONE_API __attribute__((visibility("default")))
#else
#define CSTONE_API
#endif

typedef enum cstone_arch {
    CSTONE_X86_64 = 0,
    CSTONE_X86_32,
    CSTONE_AARCH64,
    CSTONE_ARM,
    CSTONE_THUMB,
    CSTONE_RISCV32,
    CSTONE_RISCV64,
    CSTONE_MIPS32_LE,
    CSTONE_MIPS32_BE,
    CSTONE_MIPS64_LE,
    CSTONE_MIPS64_BE,
    CSTONE_ARCH_COUNT
} cstone_arch;

typedef enum cstone_status {
    CSTONE_OK = 0,
    CSTONE_INVALID_ARGUMENT,
    CSTONE_SYNTAX_ERROR,
    CSTONE_UNSUPPORTED_INSTRUCTION,
    CSTONE_INVALID_OPERAND,
    CSTONE_UNKNOWN_LABEL,
    CSTONE_DUPLICATE_LABEL,
    CSTONE_RANGE_ERROR,
    CSTONE_OUT_OF_MEMORY,
    CSTONE_LIMIT_ERROR,
    CSTONE_UNALIGNED_ADDRESS,
    CSTONE_UNSUPPORTED_ARCH,
    CSTONE_RELOCATION_ERROR
} cstone_status;

typedef enum cstone_reloc_kind {
    CSTONE_ABS8,
    CSTONE_ABS16,
    CSTONE_ABS32,
    CSTONE_ABS64,
    CSTONE_X86_REL32,
    CSTONE_A64_BRANCH26,
    CSTONE_A64_BRANCH19,
    CSTONE_A64_BRANCH14,
    CSTONE_A64_ADR21,
    CSTONE_A64_ADRP21,
    CSTONE_A64_LO12,
    CSTONE_ARM_BRANCH24,
    CSTONE_THUMB_BRANCH11,
    CSTONE_THUMB_BRANCH8,
    CSTONE_THUMB_BRANCH24,
    CSTONE_THUMB_LITERAL8,
    CSTONE_RV_JAL20,
    CSTONE_RV_BRANCH12,
    CSTONE_RV_PCREL_HI20,
    CSTONE_RV_PCREL_LO12,
    CSTONE_MIPS_BRANCH16,
    CSTONE_MIPS_JUMP26,
    CSTONE_X86_ABS32S
} cstone_reloc_kind;

typedef struct cstone_diagnostic {
    cstone_status status;
    size_t line;
    size_t column;
    char message[192];
} cstone_diagnostic;

typedef struct cstone_label {
    char *name;
    size_t offset;
    uint64_t address;
} cstone_label;

typedef struct cstone_relocation {
    char *symbol;
    size_t offset;
    cstone_reloc_kind kind;
    int64_t addend;
    uint64_t place;
    uint64_t anchor;
    int32_t pc_bias;
    size_t line;
    size_t column;
    int resolved;
} cstone_relocation;

typedef struct cstone_options {
    size_t struct_size;
    cstone_arch arch;
    uint32_t flags;
    uint64_t base_address;
    size_t max_source_size;
    size_t max_output_size;
    size_t max_symbols;
    size_t max_statements;
} cstone_options;

typedef struct cstone_result {
    uint8_t *bytes;
    size_t size;
    cstone_label *labels;
    size_t label_count;
    cstone_diagnostic diagnostic;
    cstone_relocation *relocations;
    size_t relocation_count;
    cstone_arch arch;
    uint64_t base_address;
} cstone_result;

typedef struct cstone_binding {
    const char *name;
    uint64_t address;
} cstone_binding;

/* Free a previous result before reuse. */
CSTONE_API cstone_status cstone_assemble(const char *source, uint64_t base_address, cstone_result *result);
CSTONE_API void cstone_options_init(cstone_options *options);
CSTONE_API cstone_status cstone_assemble_ex(const char *source, const cstone_options *options,
                                            cstone_result *result);
CSTONE_API cstone_status cstone_assemble_n(const char *source, size_t length, const cstone_options *options,
                                           cstone_result *result);
CSTONE_API cstone_status cstone_link(cstone_result *result, const cstone_binding *bindings, size_t count,
                                     cstone_diagnostic *diagnostic);
CSTONE_API void cstone_result_free(cstone_result *result);
CSTONE_API const char *cstone_status_string(cstone_status status);
CSTONE_API const char *cstone_arch_name(cstone_arch arch);
CSTONE_API cstone_status cstone_arch_parse(const char *name, cstone_arch *arch);
CSTONE_API const char *cstone_version(void);
CSTONE_API unsigned cstone_abi_version(void);

#ifdef __cplusplus
}
#endif

#endif
