#include "cstone.h"

#include <stdio.h>

int main(void) {
    cstone_result result;
    size_t i;
    const char *source = "entry:\n    mov eax, 42\n    ret\n";
    if (cstone_assemble(source, 0x1000, &result) != CSTONE_OK) {
        fprintf(stderr, "%zu:%zu: %s\n", result.diagnostic.line, result.diagnostic.column,
                result.diagnostic.message);
        cstone_result_free(&result);
        return 1;
    }
    for (i = 0; i < result.size; ++i)
        printf("%02X%s", (unsigned)result.bytes[i], i + 1 == result.size ? "\n" : " ");
    printf("entry: offset=%zu address=0x%llX\n", result.labels[0].offset,
           (unsigned long long)result.labels[0].address);
    cstone_result_free(&result);
    return 0;
}
