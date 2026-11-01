#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int add_address(uint64_t value, int64_t addend, uint64_t *out) {
    uint64_t magnitude = addend < 0 ? (uint64_t)(-(addend + 1)) + 1 : (uint64_t)addend;
    if (addend < 0) {
        if (value < magnitude)
            return 0;
        *out = value - magnitude;
    } else {
        if (value > UINT64_MAX - magnitude)
            return 0;
        *out = value + magnitude;
    }
    return 1;
}
static int relative(uint64_t target, uint64_t pc, unsigned width, unsigned alignment, int64_t *out) {
    uint64_t distance = target >= pc ? target - pc : pc - target;
    uint64_t limit = UINT64_C(1) << (width - 1);
    if (distance % alignment || distance > (target >= pc ? limit - 1 : limit))
        return 0;
    *out = target >= pc ? (int64_t)distance : -(int64_t)distance;
    return 1;
}
static unsigned relocation_size(cstone_reloc_kind kind) {
    switch (kind) {
    case CSTONE_ABS8:
        return 1;
    case CSTONE_ABS16:
    case CSTONE_THUMB_BRANCH11:
    case CSTONE_THUMB_BRANCH8:
    case CSTONE_THUMB_LITERAL8:
        return 2;
    case CSTONE_ABS64:
        return 8;
    default:
        return 4;
    }
}
int cstone_i_patch(uint8_t *bytes, size_t length, cstone_arch arch, const cstone_relocation *r,
                   uint64_t address) {
    uint64_t target, pc;
    uint32_t word, u;
    int64_t d = 0;
    unsigned size = relocation_size(r->kind);
    int big = cstone_i_big_endian(arch);
    if (r->offset > length || size > length - r->offset || !add_address(address, r->addend, &target))
        return 0;
    bytes += r->offset;
    if (r->kind >= CSTONE_ABS8 && r->kind <= CSTONE_ABS64) {
        if (size < 8 && target >= (UINT64_C(1) << (size * 8)))
            return 0;
        cstone_i_put(bytes, target, size, big);
        return 1;
    }
    if (cstone_i_arch32(arch) && target > UINT32_MAX)
        return 0;
    if (!add_address(r->place, r->pc_bias, &pc))
        return 0;
    if (r->kind == CSTONE_X86_REL32) {
        if (!relative(target, pc, 32, 1, &d))
            return 0;
        cstone_i_put(bytes, (uint64_t)d, 4, 0);
        return 1;
    }
    if (r->kind == CSTONE_X86_ABS32S) {
        if (target > UINT64_C(0x7fffffff) && target < UINT64_C(0xffffffff80000000))
            return 0;
        cstone_i_put(bytes, target, 4, 0);
        return 1;
    }
    word = size == 2 ? (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) : cstone_i_get32(bytes, big);
    switch (r->kind) {
    case CSTONE_A64_BRANCH26:
        if (!relative(target, pc, 28, 4, &d))
            return 0;
        word = (word & 0xfc000000u) | (((uint32_t)(d / 4)) & 0x03ffffffu);
        break;
    case CSTONE_A64_BRANCH19:
        if (!relative(target, pc, 21, 4, &d))
            return 0;
        word = (word & ~0x00ffffe0u) | ((((uint32_t)(d / 4)) & 0x7ffffu) << 5);
        break;
    case CSTONE_A64_BRANCH14:
        if (!relative(target, pc, 16, 4, &d))
            return 0;
        word = (word & ~0x0007ffe0u) | ((((uint32_t)(d / 4)) & 0x3fffu) << 5);
        break;
    case CSTONE_A64_ADR21:
    case CSTONE_A64_ADRP21:
        if (r->kind == CSTONE_A64_ADRP21) {
            if (!relative(target & ~UINT64_C(4095), pc & ~UINT64_C(4095), 33, 4096, &d))
                return 0;
            d /= 4096;
        } else if (!relative(target, pc, 21, 1, &d))
            return 0;
        u = (uint32_t)d;
        word = (word & ~0x60ffffe0u) | ((u & 3u) << 29) | (((u >> 2) & 0x7ffffu) << 5);
        break;
    case CSTONE_A64_LO12:
        word = (word & ~0x003ffc00u) | (((uint32_t)target & 4095u) << 10);
        break;
    case CSTONE_ARM_BRANCH24:
        if (!relative(target, pc, 26, 4, &d))
            return 0;
        word = (word & 0xff000000u) | ((uint32_t)(d / 4) & 0x00ffffffu);
        break;
    case CSTONE_THUMB_BRANCH11:
    case CSTONE_THUMB_BRANCH8:
        if (!relative(target, pc, r->kind == CSTONE_THUMB_BRANCH11 ? 12u : 9u, 2, &d))
            return 0;
        u = r->kind == CSTONE_THUMB_BRANCH11 ? 0x7ffu : 0xffu;
        word = (word & ~u) | ((uint32_t)(d / 2) & u);
        break;
    case CSTONE_THUMB_LITERAL8:
        pc &= ~UINT64_C(3);
        if (target < pc || target - pc > 1020 || (target - pc) % 4)
            return 0;
        word = (word & ~0xffu) | (uint32_t)((target - pc) / 4);
        break;
    case CSTONE_THUMB_BRANCH24: {
        uint32_t first = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8);
        uint32_t second = (uint32_t)bytes[2] | ((uint32_t)bytes[3] << 8);
        uint32_t sign, j1, j2;
        if (!relative(target, pc, 25, 2, &d))
            return 0;
        u = (uint32_t)d;
        sign = (u >> 24) & 1u;
        j1 = (~((u >> 23) ^ sign)) & 1u;
        j2 = (~((u >> 22) ^ sign)) & 1u;
        first = (first & 0xf800u) | (sign << 10) | ((u >> 12) & 0x3ffu);
        second = (second & 0xd000u) | (j1 << 13) | (j2 << 11) | ((u >> 1) & 0x7ffu);
        cstone_i_put(bytes, first, 2, 0);
        cstone_i_put(bytes + 2, second, 2, 0);
        return 1;
    }
    case CSTONE_RV_JAL20:
        if (!relative(target, pc, 21, 2, &d))
            return 0;
        u = (uint32_t)d;
        word = (word & 0xfffu) | ((u & 0x100000u) << 11) | ((u & 0x7feu) << 20) | ((u & 0x800u) << 9) |
               (u & 0xff000u);
        break;
    case CSTONE_RV_BRANCH12:
        if (!relative(target, pc, 13, 2, &d))
            return 0;
        u = (uint32_t)d;
        word = (word & 0x01fff07fu) | ((u & 0x1000u) << 19) | ((u & 0x7e0u) << 20) | ((u & 0x1eu) << 7) |
               ((u & 0x800u) >> 4);
        break;
    case CSTONE_RV_PCREL_HI20:
    case CSTONE_RV_PCREL_LO12:
        if (!relative(target, r->anchor, 32, 1, &d) || d > INT64_C(2147481599))
            return 0;
        if (r->kind == CSTONE_RV_PCREL_HI20) {
            int64_t rounded = d + 2048;
            int64_t hi = rounded >= 0 ? rounded / 4096 : -((-rounded + 4095) / 4096);
            word = (word & 0xfffu) | (((uint32_t)hi & 0xfffffu) << 12);
        } else
            word = (word & 0xfffffu) | (((uint32_t)d & 0xfffu) << 20);
        break;
    case CSTONE_MIPS_BRANCH16:
        if (!relative(target, pc, 18, 4, &d))
            return 0;
        word = (word & 0xffff0000u) | ((uint32_t)(d / 4) & 0xffffu);
        break;
    case CSTONE_MIPS_JUMP26:
        if (target % 4 || (target & ~UINT64_C(0xfffffff)) != (pc & ~UINT64_C(0xfffffff)))
            return 0;
        word = (word & 0xfc000000u) | ((uint32_t)(target >> 2) & 0x03ffffffu);
        break;
    default:
        return 0;
    }
    cstone_i_put(bytes, word, size, big);
    return 1;
}
cstone_status cstone_link(cstone_result *result, const cstone_binding *bindings, size_t count,
                          cstone_diagnostic *diagnostic) {
    cstone_diagnostic local;
    uint8_t *bytes;
    size_t k, j;
    cstone_status status = CSTONE_OK;
    if (!diagnostic)
        diagnostic = &local;
    memset(diagnostic, 0, sizeof(*diagnostic));
    if (!result || (count && !bindings) || (result->size && !result->bytes) ||
        (result->relocation_count && !result->relocations) || (unsigned)result->arch >= CSTONE_ARCH_COUNT) {
        diagnostic->status = CSTONE_INVALID_ARGUMENT;
        (void)snprintf(diagnostic->message, sizeof(diagnostic->message), "Invalid link arguments.");
        return diagnostic->status;
    }
    for (k = 0; k < count; ++k) {
        if (!bindings[k].name) {
            status = CSTONE_INVALID_ARGUMENT;
            break;
        }
        for (j = 0; j < k; ++j)
            if (!strcmp(bindings[k].name, bindings[j].name)) {
                status = CSTONE_INVALID_ARGUMENT;
                break;
            }
        if (status != CSTONE_OK)
            break;
    }
    if (status != CSTONE_OK) {
        diagnostic->status = status;
        (void)snprintf(diagnostic->message, sizeof(diagnostic->message),
                       "Binding names must be non-NULL and unique.");
        return status;
    }
    if (!result->relocation_count)
        return CSTONE_OK;
    bytes = (uint8_t *)malloc(result->size ? result->size : 1);
    if (!bytes) {
        diagnostic->status = CSTONE_OUT_OF_MEMORY;
        (void)snprintf(diagnostic->message, sizeof(diagnostic->message), "Cannot allocate link buffer.");
        return diagnostic->status;
    }
    if (result->size)
        memcpy(bytes, result->bytes, result->size);
    for (k = 0; k < result->relocation_count; ++k) {
        cstone_relocation *r = &result->relocations[k];
        if (!r->symbol)
            status = CSTONE_INVALID_ARGUMENT;
        else {
            for (j = 0; j < count; ++j)
                if (!strcmp(r->symbol, bindings[j].name))
                    break;
            if (j == count)
                status = CSTONE_UNKNOWN_LABEL;
            else if (!cstone_i_patch(bytes, result->size, result->arch, r, bindings[j].address))
                status = CSTONE_RELOCATION_ERROR;
        }
        if (status != CSTONE_OK) {
            diagnostic->status = status;
            diagnostic->line = r->line;
            diagnostic->column = r->column;
            (void)snprintf(diagnostic->message, sizeof(diagnostic->message),
                           "Cannot resolve external address '%s'.", r->symbol ? r->symbol : "(null)");
            break;
        }
    }
    if (status == CSTONE_OK) {
        if (result->size)
            memcpy(result->bytes, bytes, result->size);
        for (k = 0; k < result->relocation_count; ++k)
            result->relocations[k].resolved = 1;
    }
    free(bytes);
    return status;
}
