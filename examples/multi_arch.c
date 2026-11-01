#include "cstone.h"
#include <stdio.h>

int main(void) {
    static const struct {
        cstone_arch arch;
        const char *source;
    } samples[] = {{CSTONE_X86_64, "mov eax,42\nret"},          {CSTONE_X86_32, "mov eax,42\nret"},
                   {CSTONE_AARCH64, "mov x0,#42\nret"},         {CSTONE_ARM, "mov r0,#42\nbx lr"},
                   {CSTONE_THUMB, "movs r0,#42\nbx lr"},        {CSTONE_RISCV32, "li a0,42\nret"},
                   {CSTONE_RISCV64, "li a0,42\nret"},           {CSTONE_MIPS32_LE, "li $2,42\njr $31\nnop"},
                   {CSTONE_MIPS32_BE, "li $2,42\njr $31\nnop"}, {CSTONE_MIPS64_LE, "dli $2,42\njr $31\nnop"},
                   {CSTONE_MIPS64_BE, "dli $2,42\njr $31\nnop"}};
    size_t k, j;
    for (k = 0; k < sizeof(samples) / sizeof(samples[0]); ++k) {
        cstone_options options;
        cstone_result result;
        cstone_options_init(&options);
        options.arch = samples[k].arch;
        if (cstone_assemble_ex(samples[k].source, &options, &result) != CSTONE_OK) {
            fprintf(stderr, "%s\n", result.diagnostic.message);
            cstone_result_free(&result);
            return 1;
        }
        printf("%s:", cstone_arch_name(options.arch));
        for (j = 0; j < result.size; ++j)
            printf(" %02X", (unsigned)result.bytes[j]);
        putchar('\n');
        cstone_result_free(&result);
    }
    return 0;
}
