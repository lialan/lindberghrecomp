"""Build source-owned fixed-function compatibility shaders into an offline archive."""
import importlib.util
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[2]
def module(name):
 spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(name+".py"))
 result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result
out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=True)
record=module("compile_shaders").compile_pair({"program":0,"vertex":"fixed.vert","fragment":"fixed.frag","attributes":[]},
 root/"src/renderer/vulkan/shaders",out,sys.argv[2],sys.argv[3])
module("pack_shaders").pack([record],out,out/"fixed.lvsp")
