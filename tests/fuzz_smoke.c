#include "cstone.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
static uint32_t state = 0xc5700a11u;
static uint32_t next(void) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
int main(void) {
    static const char *const seeds[] = {
        "entry: mov rax,[rsp+8]\ncall external\ndq entry+8\nret",
        "entry: mov eax,[esp+8]\nadd eax,127\njne entry\nret",
        "entry: mov x0,#42\nstr x1,[sp,#8]\nadr x2,entry\nbl external\nret",
        "entry: mov r0,#42\nldr r1,[sp,#4]\nbl external\nbne entry\nbx lr",
        "entry: movs r0,#42\nldr r1,[sp,#4]\nbl external\nbne entry\nbx lr",
        "entry: li a0,0x12345678\nla a1,external\nlw a2,4(sp)\nbnez a0,entry\nret",
        "entry: li a0,0x123456789abcdef0\nla a1,external\nld a2,8(sp)\nbnez a0,entry\nret",
        "entry: li $2,0x12345678\nlw $3,4($29)\nbeq $2,$3,external\nnop\nb entry\nnop",
        "entry: li $2,0x12345678\nlw $3,4($29)\nbeq $2,$3,external\nnop\nb entry\nnop",
        "entry: dli $2,0x123456789abcdef0\nld $3,8($29)\nbeq $2,$3,external\nnop\nb entry\nnop",
        "entry: dli $2,0x123456789abcdef0\nld $3,8($29)\nbeq $2,$3,external\nnop\nb entry\nnop"};
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ,\t\r\n:#$+-*[](){}!._/";
    unsigned trial;
    for (trial = 0; trial < 100000; ++trial) {
        uint8_t data[1024];
        unsigned arch = next() % CSTONE_ARCH_COUNT;
        size_t size = strlen(seeds[arch]), k;
        data[0] = (uint8_t)arch;
        memcpy(data + 1, seeds[arch], size);
        if (trial % 4 == 0) {
            size = next() % 512u;
            for (k = 0; k < size; ++k)
                data[k + 1] = (uint8_t)next();
        } else {
            unsigned mutations = next() % 8u;
            for (k = 0; k < mutations; ++k) {
                size_t position = next() % (size + 1);
                unsigned action = next() % 3u;
                if (action == 0 && size) {
                    data[1 + position % size] = (uint8_t)alphabet[next() % (sizeof(alphabet) - 1)];
                } else if (action == 1 && size < sizeof(data) - 2) {
                    memmove(data + 2 + position, data + 1 + position, size - position);
                    data[position + 1] = (uint8_t)alphabet[next() % (sizeof(alphabet) - 1)];
                    ++size;
                } else if (size) {
                    position %= size;
                    memmove(data + 1 + position, data + 2 + position, size - position - 1);
                    --size;
                }
            }
        }
        LLVMFuzzerTestOneInput(data, size + 1);
    }
    puts("Fuzz smoke: 100000 deterministic mutations across all architectures passed");
    return 0;
}
