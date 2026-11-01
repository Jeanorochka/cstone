#include "cstone.h"

int main() {
    cstone_options options;
    cstone_options_init(&options);
    options.arch = CSTONE_AARCH64;
    cstone_result result;
    if (cstone_assemble_ex("mov x0,#42\nret", &options, &result) != CSTONE_OK)
        return 1;
    bool correct = result.size == 8;
    cstone_result_free(&result);
    return correct ? 0 : 1;
}
