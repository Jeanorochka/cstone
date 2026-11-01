#include "cstone.h"
#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef struct worker {
    unsigned index;
    int failed;
} worker;
static void run_worker(worker *w) {
    static const char *const source[] = {
        "entry: mov eax,42\nret",        "entry: mov eax,42\nret",       "entry: mov x0,#42\nret",
        "entry: mov r0,#42\nbx lr",      "entry: movs r0,#42\nbx lr",    "entry: li a0,42\nret",
        "entry: li a0,42\nret",          "entry: li $2,42\njr $31\nnop", "entry: li $2,42\njr $31\nnop",
        "entry: dli $2,42\njr $31\nnop", "entry: dli $2,42\njr $31\nnop"};
    unsigned k;
    for (k = 0; k < 1000; ++k) {
        cstone_options o;
        cstone_result r;
        cstone_options_init(&o);
        o.arch = (cstone_arch)((w->index + k) % CSTONE_ARCH_COUNT);
        o.base_address = 0x1000;
        if (cstone_assemble_ex(source[o.arch], &o, &r) != CSTONE_OK || !r.size || r.label_count != 1)
            w->failed = 1;
        cstone_result_free(&r);
        if (w->failed)
            return;
    }
}
#ifdef _WIN32
static DWORD WINAPI entry(void *context) {
    run_worker((worker *)context);
    return 0;
}
#else
static void *entry(void *context) {
    run_worker((worker *)context);
    return NULL;
}
#endif
int main(void) {
    worker workers[8];
    unsigned k;
    int failed = 0;
#ifdef _WIN32
    HANDLE threads[8];
#else
    pthread_t threads[8];
#endif
    for (k = 0; k < 8; ++k) {
        workers[k].index = k;
        workers[k].failed = 0;
#ifdef _WIN32
        threads[k] = CreateThread(NULL, 0, entry, &workers[k], 0, NULL);
        if (!threads[k])
            return 1;
#else
        if (pthread_create(&threads[k], NULL, entry, &workers[k]))
            return 1;
#endif
    }
    for (k = 0; k < 8; ++k) {
#ifdef _WIN32
        if (WaitForSingleObject(threads[k], INFINITE) != WAIT_OBJECT_0)
            failed = 1;
        CloseHandle(threads[k]);
#else
        if (pthread_join(threads[k], NULL))
            failed = 1;
#endif
        failed |= workers[k].failed;
    }
    if (!failed)
        puts("Concurrency: 8000 assemblies in 8 threads passed");
    return failed ? 1 : 0;
}
