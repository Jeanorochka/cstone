#include "cstone.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t budget, live;
static void *pointers[16384];
static int unlimited;
static size_t find(void *p) {
    size_t k;
    for (k = 0; k < sizeof(pointers) / sizeof(pointers[0]); ++k)
        if (pointers[k] == p)
            return k;
    fprintf(stderr, "Untracked allocation\n");
    abort();
}
static void track(void *p) {
    size_t k;
    if (!p)
        return;
    for (k = 0; k < sizeof(pointers) / sizeof(pointers[0]); ++k)
        if (!pointers[k]) {
            pointers[k] = p;
            ++live;
            return;
        }
    abort();
}
static int permit(void) {
    if (unlimited)
        return 1;
    if (!budget)
        return 0;
    --budget;
    return 1;
}
void *cstone_test_malloc(size_t size) {
    void *p;
    if (!permit())
        return NULL;
    p = malloc(size);
    track(p);
    return p;
}
void *cstone_test_calloc(size_t count, size_t size) {
    void *p;
    if (!permit())
        return NULL;
    p = calloc(count, size);
    track(p);
    return p;
}
void *cstone_test_realloc(void *old, size_t size) {
    void *p;
    int had_old = old != NULL;
    size_t index = old ? find(old) : 0;
    if (!permit())
        return NULL;
    p = realloc(old, size);
    if (p) {
        if (had_old)
            pointers[index] = p;
        else
            track(p);
    }
    return p;
}
void cstone_test_free(void *p) {
    if (p) {
        size_t index = find(p);
        pointers[index] = NULL;
        --live;
    }
    free(p);
}
int main(void) {
    static const struct {
        cstone_arch arch;
        const char *source;
    } cases[] = {{CSTONE_X86_64, "entry: call external\nmov rax,[rip+data]\ndata: dq external+8"},
                 {CSTONE_AARCH64, "entry: bl external\nadr x0,entry\nnop"},
                 {CSTONE_ARM, "entry: bl external\nb entry"},
                 {CSTONE_THUMB, "entry: bl external\nb entry"},
                 {CSTONE_RISCV64, "entry: la a0,external\ncall external\nj entry"},
                 {CSTONE_MIPS64_BE, "entry: beq $2,$3,external\nnop\nb entry\nnop"}};
    size_t k, limit, total = 0;
    for (k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
        int success = 0;
        for (limit = 0; limit < 256; ++limit) {
            cstone_options o;
            cstone_result r;
            cstone_status status;
            cstone_options_init(&o);
            o.arch = cases[k].arch;
            o.base_address = 0x1000;
            o.flags = CSTONE_ALLOW_UNRESOLVED;
            budget = limit;
            unlimited = 0;
            status = cstone_assemble_ex(cases[k].source, &o, &r);
            if (status != CSTONE_OK && status != CSTONE_OUT_OF_MEMORY) {
                fprintf(stderr, "Unexpected allocation-test status %s: %s\n", cstone_status_string(status),
                        r.diagnostic.message);
                return 1;
            }
            if (status != CSTONE_OK && (r.bytes || r.labels || r.relocations))
                return 1;
            unlimited = 1;
            cstone_result_free(&r);
            if (live) {
                fprintf(stderr, "Leak with allocation budget %zu: %zu blocks\n", limit, live);
                return 1;
            }
            ++total;
            if (status == CSTONE_OK) {
                success = 1;
                break;
            }
        }
        if (!success)
            return 1;
    }
    {
        cstone_options o;
        cstone_result r;
        cstone_diagnostic d;
        cstone_binding b = {"external", 0x2000};
        uint8_t old[5];
        cstone_options_init(&o);
        o.flags = CSTONE_ALLOW_UNRESOLVED;
        unlimited = 1;
        if (cstone_assemble_ex("call external", &o, &r) != CSTONE_OK)
            return 1;
        memcpy(old, r.bytes, 5);
        budget = 0;
        unlimited = 0;
        if (cstone_link(&r, &b, 1, &d) != CSTONE_OUT_OF_MEMORY || memcmp(old, r.bytes, 5))
            return 1;
        unlimited = 1;
        cstone_result_free(&r);
        if (live)
            return 1;
    }
    printf("Allocation failures: %zu paths checked, no leaked blocks\n", total);
    return 0;
}
