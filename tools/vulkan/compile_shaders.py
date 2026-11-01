"""Compile captured, explicitly linked guest shader pairs to Vulkan SPIR-V.

All work here is offline. Captured sources and resulting modules are game
data and must be written into ignored build directories, never committed.
The converter preserves guest uniform names in reflection metadata; the HLE
can address Vulkan UBO offsets and cached descriptors without a GL driver.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import subprocess

VERSION = "lindbergh-vulkan-glsl-1"
GENERATOR_HASH = hashlib.sha256(Path(__file__).read_bytes() + Path(__file__).with_name("legacy.glsl").read_bytes()).digest()
DECL = re.compile(r"\b(attribute|varying)\s+(\w+)\s+(\w+)\s*(\[\s*(\d+)\s*\])?\s*;")
SAMPLER = re.compile(r"\buniform\s+(sampler\w+)\s+(\w+)\s*(\[[^]]+\])?\s*;")
BUILTIN_INPUTS = {
    "gl_Vertex": ("vec4", 0), "gl_Normal": ("vec3", 2),
    "gl_Color": ("vec4", 3), "gl_SecondaryColor": ("vec4", 4),
    **{f"gl_MultiTexCoord{i}": ("vec4", 8 + i) for i in range(8)},
}
STATE = {
    "gl_ModelViewMatrix": "legacy.modelview", "gl_ProjectionMatrix": "legacy.projection",
    "gl_ModelViewProjectionMatrix": "(legacy.projection * legacy.modelview)",
    "gl_TextureMatrix": "legacy.texture_matrix", "gl_NormalMatrix": "legacy.normal_matrix",
    "gl_NormalScale": "legacy.normal_scale", "gl_FrontMaterial": "legacy.front_material",
    "gl_LightSource": "legacy.light_source", "gl_Fog": "legacy.fog",
    "gl_FrontLightModelProduct": "legacy.front_light_model", "gl_DepthRange": "legacy.depth_range",
}


def run(command, cwd=None):
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(" ".join(map(str, command)) + f" (exit {result.returncode})\n" + result.stdout + result.stderr)
    return result.stdout


def preprocess(path, stage, validator):
    source = run([validator, "-E", "-S", stage, str(path)])
    return re.sub(r"^\s*#(?:version|extension|line)\b[^\n]*", "", source, flags=re.M)


def declarations(source, kind):
    return {m[2]: (m[1], int(m[4] or 1), bool(m[3])) for m in DECL.findall(source) if m[0] == kind}


def varyings(vertex, fragment):
    # One linked pair gets one shared, compact interface. Assigning locations
    # independently to the two stages would silently connect different values.
    all_vars = declarations(vertex, "varying")
    for name, typ in declarations(fragment, "varying").items():
        if name in all_vars and all_vars[name] != typ:
            raise ValueError(f"Varying type mismatch: {name}")
        all_vars[name] = typ
    location, result = 4, {}
    for name, (typ, count, array) in sorted(all_vars.items()):
        columns = int(typ[-1]) if typ.startswith("mat") and typ[-1].isdigit() else 1
        result[name] = (location, typ, count, array)
        location += columns * count
    return result


def convert(source, stage, interface, attributes, samplers):
    prefix = ["#version 450", "#extension GL_EXT_multiview : require"]
    # Keep legacy color outputs at fixed locations, including front/back color
    # selection in fragment shaders. Other varyings follow the pair's layout.
    colors = ["front_color", "back_color", "front_secondary", "back_secondary"]
    for i, name in enumerate(colors):
        prefix.append(f"layout(location={i}) {'out' if stage == 'vert' else 'in'} vec4 legacy_{name};")
    if stage == "frag":
        prefix.append("layout(location=0) out vec4 legacy_frag_color;")

    def decl(m):
        kind, typ, name, arr, count = m.groups()
        if kind == "varying":
            loc = interface[name][0]
            return f"layout(location={loc}) {'out' if stage == 'vert' else 'in'} {typ} {name}{arr or ''};"
        if stage != "vert":
            raise ValueError("Vertex attribute declared in fragment shader")
        if name not in attributes:
            raise ValueError(f"Missing explicit guest attribute binding: {name}")
        return f"layout(location={attributes[name]}) in {typ} {name}{arr or ''};"

    source = DECL.sub(decl, source)
    source = SAMPLER.sub(lambda m: f"layout(set=2, binding={samplers[m[2]]}) uniform {m[1]} {m[2]}{m[3] or ''};", source)
    replacements = dict(STATE)
    if stage == "vert":
        for name, (typ, location) in BUILTIN_INPUTS.items():
            if re.search(r"\b" + name + r"\b", source) or (name == "gl_Vertex" and re.search(r"\bftransform\b", source)):
                replacement = "legacy_input_" + name[3:]
                prefix.append(f"layout(location={location}) in {typ} {replacement};")
                replacements[name] = replacement
        replacements.update({"gl_FrontColor": "legacy_front_color", "gl_BackColor": "legacy_back_color",
                             "gl_FrontSecondaryColor": "legacy_front_secondary", "gl_BackSecondaryColor": "legacy_back_secondary"})
    else:
        replacements.update({"gl_Color": "(gl_FrontFacing ? legacy_front_color : legacy_back_color)",
                             "gl_SecondaryColor": "(gl_FrontFacing ? legacy_front_secondary : legacy_back_secondary)",
                             "gl_FragColor": "legacy_frag_color", "gl_FragCoord": "legacy_frag_coord()"})
    replacements.update({"texture2D": "texture", "textureCube": "texture", "texture3D": "texture",
                         "texture2DProj": "textureProj", "texture2DLod": "textureLod",
                         "ftransform": "legacy_ftransform", "shadow2D": "legacy_shadow2D", "shadow2DProj": "legacy_shadow2DProj"})
    source = re.sub(r"\b(?:" + "|".join(re.escape(k) for k in replacements) + r")\b",
                    lambda m: replacements[m[0]], source)
    legacy = Path(__file__).with_name("legacy.glsl").read_text()
    # gl_FragCoord is fragment-stage only, even when a helper is unused.
    if stage == "vert":
        legacy = legacy[:legacy.index("vec4 legacy_frag_coord()")]
        if "legacy_ftransform" in source:
            legacy += "vec4 legacy_ftransform() { return legacy.projection * legacy.modelview * legacy_input_Vertex; }\n"
        prefix.append("layout(push_constant) uniform EyeTransforms { mat4 reprojection[2]; } eyes;")
        source, count = re.subn(r"\bvoid\s+main\s*\(\s*(?:void\s*)?\)", "void guest_main()", source)
        if count != 1:
            raise ValueError("Expected exactly one vertex main function")
        source += """
void main() {
    legacy_front_color = vec4(0); legacy_back_color = vec4(0);
    legacy_front_secondary = vec4(0); legacy_back_secondary = vec4(0);
    guest_main();
    vec4 p = eyes.reprojection[gl_ViewIndex] * gl_Position;
    gl_Position = vec4(p.x, -p.y, (p.z + p.w) * 0.5, p.w);
}
"""
    if stage == "frag":
        source, count = re.subn(r"\bvoid\s+main\s*\(\s*(?:void\s*)?\)", "void guest_main()", source)
        if count != 1: raise ValueError("Expected exactly one fragment main function")
        source += """
layout(constant_id=1) const int legacy_alpha_func=0;
void main() {
    guest_main();
    float a=legacy_frag_color.a, r=legacy.alpha_ref;
    bool keep=legacy_alpha_func==0 || legacy_alpha_func==8 ||
        (legacy_alpha_func==2 && a<r) || (legacy_alpha_func==3 && a==r) ||
        (legacy_alpha_func==4 && a<=r) || (legacy_alpha_func==5 && a>r) ||
        (legacy_alpha_func==6 && a!=r) || (legacy_alpha_func==7 && a>=r);
    if(!keep) discard;
}
"""
    return "\n".join(prefix) + "\n" + legacy + "\n" + source


def program_key(row, root):
    raw = [(root / row[k]).read_bytes() for k in ("vertex", "fragment")]
    attributes = sorted(({"name": a["name"], "location": a["location"]}
                         for a in row.get("attributes", []) if not a["name"].startswith("gl_")),
                        key=lambda a: a["name"])
    return hashlib.sha256(GENERATOR_HASH + raw[0] + b"\0" + raw[1] +
                          json.dumps(attributes, sort_keys=True).encode()).hexdigest()


def validate_uniform_layouts(reflection):
    # Optimizers can remove unused members independently in each stage while
    # preserving explicit offsets. Compare shared leaves, not whole type lists.
    shared = {}
    def walk(stage, type_id, offset, name, arrays=(), stride=0, matrix_stride=0, row_major=False):
        typ = reflection[stage].get("types", {}).get(type_id)
        if typ is None:
            return {name: (offset, type_id, tuple(arrays), stride, matrix_stride, row_major)}
        result = {}
        for i in range(arrays[0] if arrays else 1):
            prefix = f"{name}[{i}]" if arrays else name
            for m in typ.get("members", []):
                child = f"{prefix}.{m['name']}" if prefix else m["name"]
                result.update(walk(stage,m["type"],offset+i*stride+m["offset"],child,
                                   m.get("array",()),m.get("array_stride",0),m.get("matrix_stride",0),m.get("row_major",False)))
        return result
    for stage in ("vert", "frag"):
        for block in reflection[stage].get("ubos", []):
            key = block["set"], block["binding"]
            fields=walk(stage,block["type"],0,"")
            previous=shared.setdefault(key,{})
            for name, layout in fields.items():
                if name in previous and previous[name] != layout:
                    raise ValueError(f"Vertex/fragment uniform layout mismatch at {key}: {name}")
                previous[name]=layout


def compile_pair(row, root, output, validator, reflector):
    paths = [root / row[k] for k in ("vertex", "fragment")]
    raw = [p.read_bytes() for p in paths]
    key = program_key(row, root)
    dest = output / key
    dest.mkdir(parents=True, exist_ok=True)
    sources = [preprocess(p, stage, validator) for p, stage in zip(paths, ("vert", "frag"))]
    interface = varyings(*sources)
    attributes = {a["name"]: a["location"] for a in row.get("attributes", []) if not a["name"].startswith("gl_")}
    bound_attributes = dict(attributes)
    # Inactive declarations are not returned by GL_ACTIVE_ATTRIBUTES. Give
    # those declarations collision-free temporary locations, then require the
    # optimizer/reflection to prove that they really are inactive. Never guess
    # the guest binding for a live attribute.
    occupied = set(attributes.values())
    for name, (_, location) in BUILTIN_INPUTS.items():
        if re.search(r"\b" + name + r"\b", sources[0]) or (name == "gl_Vertex" and "ftransform" in sources[0]):
            occupied.add(location)
    for name in sorted(declarations(sources[0], "attribute")):
        if name not in attributes:
            free = next(i for i in range(32) if i not in occupied)
            attributes[name] = free
            occupied.add(free)
    sampler_names = sorted({m[1] for s in sources for m in SAMPLER.findall(s)})
    samplers = {name: i for i, name in enumerate(sampler_names)}
    for source, stage in zip(sources, ("vert", "frag")):
        (dest / ("guest." + stage)).write_text(convert(source, stage, interface, attributes, samplers))
    # Linked compilation establishes the same non-opaque uniform block across
    # both stages; no per-stage auto-location assumptions are used for varyings.
    run([validator, "-V", "-R", "-Os", "--target-env", "vulkan1.1", "--auto-map-bindings",
         "--set-default-uniform-block", "GuestUniforms", "1", "0", "-l", "guest.vert", "guest.frag"], cwd=dest)
    reflection = {}
    for stage in ("vert", "frag"):
        run(["spirv-val", "--target-env", "vulkan1.1", str(dest / (stage + ".spv"))])
        reflection[stage] = json.loads(run([reflector, str(dest / (stage + ".spv")), "--reflect"]))
    for item in reflection["vert"].get("inputs", []):
        name = item["name"]
        if not name.startswith("legacy_input_") and name not in bound_attributes:
            raise ValueError(f"Missing explicit guest binding for live vertex attribute: {name}")
    validate_uniform_layouts(reflection)
    meta = {"schema": VERSION, "key": key, "program": row.get("program"),
            "spirv_sha256": {stage: hashlib.sha256((dest / (stage + ".spv")).read_bytes()).hexdigest()
                             for stage in ("vert", "frag")},
            "sources": {stage: hashlib.sha256(data).hexdigest() for stage, data in zip(("vert", "frag"), raw)},
            "lookup_sources": {stage: shader_source_digest((root / row.get("original_" + kind, row[kind])).read_bytes())
                               for stage, kind in (("vert", "vertex"), ("frag", "fragment"))},
            "attributes": bound_attributes, "samplers": samplers, "reflection": reflection}
    (dest / "program.json").write_text(json.dumps(meta, indent=2) + "\n")
    return meta


def shader_source_digest(source):
    # Match runtime shader identity; preserve raw source/module integrity hashes.
    source = source.replace(b"\r", b" ")
    if source and not source.endswith(b"\n"):
        source += b"\n"
    return hashlib.sha256(source).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--limit", type=int)
    parser.add_argument("--validator", default="glslangValidator")
    parser.add_argument("--reflector", default="spirv-cross")
    args = parser.parse_args()
    rows = [json.loads(line) for line in args.manifest.read_text().splitlines() if line.strip()]
    # Later successful relinks replace earlier captures of the same program.
    rows = list({row["program"]: row for row in rows}.values())
    if args.limit:
        rows = rows[:args.limit]
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    root = args.manifest.resolve().parent
    # Different GL program objects can contain the same linked sources. Compile
    # each content key once, or parallel compiler processes would overwrite one
    # another's SPIR-V while reflection/validation is reading it.
    unique = {}
    for row in rows:
        unique.setdefault(program_key(row, root), row)
    results, failures = {}, []
    def one(row):
        try:
            return compile_pair(row, root, args.output, args.validator, args.reflector), None
        except (RuntimeError, ValueError) as e:
            return None, {"program": row["program"], "error": str(e)}
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for result, error in pool.map(one, unique.values()):
            if error: failures.append(error)
            else: results[result["key"]] = result
    programs = []
    for row in rows:
        key = program_key(row, root)
        if key in results:
            programs.append({"key": key, "program": row["program"], "sources": results[key]["sources"]})
    (args.output / "index.json").write_text(json.dumps({"schema": VERSION, "programs": programs, "failures": failures}, indent=2) + "\n")
    print(f"Vulkan shaders: {len(programs)} program links ({len(results)} unique pairs) compiled, {len(failures)} failed")
    for e in failures[:3]: print(e["error"][:2500])
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
