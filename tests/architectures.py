from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ORACLES = ROOT / "work" / "oracle-deps"
if ORACLES.exists():
    sys.path.insert(0, str(ORACLES))


def run(command: list[str]) -> str:
    command = [shutil.which(command[0]) or command[0], *command[1:]]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{' '.join(command)}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def groups() -> dict[str, list[tuple[str, list[str]]]]:
    result: dict[str, list[tuple[str, list[str]]]] = {}
    for arch, regs, bases in (
        ("x86_64", ["eax", "r9d", "rax", "r15"], ["rax", "rbp", "rsp", "r12", "r13"]),
        ("x86", ["eax", "ecx", "edi"], ["eax", "ebp", "esp", "edi"]),
    ):
        lines = []
        for r in regs:
            for base in bases:
                for displacement in (0, 1, -128, 127, 128, -129, 65536):
                    address = f"[{base} + {displacement}]" if displacement >= 0 else f"[{base} - {-displacement}]"
                    for op in ("mov", "add", "sub", "cmp", "and", "or", "xor", "test"):
                        lines += [f"{op} {r}, {address}", f"{op} {address}, {r}"]
        for r in ("al", "cl", "ah", "bh", "ax", "cx"):
            for value in (0, 1, 127):
                lines.append(f"mov {r}, {value}")
        lines += ["movzx eax, byte ptr [ebp + 8]", "movsx ecx, word ptr [esp + 16]", "lea eax, [eax + ecx*4 + 128]"]
        result[arch] = [("memory", lines)]
    a64 = []
    for width in ("w", "x"):
        for rd in (0, 1, 15, 29):
            for rn in (0, 8, 30):
                for op in ("add", "sub", "adds", "subs", "and", "orr", "eor", "ands"):
                    a64.append(f"{op} {width}{rd}, {width}{rn}, {width}7")
                for value in (0, 1, 127, 4095):
                    for op in ("add", "sub", "adds", "subs"):
                        a64.append(f"{op} {width}{rd}, {width}{rn}, #{value}")
                for op in ("lsl", "lsr", "asr"):
                    for amount in (0, 1, 15, 31):
                        a64.append(f"{op} {width}{rd}, {width}{rn}, #{amount}")
    a64 += ["movz x0, #42", "movk x1, #0xabcd, lsl #32", "movn w3, #123, lsl #16", "mov sp, x0", "mov x1, sp", "sub sp, sp, #32",
            "and sp,x0,#0xff", "ands xzr,x0,#0xff", "mul x0,x1,x2", "mneg w3,w4,w5", "madd x6,x7,x8,x9", "msub w10,w11,w12,w13", "sdiv x14,x15,x16", "udiv w17,w18,w19"]
    for op, register, scale in (("ldr", "x0", 8), ("str", "w1", 4), ("ldrb", "w2", 1), ("strh", "w3", 2)):
        for n in (0, 1, 31, 4095):
            a64.append(f"{op} {register}, [x8, #{n * scale}]")
    a64 += ["ldr x0, [sp, #-8]!", "str w1, [x3], #4", "ldur x9, [x8, #-256]", "stp x0, x1, [sp, #-16]!", "ldp x0, x1, [sp], #16", "ret"]
    result["aarch64"] = [("integer", a64), ("labels", [".Lstart:", "nop", "b .Lend", "bl .Lstart", "b.eq .Lend", "cbnz x0, .Lstart", "tbz x1, #40, .Lend", "adr x2, .Lstart", "adrp x3, .Lstart", "add x3, x3, :lo12:.Lstart", ".Lend:", "ret"])]
    arm = []
    for a in (0, 7, 12):
        for b in (1, 8, 14):
            for op in ("add", "sub", "rsb", "and", "orr", "eor", "bic", "adc", "sbc"):
                arm += [f"{op} r{a}, r{b}, r3", f"{op}s r{a}, r{b}, #255", f"{op}ne r{a}, r{b}, r4, lsl #7"]
    arm += ["mov r0, r1", "mvn r2, #255", "movw r3, #0xabcd", "movt r3, #0x1234", "cmp r1, #0xff00", "tst r2, r3", "teq r2, r3", "push {r4-r7,lr}", "pop {r4-r7,pc}", "ldr r0, [sp, #12]", "str r1, [r2, #-4095]!", "ldrb r3, [r4], #4", "nop", "bx lr"]
    result["arm"] = [("integer", arm), ("labels", [".Lstart:", "nop", "b .Lend", "bl .Lstart", "bne .Lstart", ".Lend:", "bx lr"])]
    thumb = []
    for a in (0, 3, 7):
        for b in (1, 4, 6):
            thumb += [f"movs r{a}, #255", f"mov r{a}, r{b}", f"adds r{a}, r{b}, r2", f"subs r{a}, r{b}, #7"]
            for op in ("ands", "eors", "adcs", "sbcs", "orrs", "bics", "mvns", "negs"):
                thumb.append(f"{op} r{a}, r{b}")
            for op in ("lsls", "lsrs", "asrs"):
                thumb.append(f"{op} r{a}, r{b}, #7")
    thumb += ["movw r8, #0xabcd", "movt r9, #0x1234", "push {r4-r7,lr}", "pop {r4-r7,pc}", "ldr r0, [sp, #1020]", "strb r1, [r2, #31]", "ldrh r3, [r4, #62]", "sub sp, #508", "add sp, #508", "bx lr", "nop"]
    result["thumb"] = [("integer", thumb), ("labels", [".Lstart:", "nop", "b .Lend", "bne .Lstart", "bl .Lstart", "b.w .Lend", ".Lend:", "bx lr"])]
    for arch in ("riscv32", "riscv64"):
        lines = []
        for rd in (0, 1, 15, 31):
            for rs in (0, 7, 30):
                for op in ("add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and", "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"):
                    lines.append(f"{op} x{rd}, x{rs}, x8")
                for value in (-2048, -1, 0, 1, 2047):
                    for op in ("addi", "slti", "sltiu", "xori", "ori", "andi"):
                        lines.append(f"{op} x{rd}, x{rs}, {value}")
                for amount in (0, 1, 31):
                    for op in ("slli", "srli", "srai"):
                        lines.append(f"{op} x{rd}, x{rs}, {amount}")
        for op in ("lb", "lh", "lw", "lbu", "lhu", "sb", "sh", "sw"):
            for value in (-2048, -1, 0, 2047):
                lines.append(f"{op} a0, {value}(sp)")
        lines += ["lui a0, 0xfffff", "auipc t0, 0x12345", "csrrw a0, mstatus, a1", "csrrsi t0, cycle, 31", "ecall", "ebreak", "fence", "fence.i", "ret"]
        if arch == "riscv64":
            lines += ["ld a0, 2040(sp)", "sd a1, -2048(sp)", "addiw a2, a3, -2048", "addw a0, a1, a2", "mulw t0, t1, t2", "srai t0, t1, 63", "slliw a0, a1, 31"]
        result[arch] = [("integer", lines), ("labels", [".Lstart:", "nop", "j .Lend", "jal ra, .Lstart", "beq a0, a1, .Lend", "bne a1, a2, .Lstart", "blt a2, a3, .Lend", "bgeu a3, a4, .Lstart", ".Lend:", "ret"])]
    for arch in ("mips32le", "mips32be", "mips64le", "mips64be"):
        lines = []
        for rd in (2, 8, 16, 31):
            for rs in (0, 7, 30):
                for op in ("add", "addu", "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu"):
                    lines.append(f"{op} ${rd}, ${rs}, $9")
                for value in (-32768, -1, 0, 32767):
                    for op in ("addi", "addiu", "slti", "sltiu"):
                        lines.append(f"{op} ${rd}, ${rs}, {value}")
                for value in (0, 1, 65535):
                    for op in ("andi", "ori", "xori"):
                        lines.append(f"{op} ${rd}, ${rs}, {value}")
        lines += ["lui $8, 0xffff", "lw $2, -32768($29)", "sw $3, 32767($4)", "sll $2, $3, 31", "srlv $4, $5, $6", "mfhi $8", "mtlo $9", "mul $2,$3,$4", "mult $5,$6", "div $zero,$7,$8", "syscall", "break", "jr $31", "nop"]
        if "64" in arch:
            lines += ["daddu $2, $3, $4", "daddiu $8, $9, -32768", "ld $2, 16($29)", "sd $3, -16($29)", "dsll $2, $3, 31"]
        while len(lines) % 4:
            lines.append("nop")
        result[arch] = [("integer", lines), ("labels", [".Lstart:", "nop", "beq $2, $3, .Lend", "nop", "bne $4, $5, .Lstart", "nop", ".Lend:", "jr $31", "nop", "nop"])]
    return result


TARGETS = {
    "x86_64": ("x86_64-unknown-linux-gnu", [], ".intel_syntax noprefix\n"),
    "x86": ("i386-unknown-linux-gnu", [], ".intel_syntax noprefix\n"),
    "aarch64": ("aarch64-unknown-linux-gnu", [], ""),
    "arm": ("armv7-unknown-linux-gnueabi", ["-march=armv7-a"], ".syntax unified\n.arm\n"),
    "thumb": ("thumbv7-unknown-linux-gnueabi", ["-march=armv7-a", "-mthumb"], ".syntax unified\n.thumb\n"),
    "riscv32": ("riscv32-unknown-linux-gnu", ["-march=rv32im_zicsr_zifencei"], ".option norvc\n.option norelax\n"),
    "riscv64": ("riscv64-unknown-linux-gnu", ["-march=rv64im_zicsr_zifencei"], ".option norvc\n.option norelax\n"),
}


def compare(arch: str, name: str, lines: list[str], cli: str, directory: Path) -> dict:
    source = directory / f"{arch}-{name}.asm"
    source.write_text("\n".join(lines) + "\n", encoding="ascii")
    record = json.loads(run([cli, "--arch", arch, str(source), "--base", "0x1000", "--json"]))
    if record["status"] != "ok":
        raise AssertionError(f"{source}: {record['diagnostic']}")
    actual = bytes.fromhex(record["hex"])
    if arch.startswith("mips"):
        import keystone
        mode = keystone.KS_MODE_MIPS64 if "64" in arch else keystone.KS_MODE_MIPS32
        mode |= keystone.KS_MODE_BIG_ENDIAN if arch.endswith("be") else keystone.KS_MODE_LITTLE_ENDIAN
        engine = keystone.Ks(keystone.KS_ARCH_MIPS, mode)
        reference, _ = engine.asm(".set noreorder\n" + "\n".join(lines), addr=0x1000)
        expected = bytes(reference)
        oracle = f"Keystone {keystone.__version__}"
    else:
        triple, flags, prelude = TARGETS[arch]
        asm, obj, elf, binary = (source.with_suffix(ext) for ext in (".s", ".o", ".elf", ".bin"))
        asm.write_text(".text\n" + prelude + "\n".join(lines) + "\n", encoding="ascii")
        run(["clang", f"--target={triple}", *flags, "-c", str(asm), "-o", str(obj)])
        run(["ld.lld", "--image-base=0", "-Ttext=0x1000", "--entry=0", "--no-relax", str(obj), "-o", str(elf)])
        run(["llvm-objcopy", "-O", "binary", "-j", ".text", str(elf), str(binary)])
        expected = binary.read_bytes()
        oracle = "Clang + LLD"
    source.with_suffix(".actual.bin").write_bytes(actual)
    source.with_suffix(".reference.bin").write_bytes(expected)
    if actual != expected:
        offset = next((k for k, (a, b) in enumerate(zip(actual, expected)) if a != b), min(len(actual), len(expected)))
        raise AssertionError(f"{arch}/{name}: byte {offset}, lengths {len(actual)}/{len(expected)}, actual={actual[offset:offset+16].hex()}, reference={expected[offset:offset+16].hex()}; see {source}")
    count = sum(bool(s) and not s.startswith(".") for s in lines)
    print(f"{arch}/{name}: {count} statements, {len(actual)} bytes match {oracle}")
    return {"arch": arch, "group": name, "statements": count, "bytes": len(actual), "sha256": hashlib.sha256(actual).hexdigest(), "oracle": oracle}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cstone", required=True, type=Path)
    parser.add_argument("--arch", action="append")
    args = parser.parse_args()
    for tool in ("clang", "ld.lld", "llvm-objcopy"):
        if not shutil.which(tool):
            parser.error(f"Missing development tool: {tool}")
    directory = ROOT / "work" / "architecture-tests"
    directory.mkdir(parents=True, exist_ok=True)
    records = []
    for arch, batches in groups().items():
        if args.arch and arch not in args.arch:
            continue
        for name, lines in batches:
            records.append(compare(arch, name, lines, str(args.cstone.resolve()), directory))
    (directory / "report.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: {len(records)} groups, {sum(r['statements'] for r in records)} statements, {sum(r['bytes'] for r in records)} bytes")


if __name__ == "__main__":
    main()
