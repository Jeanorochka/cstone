#include "cstone.h"
#include <string.h>

int main(void) {
    cstone_result result;
    const unsigned char expected[] = {0xb8, 42, 0, 0, 0, 0xc3};
    int failed;
    if (cstone_assemble("mov eax,42\nret", 0, &result) != CSTONE_OK)
        return 1;
    failed = result.size != sizeof(expected) || memcmp(result.bytes, expected, sizeof(expected));
    cstone_result_free(&result);
    return failed ? 1 : 0;
}
