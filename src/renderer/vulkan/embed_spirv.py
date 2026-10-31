"""Embed build-time SPIR-V as aligned words; no shader compiler ships at runtime."""
from pathlib import Path
import struct
import sys

source, dest, name = sys.argv[1:]
data = Path(source).read_bytes()
if len(data) % 4 or len(data) < 20 or data[:4] != b'\x03\x02\x23\x07':
    raise SystemExit(f"Invalid SPIR-V module: {source}")
words = struct.unpack(f"<{len(data) // 4}I", data)
lines = ["#pragma once", "#include <cstdint>", f"static constexpr uint32_t {name}[] = {{"]
for i in range(0, len(words), 8):
    lines.append("    " + ", ".join(f"0x{x:08x}u" for x in words[i:i + 8]) + ",")
lines.append("};\n")
Path(dest).write_text("\n".join(lines))
