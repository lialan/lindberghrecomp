"""Generate typed direct Vulkan guest entry declarations and fail-fast gaps.

GL headers describe the guest ABI only. No entry resolves a host GL driver.
Unimplemented entry points abort by name, never silently drop rendering.
"""
import importlib.util
from pathlib import Path
import re
import sys
spec = importlib.util.spec_from_file_location("thunks", Path(__file__).parents[1] / "gen_gl_thunks.py")
thunks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(thunks)
output, implementation, headers, *sources = sys.argv[1:]
headers = headers.split(";")
signatures = thunks.load(headers)
implementation_text = Path(implementation).read_text()
# Internal forwarding calls need the same remapping even when a small guest
# (such as the runtime self-test) does not import their destination entry.
required = set(re.findall(r"\b(gl[A-Z]\w*)\s*\(", implementation_text))
for path in sources:
    required.update(re.findall(r"\b(gl[A-Z]\w*)\s*\(", Path(path).read_text()))
required = sorted(n for n in required if not n.startswith("glX") and n in signatures)
implemented = set(re.findall(r"\blvk_(gl[A-Z]\w*)\s*\(", implementation_text))
out = Path(output)
out.mkdir(parents=True, exist_ok=True)
h = ["/* Generated guest ABI declarations; no host OpenGL calls. */", "#pragma once"]
h += [f'#include "{p}"' for p in headers]
h += ['#ifdef __cplusplus', 'extern "C" {', '#endif']
stubs = ['#include "vulkan_abi.h"', '#include <stdio.h>', '#include <stdlib.h>',
         'static void missing(const char *name) { fprintf(stderr, "[vulkan] unimplemented guest operation: %s\\n", name); abort(); }']
for name in required:
    if name not in signatures: raise ValueError(f"Missing guest ABI prototype: {name}")
    ret, args = signatures[name]
    types = thunks.split_args(args)
    params = ", ".join(f"{t} a{i}" for i, t in enumerate(types)) or "void"
    h.append(f"{ret} lvk_{name}({params});")
    if name not in implemented:
        result = "" if ret == "void" else " return {};"
        stubs.append(f'extern "C" {ret} lvk_{name}({params}) {{ missing("{name}");{result} }}')
h += ['#ifdef __cplusplus', '}', '#endif']
h += [f"#define {name} lvk_{name}" for name in required]
def write(path, text):
    if not path.exists() or path.read_text() != text: path.write_text(text)
write(out / "vulkan_abi.h", "\n".join(h) + "\n")
write(out / "vulkan_missing.cpp", "\n".join(stubs) + "\n")
# Desktop compatibility for disabled stereo hooks. VR is supplied by OpenXR,
# never emulated by the old host GL stereo/replay implementation.
stereo = Path(sources[0]).parent / "stereo.h"
text = re.sub(r"/\*.*?\*/", "", stereo.read_text(), flags=re.S)
bridge = ['#include "stereo.h"', '#include <stdlib.h>', '#include <stdio.h>',
          'uint32_t stereo_state_lo = UINT32_MAX, stereo_state_hi = 0;', 'int stereo_glprof_on(void) { return 0; }']
for ret, name, args in re.findall(r"\b(void|int|unsigned)\s+(stereo_\w+)\s*\((.*?)\);", text, re.S):
    if name in ("stereo_start", "stereo_start_wide"):
        body = 'fprintf(stderr, "[vulkan] legacy stereo replay is unavailable; use the Vulkan OpenXR host\\n"); abort();'
    else: body = "" if ret == "void" else "return 0;"
    bridge.append(f"{ret} {name}({args}) {{ {body} }}")
write(out / "vulkan_desktop_stereo.c", "\n".join(bridge) + "\n")
print(f"Vulkan guest ABI: {len(required)} typed entries, {len(set(required)-implemented)} explicit gaps")
