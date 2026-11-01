# Supported instruction subset: 0.2.0

All profiles emit raw bytes. Unknown instructions and unsupported operand forms return an error. Selecting a profile does not check which extensions a future physical CPU implements.

## Common syntax

- One instruction per line, with LF or CRLF endings. `;` and `//` start comments. x86, RISC-V, and MIPS also accept `#` comments; Arm uses `#` for immediate operands.
- Mnemonics and register names are case-insensitive. Labels are case-sensitive: `Entry` and `entry` differ.
- Labels use `name:`. A symbol starts with an ASCII letter, `_`, `.`, or `$`; later characters may also be digits. x86 register names are reserved.
- Integers may be decimal, `0x` hexadecimal, or `0b` binary, with an optional sign. `010` means decimal ten. General expressions, macros, and string literals are not implemented.
- Address expressions accept `label`, `label+integer`, `label-integer`, or an absolute integer address. A numeric branch operand always denotes the final target address.
- Data directives: `db/dw/dd/dq` and `.byte/.short/.hword/.long/.word/.quad`. `.word` is 32 bits. Multibyte data follows the selected profile's byte order.
- `.align N` and `.balign N` align the absolute address to N bytes. N must be a power of two from 1 to 4096. Padding defaults to zero; an optional second argument selects another fill byte. `.zero` and `.space` reserve a byte count with the same optional fill argument.
- `.text` is accepted without arguments. Sections, in-file architecture switches, numeric local labels, and `.equ` are not implemented.
- Address arithmetic does not wrap around the address-space boundary. For 32-bit profiles, the start and end of the output block must fit in 32 bits.

## x86 and x86-64

Intel operand order: destination, source. General-purpose registers have 8/16/32-bit forms; 64-bit registers and R8-R15 are available in x86-64. AH/CH/DH/BH restrictions are checked when a REX prefix is required.

| Group | Instructions and forms |
| --- | --- |
| Data movement | `mov`, `lea`, `movzx`, `movsx`, `movsxd` |
| Arithmetic | `add`, `adc`, `sub`, `sbb`, `cmp`, `inc`, `dec`, `neg`, `mul`, `imul`, `div`, `idiv` |
| Logic | `and`, `or`, `xor`, `test`, `not`, `xchg` |
| Shifts and rotates | `rol`, `ror`, `rcl`, `rcr`, `shl/sal`, `shr`, `sar`; immediate count or CL |
| Stack | `push`, `pop`, `ret`, `ret imm16`, `leave` |
| Control flow | `jmp`, `call`, Jcc, SETcc, CMOVcc |
| Other | `nop`, `int`, `int3`, `ud2`, `syscall`, `sysenter`, `cpuid`, `rdtsc`, `clc`, `stc`, `cld`, `std`, `pause`, `endbr32`, `endbr64` |

Arithmetic and MOV include supported register/register, register/memory, memory/register, and immediate forms. Memory-to-memory operations are invalid. IMUL supports one operand, two operands, and register/rm/immediate forms. TEST with an immediate, MOVZX/MOVSX, and unary memory operations require a known operand width.

Memory syntax is `[base + index*scale +/- displacement]`, with scale 1/2/4/8. Width prefixes may be `byte ptr`, `word ptr`, `dword ptr`, `qword ptr`, or the same names without `ptr`. x86-64 supports 32-bit addressing with a 67h prefix and RIP-relative `[rip + label + addend]`. A number in `[rip + number]` is a displacement; a symbol denotes the target address. Absolute memory in 64-bit mode is restricted to sign-extendable disp32.

Direct branches always use rel32; short-branch selection is not automatic. SIMD/FPU, segment addressing, 16-bit mode, LOCK/REP, and special registers are not implemented.

## AArch64

Little-endian profile with 4-byte instructions. Registers: X0-X30/W0-W30, XZR/WZR, and SP/WSP. SP/ZR roles are checked for each instruction form.

- `mov` register/register and integer expansion; `movz/movn/movk` with LSL in multiples of 16.
- `add/sub/adds/subs`, `cmp/cmn`: registers with optional LSL, or imm12 with LSL 0/12.
- `and/orr/eor/ands`: registers with LSL, or an encodable logical bitmask immediate.
- Immediate `lsl/lsr/asr`; `mul/mneg/madd/msub`, `sdiv/udiv`.
- `b/bl`, `b.cond`, `cbz/cbnz`, `tbz/tbnz`, `br/blr/ret`.
- `adr/adrp`; `add ..., :lo12:symbol`.
- `ldr/str`, `ldrb/strb`, `ldrh/strh`, `ldur/stur`; literal LDR; `ldp/stp`.
- Memory forms: `[Xn|SP,#offset]`, pre-index `!`, and post-index. Range, scale, and writeback/data-register overlap are checked.
- `nop`, `svc/hvc/smc`, `brk/hlt`.

SIMD/SVE, floating-point, system registers, register-offset memory, and register-controlled shifts are not implemented. MOV with a large integer may emit multiple instructions.

## ARMv7 ARM state

Little-endian, 4-byte alignment. Registers: R0-R15 and SP/LR/PC/IP/FP aliases.

- `and/eor/sub/rsb/add/adc/sbc/rsc/tst/teq/cmp/cmn/orr/mov/bic/mvn`, with rotated imm8 or a register and immediate LSL/LSR/ASR/ROR. Data-processing instructions support condition suffixes and S.
- `movw/movt`, `mul`, `b/bl`, and register `bx/blx`.
- `ldr/str/ldrb/strb` with immediate offsets up to +/-4095 and pre/post-index forms.
- `push/pop` with register lists and ranges; `nop`, `svc`, `bkpt`.

MOV does not automatically expand arbitrary 32-bit integers; use MOVW/MOVT. VFP/NEON, register-offset memory, and broader suffix combinations are not implemented.

## Thumb and selected Thumb-2 forms

Little-endian, 2-byte alignment, ARMv7 profile. Most supported forms are 16-bit instructions for R0-R7; selected forms accept high registers.

- `mov`, `movs`, `movw/movt`, `cmp`; `adds/subs`, ADD/SUB SP with an immediate, and register/register ADD.
- `ands/eors/adcs/sbcs/rors/tst/cmn/orrs/muls/bics/mvns/negs`, register `lsls/lsrs/asrs`, and immediate `lsls/lsrs/asrs`.
- `b`, short conditional branches, `b.w`, `bl`, `bx/blx`.
- `ldr/str/ldrb/strb/ldrh/strh` with a positive scaled offset; SP-relative word memory; literal LDR and ADR.
- `push/pop` with R0-R7 plus LR/PC; `nop`, `svc/bkpt/udf`.

IT blocks, automatic short-branch expansion, arbitrary Thumb-2 forms, and VFP/NEON are not implemented. Use `b.w` for a long unconditional branch.

## RISC-V RV32/RV64

Little-endian with 4-byte instruction alignment. Base integer instructions and selected M/Zicsr/Zifencei forms. Registers: X0-X31 and standard ABI aliases.

- `add/sub/sll/slt/sltu/xor/srl/sra/or/and` and their applicable immediate forms.
- `mul/mulh/mulhsu/mulhu/div/divu/rem/remu`; implemented W forms in RV64.
- `lui/auipc`, `jal/jalr`, `beq/bne/blt/bge/bltu/bgeu`, and selected zero aliases.
- `lb/lh/lw/lbu/lhu`, `sb/sh/sw`; RV64 also has `ld/lwu/sd`. Memory syntax is `signed_imm12(register)`.
- `li/mv/not/neg/seqz/snez/sext.w`, `j/jr/ret`, `la/call/tail`.
- `csrrw/csrrs/csrrc` and immediate variants; `csrr/csrw`.
- `nop/ecall/ebreak`, argument-free `fence` (IORW,IORW), and `fence.i`.

LI may emit multiple instructions. LA/CALL/TAIL use a PC-relative pair. Compressed C instructions, A atomics, floating-point, and vector instructions are not implemented. There is no relaxation or PIC/GOT mode.

## MIPS32 and MIPS64

Little-endian and big-endian profiles with 4-byte alignment. Selected MIPS32r2/MIPS64r2 instructions. MIPS32 uses o32 register aliases; MIPS64 uses n64 aliases, so `$t0` is `$12` in MIPS64. Numeric `$0` through `$31` names are unambiguous.

- `add/addu/sub/subu/and/or/xor/nor/slt/sltu`, immediate forms, and `lui`.
- `mul`, `mult/multu/div/divu`, `mfhi/mflo/mthi/mtlo`.
- `sll/srl/sra` and V variants; MIPS64 D arithmetic, D shifts, and `dmult/dmultu/ddiv/ddivu`.
- `lb/lh/lw/lbu/lhu/sb/sh/sw/ll/sc`; MIPS64 also has `lwu/ld/sd/lld/scd`.
- `beq/bne/bltz/bgez/bltzal/bgezal/blez/bgtz`, `j/jal/jr/jalr`, `b/bal`.
- `li/dli/move/not/neg/negu`, `nop/syscall/break/sync`.

Two-operand DIV denotes the hardware HI/LO operation. Explicit `div $zero,rs,rt` is also accepted. BREAK accepts up to two unsigned imm10 operands.

**The caller must supply each delay slot.** Cstone does not insert NOPs or reorder instructions. J/JAL targets are restricted to the current 256-MiB region. microMIPS, MIPS16, DSP/MSA, floating-point, and PIC/GOT are not implemented.

## References and validation

Encoding checks use the [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html), [Arm A64 reference](https://developer.arm.com/documentation/ddi0602/latest/), [Armv7 reference](https://documentation-service.arm.com/static/5f8daeb7f86e16515cdb8c4e), and [RISC-V specification](https://docs.riscv.org/reference/isa/unpriv/rv32.html). MIPS follows MD00086/MIPS32 and the corresponding MIPS64 formats. Independent tests use Clang/LLD and Keystone; their implementations are not included in Cstone.
