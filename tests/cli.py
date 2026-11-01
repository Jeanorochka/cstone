from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import unittest


class CommandLineTests(unittest.TestCase):
    executable: Path
    directory: Path

    def call(self, *args: str, data: bytes | None = None) -> subprocess.CompletedProcess:
        return subprocess.run([str(self.executable), *args], input=data, capture_output=True)

    def test_architectures(self):
        reply = self.call("--list-arch")
        self.assertEqual(reply.returncode, 0)
        self.assertEqual(len(reply.stdout.splitlines()), 11)

    def test_stdin(self):
        reply = self.call("-", data=b"mov eax,42\nret\n")
        self.assertEqual(reply.returncode, 0)
        self.assertEqual(reply.stdout.strip(), b"B8 2A 00 00 00 C3")

    def test_raw_stdout(self):
        reply = self.call("-", "-o", "-", data=b"db 10,13,26\n")
        self.assertEqual(reply.returncode, 0)
        self.assertEqual(reply.stdout, b"\x0a\x0d\x1a")

    def test_json_and_labels(self):
        reply = self.call("-", "--json", "--base", "0x1000", data=b"entry: mov eax,42\nret")
        record = json.loads(reply.stdout)
        self.assertEqual(reply.returncode, 0)
        self.assertEqual(record["hex"], "b82a000000c3")
        self.assertEqual(record["labels"], [{"name": "entry", "offset": 0, "address": "0x1000"}])

    def test_error_json_architecture(self):
        reply = self.call("--arch", "aarch64", "-", "--json", data=b"nop\nunknown")
        self.assertEqual(reply.returncode, 1)
        record = json.loads(reply.stdout)
        self.assertEqual(record["arch"], "aarch64")
        self.assertEqual(record["diagnostic"]["line"], 2)
        self.assertEqual(record["hex"], "")

    def test_external_symbol(self):
        reply = self.call("-", "--json", "--allow-unresolved", data=b"call helper")
        self.assertEqual(reply.returncode, 0)
        record = json.loads(reply.stdout)
        self.assertEqual(record["relocations"][0]["symbol"], "helper")
        self.assertFalse(record["relocations"][0]["resolved"])

    def test_external_link(self):
        reply = self.call("-", "--define", "helper=0x2000", "--base", "0x1000", data=b"call helper")
        self.assertEqual(reply.returncode, 0)
        self.assertEqual(reply.stdout.strip(), b"E8 FB 0F 00 00")

    def test_unresolved_raw_output_rejected(self):
        reply = self.call("-", "--allow-unresolved", "-o", "-", data=b"call helper")
        self.assertEqual(reply.returncode, 1)
        self.assertEqual(reply.stdout, b"")

    def test_embedded_nul(self):
        reply = self.call("-", "--json", data=b"nop\0ret")
        self.assertEqual(reply.returncode, 1)
        self.assertEqual(json.loads(reply.stdout)["diagnostic"]["column"], 4)

    def test_unicode_filename(self):
        path = self.directory / "\u043f\u0440\u0438\u043c\u0435\u0440 \u0438\u0441\u0445\u043e\u0434\u043d\u0438\u043a\u0430.asm"
        path.write_bytes(b"\xef\xbb\xbfmov eax,42\nret\n")
        reply = self.call(str(path))
        self.assertEqual(reply.returncode, 0, reply.stderr)
        self.assertEqual(reply.stdout.strip(), b"B8 2A 00 00 00 C3")

    def test_failed_assembly_keeps_existing_output(self):
        target = self.directory / "existing.bin"
        target.write_bytes(b"preserve")
        reply = self.call("-", "-o", str(target), data=b"unknown_instruction")
        self.assertEqual(reply.returncode, 1)
        self.assertEqual(target.read_bytes(), b"preserve")

    def test_argument_errors(self):
        self.assertEqual(self.call("--arch", "imaginary", "-").returncode, 2)
        self.assertEqual(self.call("--base", "-1", "-").returncode, 2)
        self.assertEqual(self.call().returncode, 2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cstone", required=True, type=Path)
    args = parser.parse_args()
    CommandLineTests.executable = args.cstone.resolve()
    identity = hashlib.sha256(str(CommandLineTests.executable).encode()).hexdigest()[:12]
    CommandLineTests.directory = Path(__file__).resolve().parents[1] / "work" / "cli-fixtures" / identity
    CommandLineTests.directory.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[__file__])
