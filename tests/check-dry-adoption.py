#!/usr/bin/env python3
"""Offline dry manifest controls for lmok --lmcode and check startup."""
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/lmok").resolve()
SCHEMA = ROOT / "schema/lmok.dsl.json"
INPUT = ROOT / "tests/fixtures/c-logic-clean.c"


def u16(value):
    return struct.pack(">H", value)

def u32(value):
    return struct.pack(">I", value)

def make_program(schema_bytes, sentinel):
    imports = [
        "fn file_write(string, string) -> void",
        "fn lmcode_return_set(bool) -> void",
    ]
    constants = imports + [str(sentinel), "h6 sentinel", "true"]
    pool = u32(len(constants)) + b"".join(b"\x01" + u32(len(item.encode())) + item.encode() for item in constants)
    import_table = u16(len(imports)) + u32(0) + u32(1)
    code = (
        b"\x02" + u32(2) + b"\x02" + u32(3) + b"\x07" + u16(0) + b"\x02"
        + b"\x03\x01" + b"\x07" + u16(1) + b"\x01" + b"\x11"
    )
    return (
        b"DSLB" + u16(6) + hashlib.sha256(schema_bytes).digest() + pool + import_table
        + u16(0) + u16(0) + u16(2) + u32(len(code)) + code + u32(0) + u16(0)
    )


def run_case(mode, dry):
    with tempfile.TemporaryDirectory(prefix="lmok-h6-dry-") as folder:
        root = Path(folder)
        sentinel = root / "sentinel.txt"
        bytecode = root / "sentinel.dslb"
        bytecode.write_bytes(make_program(SCHEMA.read_bytes(), sentinel))
        manifest = root / "invocation.json"
        manifest.write_text(json.dumps({
            "version": 1,
            "mode": "single",
            "dry": dry,
            "parts": [{"index": 0, "file": str(bytecode)}],
        }), encoding='utf-8')
        env = dict(os.environ, LMCODE_INVOCATION_FILE=str(manifest))
        if mode == "--lmcode":
            args = [str(HOST), "--lmcode", str(bytecode)]
        else:
            args = [str(HOST), "check", "--lang", "c", str(bytecode), str(INPUT)]
        result = subprocess.run(args, env=env, capture_output=True)
        lines = result.stdout.splitlines()
        starts = [i for i, line in enumerate(lines) if line == b"LMCODE_RETURN_BEGIN"]
        ends = [i for i, line in enumerate(lines) if line == b"LMCODE_RETURN_END"]
        assert len(starts) == len(ends) == 1 and ends[0] == starts[0] + 2, (mode, result.returncode, result.stdout, result.stderr)
        envelope = json.loads(lines[starts[0] + 1])
        assert result.returncode == 0 and envelope.get("ok") is True and envelope.get("result") is True, (mode, dry, result.returncode, envelope, result.stderr)
        assert (envelope.get("dry") is True) == dry, (mode, dry, envelope)
        assert sentinel.exists() is not dry, (mode, dry, "effect sentinel state", sentinel.exists())
        if dry:
            assert not sentinel.exists(), (mode, "dry callback wrote sentinel")
        else:
            assert sentinel.read_bytes() == b"h6 sentinel", (mode, sentinel.read_bytes())


commands = [sys.argv[2]] if len(sys.argv) > 2 else ["--lmcode", "check"]
for command in commands:
    run_case(command, True)
    run_case(command, False)
    print(f"PASS {command} dry sentinel, marker, and real-mode effect")
