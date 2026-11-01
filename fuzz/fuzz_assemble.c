#include "cstone.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    cstone_options options;
    cstone_result result;
    cstone_binding bindings[] = {{"external", 0x2000}, {"target", 0x3000}};
    cstone_diagnostic diagnostic;
    if (!size)
        return 0;
    cstone_options_init(&options);
    options.arch = (cstone_arch)(data[0] % CSTONE_ARCH_COUNT);
    options.flags = CSTONE_ALLOW_UNRESOLVED;
    options.base_address = 0x1000;
    options.max_source_size = 4096;
    options.max_output_size = 4096;
    options.max_symbols = 128;
    options.max_statements = 256;
    if (cstone_assemble_n((const char *)data + 1, size - 1, &options, &result) == CSTONE_OK) {
        if (result.size > options.max_output_size || (result.size && !result.bytes) ||
            result.label_count > options.max_symbols)
            abort();
        (void)cstone_link(&result, bindings, 2, &diagnostic);
    } else if (result.bytes || result.size || result.labels || result.label_count || result.relocations ||
               result.relocation_count) {
        abort();
    }
    cstone_result_free(&result);
    return 0;
}
