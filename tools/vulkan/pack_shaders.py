"""Package validated SPIR-V and reflection into a runtime-only Vulkan archive.

No game shaders are committed. The archive is a local build/package artifact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

MAGIC = b"LVSPV001"


def uniform_fields(reflect, block):
    fields = []
    def leaf(type_name, offset, name, count=1, array_stride=0, matrix_stride=0):
        if type_name in reflect["types"]:
            if count != 1:
                for i in range(count):
                    leaf(type_name, offset + i * array_stride, f"{name}[{i}]")
            else:
                for member in reflect["types"][type_name]["members"]:
                    if member.get("row_major"):
                        raise ValueError("Row-major uniform storage requires an explicit pack layout")
                    arrays = member.get("array", [])
                    if len(arrays) > 1 or any(n < 1 for n in arrays):
                        raise ValueError("Unsupported multidimensional/runtime-sized uniform array")
                    leaf(member["type"], offset + member["offset"],
                         f"{name}.{member['name']}" if name else member["name"],
                         arrays[0] if arrays else 1, member.get("array_stride", 0), member.get("matrix_stride", 0))
            return
        scalar, columns, rows = 0, 1, 1
        matrix = re.fullmatch(r"mat([2-4])(?:x([2-4]))?", type_name)
        vector = re.fullmatch(r"([iub]?)vec([2-4])", type_name)
        if matrix:
            columns, rows = int(matrix[1]), int(matrix[2] or matrix[1])
        elif vector:
            scalar, rows = {"": 0, "i": 1, "u": 2, "b": 1}[vector[1]], int(vector[2])
        elif type_name in ("float", "int", "uint", "bool"):
            scalar = {"float": 0, "int": 1, "uint": 2, "bool": 1}[type_name]
        else:
            raise ValueError(f"Unsupported uniform scalar type {type_name}")
        end = offset + (count - 1) * array_stride + (columns - 1) * matrix_stride + rows * 4
        if end > block["block_size"]:
            raise ValueError(f"Uniform field {name} escapes reflected block")
        fields.append((name, offset, scalar, columns, rows, count, array_stride, matrix_stride))
    leaf(block["type"], 0, "")
    return fields


def pack(programs, root, output):
    data = bytearray(MAGIC)
    def u32(value): data.extend(struct.pack("<I", value))
    def string(value):
        encoded = value.encode("utf-8")
        u32(len(encoded)); data.extend(encoded)
    unique = sorted({p["key"] for p in programs})
    u32(1); u32(len(unique))
    for key in unique:
        meta = json.loads((root / key / "program.json").read_text())
        data.extend(bytes.fromhex(key))
        for stage in ("vert", "frag"):
            data.extend(bytes.fromhex(meta.get("lookup_sources", meta["sources"])[stage]))
            spv = (root / key / (stage + ".spv")).read_bytes()
            if len(spv) % 4 or len(spv) < 20 or struct.unpack_from("<I", spv)[0] != 0x07230203:
                raise ValueError(f"Invalid SPIR-V module: {key}/{stage}")
            if hashlib.sha256(spv).hexdigest() != meta["spirv_sha256"][stage]:
                raise ValueError(f"SPIR-V module changed after validation: {key}/{stage}")
            u32(len(spv) // 4); data.extend(spv)
        attrs = sorted(meta["attributes"].items())
        u32(len(attrs))
        for name, location in attrs: string(name); u32(location)
        inputs = meta["reflection"]["vert"].get("inputs", [])
        u32(len(inputs))
        for value in sorted(inputs, key=lambda v: v["location"]):
            u32(value["location"]); string(value["type"])
        blocks, samplers = {}, {}
        for stage in ("vert", "frag"):
            reflect = meta["reflection"][stage]
            for block in reflect.get("ubos", []):
                block_key = block["set"], block["binding"]
                fields = uniform_fields(reflect, block)
                size, combined = blocks.get(block_key,(0,{}))
                for field in fields:
                    name=field[0]
                    if name in combined and combined[name]!=field:
                        raise ValueError(f"Uniform layout mismatch: {key}/{block_key}/{name}")
                    combined[name]=field
                blocks[block_key]=(max(size,block["block_size"]),combined)
            for sampler in reflect.get("textures", []):
                if sampler["set"] != 2:
                    raise ValueError("Unexpected sampler descriptor set")
                dims = sampler.get("array", [])
                if len(dims) > 1: raise ValueError("Multidimensional sampler array")
                samplers[sampler["binding"]] = sampler["name"], sampler["type"], dims[0] if dims else 1
        u32(len(blocks))
        for (set_number, binding), (size, fields) in sorted(blocks.items()):
            u32(set_number); u32(binding); u32(size); u32(len(fields))
            for name, *values in sorted(fields.values(), key=lambda f: (f[1],f[0])):
                string(name)
                for v in values: u32(v)
        u32(len(samplers))
        for binding, (name, typ, count) in sorted(samplers.items()):
            u32(binding); string(name); string(typ); u32(count)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.write_bytes(data)
    temporary.replace(output)
    return len(unique), len(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    index = json.loads((args.directory / "index.json").read_text())
    if index.get("failures"):
        raise ValueError("Cannot package a shader conversion with failures")
    count, size = pack(index["programs"], args.directory, args.output)
    print(f"Vulkan archive: {count} unique programs, {size} bytes")


if __name__ == "__main__": main()
