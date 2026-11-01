# Release 0.2.0 MVP

## Included

- A C11 core with 11 target profiles and an explicit instruction reference.
- A bounded C API, resource limits, labels, checked relocations, and transactional linking.
- Static/shared builds, controlled public exports, CMake installation, and pkg-config discovery.
- A CLI with JSON, stdin, raw output, and Unicode Windows filenames.
- Encoding, boundary, allocation-failure, threading, and CLI tests.
- Independent encoding comparisons and execution tests.
- Sanitizer configurations, a libFuzzer harness, and Windows/Linux/macOS CI definitions.
- An MIT license, changelog, security policy, packaging script, and SHA-256 checksums.

## Local validation baseline

Windows x64 builds were checked with GCC 16.2, Clang 23.1, and MSVC 19.51; DLLs with GCC and MSVC. MSVC AddressSanitizer covered all CTest groups, including 100,000 input mutations. Ubuntu 24.04 under WSL was checked with GCC 13.3 and AddressSanitizer/UBSan.

Six CTest groups passed, including 8,000 assemblies across eight threads. Installed shared libraries were consumed from separate C and C++ projects with GCC/Windows, MSVC/Windows, and GCC/Linux. The original independent x86-64 set contains 7,291 forms; the additional multi-architecture set contains 8,207 statements. The compared blocks matched across 78,195 bytes.

Independent checks cover encodings, linked branch addresses, and 1,029 execution cases for x86/x64, AArch64, ARM, Thumb, RV32/RV64, and MIPS32 LE/BE. MIPS64 has encoding comparisons but no execution validation. Reports are written to `work/architecture-tests/report.json` and `work/execution-tests/report.json`.

Unpacked source archives were built from scratch. Unpacked Windows shared SDKs were checked with GCC and MSVC C/C++ consumers. Unpacked Linux static/shared SDKs were checked with GCC C/C++ consumers and their installed CLIs.

The local baseline does not include a native macOS run. Current remote CI results are tracked separately in [GitHub Actions](https://github.com/Jeanorochka/cstone/actions).

## Binary packages and runtimes

The local packages target x64 hosts: Windows with MinGW/UCRT or MSVC, and Linux with GCC/glibc. Every package includes all 11 assembly target profiles. Build the source archive for other hosts.

Windows packages were checked on Windows 11. The MinGW builds use the system UCRT and do not require extra libgcc/libstdc++ DLLs. The MSVC builds use the dynamic `/MD` runtime and require `VCRUNTIME140.dll` and UCRT; Microsoft runtime binaries are not bundled. The static MSVC library also expects a compatible application runtime. A static Cstone package does not mean a statically linked system C runtime.

Linux packages were checked on Ubuntu 24.04 x86-64 with glibc. Compatibility with musl or other distributions was not validated. Keep the `bin/` and `lib/` layout together when relocating a shared CLI package.

Each binary archive includes `VALIDATION.json` with the version, build type, compiler, and test results. Verify archives against `SHA256SUMS`. MinGW `.a` and MSVC `.lib` files are toolchain-specific; a shared C API does not make these archive formats interchangeable.

## Packaging

Build the required variant first:

```powershell
.\build.ps1 -Compiler gcc -Shared
python tools\package.py --build work\mvp-gcc-shared --label windows-x64-mingw-shared --source
```

For MSVC, select its build directory and `--configuration Release`. Python 3.10 or newer is required. The script runs the complete release test gate, rejects sanitizer/debug builds, installs into a fresh staging directory, checks the installed CLI, then creates the ZIP.

Source archives contain only source, tests, examples, documentation, and build files. They exclude work directories, Git data, downloaded tools, old builds, and caches.

Archive entries are sorted and have a fixed ZIP timestamp. This makes archives reproducible for identical file contents; it does not promise identical binaries across compiler versions. Checksums are written to `dist/SHA256SUMS`. Archives are local outputs until explicitly published.

## Before a stable production release

1. Pass CI from a clean checkout on every claimed host platform and validate relocated SDKs.
2. Run extended coverage-guided fuzzing, retain regression inputs, and obtain independent encoder review.
3. Configure and verify a private security-reporting channel, release ownership, and a version-support policy.
4. Select the supported host packages and document their runtime requirements.
5. Update the instruction reference and independent tests for every added form. Increment the ABI for incompatible public-layout or enum changes.

Disassembly, complete ISA coverage, automatic relaxation, and ELF/COFF output are outside 0.2.0. Version 1.0 requires a stable API and validated external-user workflows.
