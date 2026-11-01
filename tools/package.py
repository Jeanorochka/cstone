from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def run(command: list[str]) -> str:
    reply = subprocess.run(command, capture_output=True, text=True)
    if reply.returncode:
        raise RuntimeError(f"{' '.join(command)}\n{reply.stdout}\n{reply.stderr}")
    return reply.stdout


def archive(path: Path, files: list[tuple[Path, str]]) -> None:
    temporary = path.with_suffix(".zip.tmp")
    if not temporary.resolve().is_relative_to(path.parent.resolve()):
        raise ValueError("Invalid package destination")
    with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for source, name in sorted(files, key=lambda item: item[1]):
            info = zipfile.ZipInfo(name, date_time=(2026, 11, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o755 if source.suffix in (".exe", ".dll", ".so") or source.name == "cstone" else 0o644) << 16
            output.writestr(info, source.read_bytes())
    temporary.replace(path)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--label", required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    parser.add_argument("--source", action="store_true")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", args.label):
        parser.error("Package label must contain lowercase letters, numbers and hyphens")
    version = re.search(r'#define CSTONE_VERSION "([^"]+)"', (ROOT / "include" / "cstone.h").read_text()).group(1)
    output, build = args.output.resolve(), args.build.resolve()
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    for option in ("CSTONE_SANITIZERS", "CSTONE_ADDRESS_SANITIZER", "CSTONE_FUZZ"):
        if re.search(rf"^{option}:BOOL=ON$", cache, re.MULTILINE):
            raise RuntimeError("Sanitizer builds are not distribution packages")
    if args.configuration.lower() == "debug":
        raise RuntimeError("Debug builds are not distribution packages")
    build_type = re.search(r"^CMAKE_BUILD_TYPE:STRING=(.*)$", cache, re.MULTILINE)
    if build_type and build_type.group(1).strip().lower() == "debug":
        raise RuntimeError("Debug builds are not distribution packages")
    output.mkdir(parents=True, exist_ok=True)
    tests = run(["ctest", "--test-dir", str(build), "-C", args.configuration, "--output-on-failure", "--no-tests=error"])
    if "100% tests passed" not in tests:
        raise RuntimeError("Test gate did not confirm a complete passing suite")
    for required in ("cstone_tests", "cstone_api_tests", "cstone_allocation_tests", "cstone_fuzz_smoke", "cstone_cli_tests", "cstone_thread_tests"):
        if required not in tests:
            raise RuntimeError(f"Release test gate is missing {required}")
    staging = ROOT / "work" / "package-staging"
    staging.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=args.label + "-", dir=staging))
    run(["cmake", "--install", str(build), "--prefix", str(stage), "--config", args.configuration])
    if not (stage / "include" / "cstone.h").is_file():
        raise RuntimeError("Installed public header is missing")
    candidates = list((stage / "bin").glob("cstone*"))
    cli = next((p for p in candidates if p.name in ("cstone", "cstone.exe")), None)
    if cli is None or run([str(cli), "--version"]).strip() != version:
        raise RuntimeError("Installed CLI failed the version check")
    if len(run([str(cli), "--list-arch"]).splitlines()) != 11:
        raise RuntimeError("Installed CLI failed architecture discovery")
    compiler = {}
    for compiler_file in (build / "CMakeFiles").glob("*/CMakeCCompiler.cmake"):
        content = compiler_file.read_text(encoding="utf-8")
        for field in ("ID", "VERSION"):
            match = re.search(rf'set\(CMAKE_C_COMPILER_{field} "([^"]+)"\)', content)
            if match:
                compiler[field.lower()] = match.group(1)
    report = {"version": version, "abi": 1, "label": args.label, "configuration": args.configuration,
              "shared": bool(re.search(r"^BUILD_SHARED_LIBS:BOOL=ON$", cache, re.MULTILINE)),
              "compiler": compiler, "tests": tests.strip(), "cmake": run(["cmake", "--version"]).splitlines()[0]}
    (stage / "VALIDATION.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    prefix = f"cstone-{version}-{args.label}"
    package = output / f"{prefix}.zip"
    archive(package, [(p, f"{prefix}/{p.relative_to(stage).as_posix()}") for p in stage.rglob("*") if p.is_file()])
    if args.source:
        selected = []
        for directory in ("include", "src", "tests", "examples", "cmake", "docs", "fuzz", "tools", ".github"):
            selected += [p for p in (ROOT / directory).rglob("*") if p.is_file() and "__pycache__" not in p.parts and p.suffix != ".pyc"]
        selected += [ROOT / name for name in ("CMakeLists.txt", "build.ps1", "README.md", "LICENSE", "CHANGELOG.md", "SECURITY.md", ".gitignore", ".gitattributes", ".clang-format")]
        archive(output / f"cstone-{version}-source.zip", [(p, f"cstone-{version}/{p.relative_to(ROOT).as_posix()}") for p in selected])
    packages = sorted(output.glob(f"cstone-{version}-*.zip"))
    sums = "".join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n" for p in packages)
    (output / "SHA256SUMS").write_text(sums, encoding="ascii")
    print(package)


if __name__ == "__main__":
    main()
