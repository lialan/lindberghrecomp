"""Build a source-owned legacy shader fixture through the production converter."""
import importlib.util
import json
from pathlib import Path
import shutil
import sys

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("shader_converter", root / "tools/vulkan/compile_shaders.py")
converter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(converter)
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=True)
# Model an offline compatibility repair: compile the repaired source while
# retaining the original guest source identity for runtime lookup.
patched = output / "repaired.vert"
patched.write_text((Path(__file__).parent / "legacy.vert").read_text().replace("void main() {", "void main() { float unused_repair_value = 0.0;"))
result = converter.compile_pair({"program": 1, "vertex": str(patched), "fragment": "legacy.frag",
                                 "original_vertex": "legacy.vert", "attributes": []},
                                Path(__file__).parent, output, sys.argv[2], sys.argv[3])
for stage in ("vert", "frag"):
    shutil.copy2(output / result["key"] / (stage + ".spv"), output / (stage + ".spv"))
# Cross-stage descriptor layouts must agree, including scalar/array offsets.
blocks = {}
for stage in ("vert", "frag"):
    reflect = result["reflection"][stage]
    for block in reflect.get("ubos", []):
        key = block["set"], block["binding"]
        members = reflect["types"][block["type"]]["members"]
        layout = [(m["name"], m.get("offset"), m.get("array_stride"), m.get("matrix_stride")) for m in members]
        if key in blocks:
            assert blocks[key] == (block["block_size"], layout)
        blocks[key] = block["block_size"], layout
(output / "program.json").write_text(json.dumps(result, indent=2) + "\n")

assert blocks[(1, 0)][0] == 16, "guestTint uniform layout changed"

spec = importlib.util.spec_from_file_location("shader_packer", root / "tools/vulkan/pack_shaders.py")
packer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packer)
packer.pack([result], output, output / "fixture.lvsp")
