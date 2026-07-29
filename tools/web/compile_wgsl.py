#!/usr/bin/env python3
"""Compile the Vulkan GLSL sources in shaders/ to WGSL in assets/shaders/.

Pipeline: GLSL --(textual web variant)--> glslangValidator SPIR-V --> naga WGSL
--> post-processing for the SDL WebGPU backend's source reflection.

The web variant differs from the Vulkan GLSL in two mechanical ways:
  * Combined `sampler2D` uniforms are split into texture2D + sampler pairs.
    WebGPU has no combined image samplers, and the SDL WebGPU backend binds
    SDL sampler slot i as texture @binding(2i) / sampler @binding(2i+1).
  * `texture()` becomes `textureLod(..., 0.0)`: every engine texture is
    single-level, and explicit-LOD sampling is exempt from WGSL's
    uniform-control-flow analysis (mesh.frag samples inside varying branches).

Depth textures (sampled with the engine's nearest sampler) are tagged with a
`//!nofilter` comment, which the patched SDL backend reflects as an
unfilterable-float texture + non-filtering sampler -- required by WebGPU for
depth formats.

Requires glslangValidator (brew install glslang) and naga (cargo install naga-cli).
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SHADERS = ROOT / "shaders"
OUT = ROOT / "assets" / "shaders"

# Same list as shaders/compile.sh: <source> -> <output stem>.
SOURCES = {
    "mesh.vert": "mesh.vertex",
    "mesh.frag": "mesh.fragment",
    "blob.vert": "blob.vertex",
    "blob.frag": "blob.fragment",
    "overlay.vert": "overlay.vertex",
    "overlay.frag": "overlay.fragment",
    "text.vert": "text.vertex",
    "text.frag": "text.fragment",
    "particle.vert": "particle.vertex",
    "particle.frag": "particle.fragment",
    "dof.vert": "dof.vertex",
    "dof.frag": "dof.fragment",
    "skinned.vert": "skinned.vertex",
    "card.vert": "card.vertex",
    "card.frag": "card.fragment",
    "shadow.vert": "shadow.vertex",
    "shadow.frag": "shadow.fragment",
    "shadowskin.vert": "shadowskin.vertex",
    "mask.frag": "mask.fragment",
    "highlight.frag": "highlight.fragment",
}

# Samplers that the engine binds to depth textures (nearest sampler only).
NOFILTER = {
    "mesh.frag": {"shadow_map"},
    "dof.frag": {"depthTex"},
}

SAMPLER_DECL = re.compile(
    r"layout\(set\s*=\s*(\d+),\s*binding\s*=\s*(\d+)\)\s*uniform\s+sampler2D\s+(\w+)\s*;"
)


def to_web_glsl(src: str) -> tuple[str, list[str]]:
    """Split combined samplers and force explicit-LOD sampling."""
    names = []

    def split(m: re.Match) -> str:
        s, b, name = int(m.group(1)), int(m.group(2)), m.group(3)
        names.append(name)
        return (
            f"layout(set = {s}, binding = {2 * b}) uniform texture2D {name}_tx;\n"
            f"layout(set = {s}, binding = {2 * b + 1}) uniform sampler {name}_sp;\n"
            f"#define {name} sampler2D({name}_tx, {name}_sp)"
        )

    out = SAMPLER_DECL.sub(split, src)
    if names:
        # The define must sit after the #version directive.
        lines = out.split("\n")
        v = next(i for i, l in enumerate(lines) if l.startswith("#version"))
        lines.insert(v + 1, "#define texture(s, uv) textureLod(s, uv, 0.0)")
        out = "\n".join(lines)
    return out, names


ATTR_LINE = re.compile(r"(@group\(\d+\) @binding\(\d+\))\s*\n")


def post_process(wgsl: str, nofilter: set[str]) -> str:
    # naga writes "@group(G) @binding(B) \nvar ..."; the SDL backend reflects
    # bindings line-by-line and needs the attributes and `var` on one line.
    wgsl = ATTR_LINE.sub(r"\1 ", wgsl)
    lines = []
    for line in wgsl.split("\n"):
        decl = re.match(r"@group\(\d+\) @binding\(\d+\) var (\w+)_(tx|sp):", line)
        if decl and decl.group(1) in nofilter:
            line += " //!nofilter"
        lines.append(line)
    return "\n".join(lines)


def run(cmd: list[str]) -> None:
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"error: {' '.join(str(c) for c in cmd)}\n{r.stdout}{r.stderr}")


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        for src_name, out_stem in SOURCES.items():
            src = (SHADERS / src_name).read_text()
            web_glsl, names = to_web_glsl(src)
            glsl_path = tmp / src_name
            glsl_path.write_text(web_glsl)
            spv = tmp / (src_name + ".spv")
            wgsl_tmp = tmp / (src_name + ".wgsl")
            run(["glslangValidator", "-V", glsl_path, "-o", spv])
            # --keep-coordinate-space: naga otherwise negates clip-space Y to
            # translate raw-Vulkan NDC to WebGPU. SDL_GPU shaders already use
            # the Metal/D3D convention (SDL flips the viewport on Vulkan
            # instead), so the adjustment would mirror every pass vertically.
            run(["naga", "--keep-coordinate-space", spv, wgsl_tmp])
            wgsl = post_process(
                wgsl_tmp.read_text(), NOFILTER.get(src_name, set())
            )
            (OUT / f"{out_stem}.wgsl").write_text(wgsl)
            print(f"  {src_name} -> assets/shaders/{out_stem}.wgsl")
    print("WGSL shaders rebuilt.")


if __name__ == "__main__":
    main()
