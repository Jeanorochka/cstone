# C API and ABI 1

Version 0.2.0 introduces ABI 1. It is not binary-compatible with the 0.1 prototype. Changes to public structure layouts or enum contracts require an ABI increment. CMake package compatibility is limited to the same minor version.

## Assembly lifecycle

1. Create a `cstone_options` and call `cstone_options_init`.
2. Set `arch`, `base_address`, and any resource limits.
3. Call `cstone_assemble_n(source, length, &options, &result)` for a bounded buffer, or `cstone_assemble_ex` for a NUL-terminated string.
4. Check the returned `cstone_status`.
5. Call `cstone_result_free`, including after a failed assembly.

A NULL options pointer selects x86-64 and the default limits. `cstone_assemble(source, base, &result)` is the x86-64 convenience interface. A non-NULL empty source is valid; a NULL source is an error.

Assembly overwrites the result without reading its previous contents. Free an earlier result before reusing the same variable. Do not copy a result for independent ownership, or modify its owned pointers and counts.

Free all owned memory through the library that created it, especially across DLL or C-runtime boundaries. `cstone_result_free(NULL)` is safe. Freeing an already-cleared result again is also safe.

## Default limits

| Field | Default |
| --- | ---: |
| `max_source_size` | 1 MiB |
| `max_output_size` | 16 MiB |
| `max_symbols` | 8,192 |
| `max_statements` | 100,000 |

Limits can be lowered or explicitly increased. Zero means a zero allowance, not unlimited capacity. The statement limit counts nonempty lines after comment removal. Symbols include definitions and references. Operand delimiter nesting is limited to 32 levels. Labels are indexed by a hash table.

These limits bound input and logical structures, rather than every internal byte allocated with malloc. Cstone has no built-in timeout. A service processing external input must manage its own execution and resource quotas.

## Results and diagnostics

- `bytes` / `size`: emitted machine code.
- `labels` / `label_count`: definitions in source order, including name, offset, and absolute address.
- `relocations` / `relocation_count`: external references left unresolved during assembly.
- `arch`, `base_address`: target profile and block base.
- `diagnostic`: status, source line, byte column, and the first error message.

Line and column numbers start at 1. Argument errors or whole-input limit failures can use 0. The bounded API rejects embedded NUL bytes. Unicode comments do not affect instruction encoding; symbol names use ASCII syntax.

On assembly failure, `bytes`, `labels`, and `relocations` are NULL and their counts are zero. There is no partial result. A successful empty block may also have a NULL byte pointer with size zero.

## External symbols

An undefined label is an error by default. `CSTONE_ALLOW_UNRESOLVED` allows assembly to return relocation sites:

```c
cstone_options options;
cstone_result result;
cstone_diagnostic diagnostic;
cstone_binding bindings[] = {{"helper", 0x2000}};
cstone_options_init(&options);
options.base_address = 0x1000;
options.flags = CSTONE_ALLOW_UNRESOLVED;

if (cstone_assemble_ex("call helper", &options, &result) == CSTONE_OK) {
    if (cstone_link(&result, bindings, 1, &diagnostic) == CSTONE_OK) {
        /* E8 FB 0F 00 00 */
    }
}
cstone_result_free(&result);
```

A relocation contains a symbol name, offset, kind, addend, place, anchor, PC bias, and source position. Do not construct these fields manually: their interpretation depends on the target architecture. After a successful link, `resolved` becomes 1. The relocation table remains available for inspection and relinking.

JSON represents 64-bit addresses as hexadecimal strings and addends as decimal strings. This avoids precision loss in clients with limited JSON-number precision.

`cstone_link` is transactional. Missing bindings, misalignment, range errors, or allocation failures leave the bytes and resolved flags unchanged. Binding names must be non-NULL and unique. Relinking requires the full binding set again. The diagnostic pointer may be NULL.

Unresolved bytes are an intermediate result. The CLI only emits them in JSON with relocation metadata, or emits final raw bytes after successful linking. This is not an ELF/COFF object format.

## Threads and errors

Independent calls with different result/options structures may run concurrently. Reading, linking, or freeing the same mutable result concurrently requires caller-provided synchronization.

Library errors do not call exit or abort, and do not write to stdout or stderr. `cstone_status_string` returns a static string; `diagnostic.message` belongs to the result. Address and offset arithmetic is checked without intentional overflow or wraparound.
