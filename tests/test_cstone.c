#include "cstone.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && defined(_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static unsigned passed, failed;

#define CHECK(expr)                                                                                          \
    do {                                                                                                     \
        if (expr)                                                                                            \
            ++passed;                                                                                        \
        else {                                                                                               \
            ++failed;                                                                                        \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #expr);                               \
        }                                                                                                    \
    } while (0)

static void bytes(const char *source, uint64_t base, const char *hex) {
    cstone_result result;
    cstone_status status = cstone_assemble(source, base, &result);
    size_t count = strlen(hex) / 2, i;
    CHECK(status == CSTONE_OK);
    if (status != CSTONE_OK) {
        fprintf(stderr, "source: %s\nerror: %s\n", source, result.diagnostic.message);
    } else {
        CHECK(result.size == count);
        for (i = 0; i < count && i < result.size; ++i) {
            unsigned expected = 0;
            (void)sscanf(hex + i * 2, "%2x", &expected);
            CHECK(result.bytes[i] == expected);
        }
    }
    cstone_result_free(&result);
    CHECK(result.bytes == NULL && result.labels == NULL && result.size == 0);
}

static void error(const char *source, uint64_t base, cstone_status expected, size_t line, size_t column) {
    cstone_result result;
    cstone_status status = cstone_assemble(source, base, &result);
    CHECK(status == expected);
    if (status != expected)
        fprintf(stderr, "source: %s\nactual: %s (%s)\n", source ? source : "NULL",
                cstone_status_string(status), result.diagnostic.message);
    CHECK(result.diagnostic.status == status);
    CHECK(result.diagnostic.line == line);
    CHECK(result.diagnostic.column == column);
    CHECK(result.diagnostic.message[0] != '\0');
    CHECK(result.bytes == NULL && result.size == 0);
    CHECK(result.labels == NULL && result.label_count == 0);
    cstone_result_free(&result);
    cstone_result_free(&result);
}

static void encodings(void) {
    bytes("", 0, "");
    bytes("; comment\r\n # comment\n", 0, "");
    bytes("nop\nret\nleave\nint3\nud2", 0, "90c3c9cc0f0b");
    bytes("XoR EAX, EAX\r\nret ; trailing comment", 0, "31c0c3");
    bytes("mov eax, 42\nret", 0, "b82a000000c3");
    bytes("mov rax, 42", 0, "48c7c02a000000");
    bytes("mov r8, -1", 0, "49c7c0ffffffff");
    bytes("mov r15, 0x123456789abcdef0", 0, "49bff0debc9a78563412");
    bytes("mov rax, -9223372036854775808", 0, "48b80000000000000080");
    bytes("mov rax, 18446744073709551615", 0, "48b8ffffffffffffffff");
    bytes("mov r8d, 0xffffffff", 0, "41b8ffffffff");
    bytes("mov r15, r8\nxor r8d, r15d", 0, "4d89c74531f8");
    bytes("add rax, rcx\nsub r9d, edx\ncmp r8, r15\ntest r15d, r8d", 0, "4801c84129d14d39f84585c7");
    bytes("add eax, 127\nadd eax, 128\nsub r9, -128\nsub r9, -129", 0,
          "83c07f05800000004983e9804981e97fffffff");
    bytes("and eax, 0xffffffff\nor r8d, 0x80000000\nxor rax, -2147483648", 0,
          "83e0ff4181c800000080483500000080");
    bytes("push rbp\npush r15\npop r15\npop rbp", 0, "554157415f5d");
    bytes("push -128\npush 128\npush -2147483648", 0, "6a8068800000006800000080");
    bytes("inc eax\ndec r15\ncall r8\njmp r15", 0, "ffc049ffcf41ffd041ffe7");
    bytes("jmp target\nnop\ntarget: ret", 0, "e90100000090c3");
    bytes("call target\nnop\ntarget: ret", 0x1000, "e80100000090c3");
    bytes("again: dec ecx\njnz again", 0, "ffc90f85f8ffffff");
    bytes("xor eax,eax\njz done\ninc eax\ndone: ret", 0, "31c00f8402000000ffc0c3");
    bytes("jmp 2147483652", 0, "e9ffffff7f");
    bytes("jmp 5", UINT64_C(0x80000000), "e900000080");
    bytes("jmp end\nend: ret", UINT64_C(0xffffffff80000000), "e900000000c3");
    bytes("db 0, 127, 128, 255, -1, -128", 0, "007f80ffff80");
    bytes("mov eax,+010\ndb 0Xff", 0, "b80a000000ff");
    bytes("a: b: nop\nc:", 0, "90");
}

static void labels(void) {
    cstone_result result;
    CHECK(cstone_assemble("entry: jmp done\ndata: db 1,2,3\ndone: ret", 0x1000, &result) == CSTONE_OK);
    CHECK(result.label_count == 3);
    if (result.label_count == 3) {
        CHECK(!strcmp(result.labels[0].name, "entry"));
        CHECK(result.labels[0].offset == 0 && result.labels[0].address == 0x1000);
        CHECK(!strcmp(result.labels[1].name, "data"));
        CHECK(result.labels[1].offset == 5 && result.labels[1].address == 0x1005);
        CHECK(!strcmp(result.labels[2].name, "done"));
        CHECK(result.labels[2].offset == 8 && result.labels[2].address == 0x1008);
        CHECK(result.bytes[1] == 3);
    }
    cstone_result_free(&result);
    CHECK(cstone_assemble("end:", UINT64_MAX, &result) == CSTONE_OK);
    CHECK(result.label_count == 1 && result.labels[0].address == UINT64_MAX);
    cstone_result_free(&result);
}

static void diagnostics(void) {
    error(NULL, 0, CSTONE_INVALID_ARGUMENT, 0, 0);
    CHECK(cstone_assemble("ret", 0, NULL) == CSTONE_INVALID_ARGUMENT);
    error("nop\n\n   add eax, rax", 0, CSTONE_INVALID_OPERAND, 3, 13);
    error("  jmp missing", 0, CSTONE_UNKNOWN_LABEL, 1, 7);
    error("a: nop\na: ret", 0, CSTONE_DUPLICATE_LABEL, 2, 1);
    error("end: nop\njmp END", 0, CSTONE_UNKNOWN_LABEL, 2, 5);
    error("eax: ret", 0, CSTONE_INVALID_OPERAND, 1, 1);
    error("vaddps", 0, CSTONE_UNSUPPORTED_INSTRUCTION, 1, 1);
    bytes("mov eax, [rax]", 0, "8b00");
    error("mov eax, 4294967296", 0, CSTONE_RANGE_ERROR, 1, 10);
    error("mov eax, -2147483649", 0, CSTONE_RANGE_ERROR, 1, 10);
    error("mov rax, 18446744073709551616", 0, CSTONE_RANGE_ERROR, 1, 10);
    error("mov rax, -9223372036854775809", 0, CSTONE_RANGE_ERROR, 1, 10);
    error("mov eax, 0x", 0, CSTONE_SYNTAX_ERROR, 1, 10);
    error("add rax, 2147483648", 0, CSTONE_RANGE_ERROR, 1, 10);
    error("push 2147483648", 0, CSTONE_INVALID_OPERAND, 1, 6);
    error("pop eax", 0, CSTONE_INVALID_OPERAND, 1, 5);
    error("je rax", 0, CSTONE_INVALID_OPERAND, 1, 4);
    error("jmp -1", 0, CSTONE_RANGE_ERROR, 1, 5);
    error("jmp 2147483653", 0, CSTONE_RANGE_ERROR, 1, 5);
    error("jmp 4", UINT64_C(0x80000000), CSTONE_RANGE_ERROR, 1, 5);
    error("ret", UINT64_MAX, CSTONE_RANGE_ERROR, 1, 1);
    error("mov eax,", 0, CSTONE_SYNTAX_ERROR, 1, 9);
    error("mov eax, 1, 2", 0, CSTONE_INVALID_OPERAND, 1, 13);
    bytes("ret 1", 0, "c20100");
    error("db 256", 0, CSTONE_RANGE_ERROR, 1, 4);
    error("db -129", 0, CSTONE_RANGE_ERROR, 1, 4);
    error("db", 0, CSTONE_INVALID_OPERAND, 1, 3);
    error("db 1,", 0, CSTONE_SYNTAX_ERROR, 1, 6);
    error("nop!", 0, CSTONE_SYNTAX_ERROR, 1, 4);
}

static void growth_and_malformed_input(void) {
    char *source = (char *)malloc(4u * 20000u + 1u);
    cstone_result result;
    uint32_t state = 0x12345678;
    static const char alphabet[] = "abcde0123456789,:;[]+- \r\n\t";
    unsigned trial;
    size_t i;
    CHECK(source != NULL);
    if (source) {
        for (i = 0; i < 20000; ++i)
            memcpy(source + i * 4, "nop\n", 4);
        source[80000] = '\0';
        CHECK(cstone_assemble(source, 0, &result) == CSTONE_OK);
        CHECK(result.size == 20000);
        if (result.size == 20000)
            CHECK(result.bytes[0] == 0x90 && result.bytes[19999] == 0x90);
        cstone_result_free(&result);
        free(source);
    }
    for (trial = 0; trial < 20000; ++trial) {
        char input[129];
        size_t length;
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        length = state % 128u;
        for (i = 0; i < length; ++i) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            input[i] = alphabet[state % (sizeof(alphabet) - 1)];
        }
        input[length] = '\0';
        if (cstone_assemble(input, trial, &result) != CSTONE_OK) {
            CHECK(result.bytes == NULL && result.labels == NULL && result.size == 0);
        }
        cstone_result_free(&result);
    }
}

#if defined(_WIN32) && defined(_WIN64)
static void native_execution(void) {
    static const struct {
        const char *source;
        int argument;
        int expected;
    } cases[] = {{"mov eax,42\nret", 0, 42},
                 {"xor eax,eax\nagain: add eax,ecx\ndec ecx\njnz again\nret", 10, 55},
                 {"cmp ecx,7\njne other\nmov eax,42\nret\nother: mov eax,-1\nret", 7, 42},
                 {"cmp ecx,7\njne other\nmov eax,42\nret\nother: mov eax,-1\nret", 8, -1}};
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        cstone_result result;
        void *memory;
        DWORD previous;
        int (*function)(int);
        cstone_status status = cstone_assemble(cases[i].source, 0, &result);
        CHECK(status == CSTONE_OK);
        if (status != CSTONE_OK) {
            cstone_result_free(&result);
            continue;
        }
        memory = VirtualAlloc(NULL, result.size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        CHECK(memory != NULL);
        if (!memory) {
            cstone_result_free(&result);
            continue;
        }
        memcpy(memory, result.bytes, result.size);
        if (VirtualProtect(memory, result.size, PAGE_EXECUTE_READ, &previous) &&
            FlushInstructionCache(GetCurrentProcess(), memory, result.size)) {
            _Static_assert(sizeof(function) == sizeof(memory), "Windows x64 pointer size");
            memcpy(&function, &memory, sizeof(function));
            CHECK(function(cases[i].argument) == cases[i].expected);
        } else
            CHECK(0 && "Cannot prepare native execution test");
        CHECK(VirtualFree(memory, 0, MEM_RELEASE) != 0);
        cstone_result_free(&result);
    }
}
#endif

int main(void) {
    CHECK(!strcmp(cstone_version(), CSTONE_VERSION));
    CHECK(!strcmp(cstone_status_string(CSTONE_RANGE_ERROR), "range error"));
    encodings();
    labels();
    diagnostics();
    growth_and_malformed_input();
#if defined(_WIN32) && defined(_WIN64)
    native_execution();
#endif
    printf("%u checks passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
