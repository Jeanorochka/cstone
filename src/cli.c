#include "cstone.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

static FILE *open_file(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t *wide;
    wchar_t wide_mode[8];
    FILE *file;
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    size_t k;
    if (!size)
        return NULL;
    wide = (wchar_t *)malloc((size_t)size * sizeof(*wide));
    if (!wide)
        return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, size)) {
        free(wide);
        return NULL;
    }
    for (k = 0; mode[k] && k < 7; ++k)
        wide_mode[k] = (wchar_t)(unsigned char)mode[k];
    wide_mode[k] = 0;
    file = _wfopen(wide, wide_mode);
    free(wide);
    return file;
#else
    return fopen(path, mode);
#endif
}
static void usage(void) {
    puts("Cstone " CSTONE_VERSION " - C11 multi-architecture assembler\n"
         "Usage: cstone [--arch ARCH] input.asm [-o output.bin] [--base ADDRESS]\n"
         "       cstone --list-arch\n"
         "Options: --json --labels --allow-unresolved --define NAME=ADDRESS\n"
         "Input '-' reads stdin; output '-o -' writes raw bytes to stdout.\n"
         "Default architecture: x86_64. See docs/support.md for exact coverage.");
}
static int integer(const char *text, uint64_t *out) {
    char *end;
    unsigned long long value;
    int base;
    if (!*text || *text < '0' || *text > '9')
        return 0;
    base = text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 16 : 10;
    errno = 0;
    value = strtoull(text, &end, base);
    if (errno || *end || value > UINT64_MAX)
        return 0;
    *out = (uint64_t)value;
    return 1;
}
static char *read_source(const char *path, size_t limit, size_t *length) {
    FILE *file = !strcmp(path, "-") ? stdin : open_file(path, "rb");
    char *data;
    size_t size = 0, capacity = 4096;
    if (!file) {
        fprintf(stderr, "Cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    if (capacity > limit + 1)
        capacity = limit + 1;
    data = (char *)malloc(capacity ? capacity : 1);
    if (!data) {
        if (file != stdin)
            fclose(file);
        return NULL;
    }
    for (;;) {
        size_t count;
        if (size == capacity) {
            char *next;
            size_t new_capacity = capacity > limit / 2 ? limit + 1 : capacity * 2;
            if (new_capacity <= capacity) {
                fprintf(stderr, "Source byte limit exceeded.\n");
                free(data);
                if (file != stdin)
                    fclose(file);
                return NULL;
            }
            next = (char *)realloc(data, new_capacity);
            if (!next) {
                free(data);
                if (file != stdin)
                    fclose(file);
                return NULL;
            }
            data = next;
            capacity = new_capacity;
        }
        count = fread(data + size, 1, capacity - size, file);
        size += count;
        if (size > limit) {
            fprintf(stderr, "Source byte limit exceeded.\n");
            free(data);
            if (file != stdin)
                fclose(file);
            return NULL;
        }
        if (!count) {
            if (ferror(file)) {
                fprintf(stderr, "Cannot read input.\n");
                free(data);
                if (file != stdin)
                    fclose(file);
                return NULL;
            }
            break;
        }
    }
    if (file != stdin)
        fclose(file);
    if (size >= 3 && (unsigned char)data[0] == 0xef && (unsigned char)data[1] == 0xbb &&
        (unsigned char)data[2] == 0xbf) {
        memmove(data, data + 3, size - 3);
        size -= 3;
    }
    *length = size;
    return data;
}
static void json_string(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    putchar('"');
    while (*p) {
        unsigned ch = *p++;
        if (ch == '"' || ch == '\\') {
            putchar('\\');
            putchar((int)ch);
        } else if (ch < 32)
            printf("\\u%04x", ch);
        else
            putchar((int)ch);
    }
    putchar('"');
}
static void json_result(const cstone_result *r) {
    size_t k;
    printf("{\"version\":\"%s\",\"arch\":\"%s\",\"status\":", cstone_version(), cstone_arch_name(r->arch));
    json_string(cstone_status_string(r->diagnostic.status));
    printf(",\"base\":\"0x%llx\",\"size\":%zu,\"hex\":\"", (unsigned long long)r->base_address, r->size);
    for (k = 0; k < r->size; ++k)
        printf("%02x", (unsigned)r->bytes[k]);
    printf("\",\"labels\":[");
    for (k = 0; k < r->label_count; ++k) {
        if (k)
            putchar(',');
        printf("{\"name\":");
        json_string(r->labels[k].name);
        printf(",\"offset\":%zu,\"address\":\"0x%llx\"}", r->labels[k].offset,
               (unsigned long long)r->labels[k].address);
    }
    printf("],\"relocations\":[");
    for (k = 0; k < r->relocation_count; ++k) {
        const cstone_relocation *p = &r->relocations[k];
        if (k)
            putchar(',');
        printf("{\"symbol\":");
        json_string(p->symbol);
        printf(
            ",\"offset\":%zu,\"kind\":%u,\"addend\":\"%lld\",\"place\":\"0x%llx\",\"anchor\":\"0x%llx\",\"pc_"
            "bias\":%d,\"line\":%zu,\"column\":%zu,\"resolved\":%s}",
            p->offset, (unsigned)p->kind, (long long)p->addend, (unsigned long long)p->place,
            (unsigned long long)p->anchor, (int)p->pc_bias, p->line, p->column,
            p->resolved ? "true" : "false");
    }
    printf("],\"diagnostic\":{\"line\":%zu,\"column\":%zu,\"message\":", r->diagnostic.line,
           r->diagnostic.column);
    json_string(r->diagnostic.message);
    puts("}}");
}
static int cli_main(int argc, char **argv) {
    const char *input = NULL, *output = NULL;
    int labels = 0, json = 0, k;
    size_t length, j, binding_count = 0;
    char *source;
    cstone_options options;
    cstone_result result;
    cstone_binding bindings[256];
    cstone_options_init(&options);
    if (argc == 1) {
        usage();
        return 2;
    }
    for (k = 1; k < argc; ++k) {
        if (!strcmp(argv[k], "--help") || !strcmp(argv[k], "-h")) {
            usage();
            return 0;
        }
        if (!strcmp(argv[k], "--version")) {
            puts(cstone_version());
            return 0;
        }
        if (!strcmp(argv[k], "--list-arch")) {
            unsigned a;
            for (a = 0; a < CSTONE_ARCH_COUNT; ++a)
                puts(cstone_arch_name((cstone_arch)a));
            return 0;
        }
        if (!strcmp(argv[k], "--labels")) {
            labels = 1;
            continue;
        }
        if (!strcmp(argv[k], "--json")) {
            json = 1;
            continue;
        }
        if (!strcmp(argv[k], "--allow-unresolved")) {
            options.flags |= CSTONE_ALLOW_UNRESOLVED;
            continue;
        }
        if (!strcmp(argv[k], "--arch") && k + 1 < argc) {
            if (cstone_arch_parse(argv[++k], &options.arch) != CSTONE_OK) {
                fprintf(stderr, "Unknown architecture '%s'.\n", argv[k]);
                return 2;
            }
            continue;
        }
        if (!strcmp(argv[k], "--base") && k + 1 < argc) {
            if (!integer(argv[++k], &options.base_address)) {
                fprintf(stderr, "Invalid base address.\n");
                return 2;
            }
            continue;
        }
        if (!strcmp(argv[k], "--define") && k + 1 < argc) {
            char *entry = argv[++k], *equals = strchr(entry, '=');
            if (!equals || equals == entry || binding_count == 256 ||
                !integer(equals + 1, &bindings[binding_count].address)) {
                fprintf(stderr, "Expected --define NAME=ADDRESS (maximum 256).\n");
                return 2;
            }
            *equals = 0;
            bindings[binding_count++].name = entry;
            options.flags |= CSTONE_ALLOW_UNRESOLVED;
            continue;
        }
        if ((!strcmp(argv[k], "-o") || !strcmp(argv[k], "--output")) && k + 1 < argc) {
            output = argv[++k];
            continue;
        }
        if ((argv[k][0] == '-' && strcmp(argv[k], "-")) || input) {
            usage();
            return 2;
        }
        input = argv[k];
    }
    if (!input || (json && output)) {
        usage();
        return 2;
    }
    source = read_source(input, options.max_source_size, &length);
    if (!source)
        return 1;
    (void)cstone_assemble_n(source, length, &options, &result);
    free(source);
    if (result.diagnostic.status == CSTONE_OK && binding_count) {
        cstone_diagnostic diagnostic;
        if (cstone_link(&result, bindings, binding_count, &diagnostic) != CSTONE_OK)
            result.diagnostic = diagnostic;
    }
    if (result.diagnostic.status != CSTONE_OK) {
        if (json)
            json_result(&result);
        else
            fprintf(stderr, "%s:%zu:%zu: %s: %s\n", input, result.diagnostic.line, result.diagnostic.column,
                    cstone_status_string(result.diagnostic.status), result.diagnostic.message);
        cstone_result_free(&result);
        return 1;
    }
    for (j = 0; j < result.relocation_count; ++j)
        if (!result.relocations[j].resolved && !json) {
            fprintf(stderr, "Unresolved symbols require --json or complete --define bindings.\n");
            cstone_result_free(&result);
            return 1;
        }
    if (json)
        json_result(&result);
    else if (output) {
        FILE *file;
        int failed;
        if (!strcmp(output, "-")) {
#ifdef _WIN32
            if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
                cstone_result_free(&result);
                return 1;
            }
#endif
            file = stdout;
        } else
            file = open_file(output, "wb");
        if (!file) {
            fprintf(stderr, "Cannot open output.\n");
            cstone_result_free(&result);
            return 1;
        }
        failed = result.size && fwrite(result.bytes, 1, result.size, file) != result.size;
        if (file == stdout) {
            if (fflush(file))
                failed = 1;
        } else if (fclose(file))
            failed = 1;
        if (failed) {
            fprintf(stderr, "Cannot write output.\n");
            cstone_result_free(&result);
            return 1;
        }
    } else {
        for (j = 0; j < result.size; ++j)
            printf("%s%02X", j ? " " : "", (unsigned)result.bytes[j]);
        putchar('\n');
    }
    if (labels && !json) {
        fflush(stdout);
        for (j = 0; j < result.label_count; ++j)
            fprintf(stderr, "%s: offset=%zu address=0x%llX\n", result.labels[j].name, result.labels[j].offset,
                    (unsigned long long)result.labels[j].address);
    }
    cstone_result_free(&result);
    return ferror(stdout) ? 1 : 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t **wide) {
    char **argv = (char **)calloc((size_t)argc + 1, sizeof(*argv));
    int k, status;
    if (!argv)
        return 1;
    for (k = 0; k < argc; ++k) {
        int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[k], -1, NULL, 0, NULL, NULL);
        if (!size)
            break;
        argv[k] = (char *)malloc((size_t)size);
        if (!argv[k] ||
            !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[k], -1, argv[k], size, NULL, NULL))
            break;
    }
    status = k == argc ? cli_main(argc, argv) : 1;
    for (k = 0; k < argc; ++k)
        free(argv[k]);
    free(argv);
    return status;
}
#else
int main(int argc, char **argv) {
    return cli_main(argc, argv);
}
#endif
