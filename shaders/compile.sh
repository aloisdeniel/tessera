#!/usr/bin/env bash
# Compile the Vulkan GLSL sources in this dir to SPIR-V in assets/shaders/.
# The MSL shaders (Metal, macOS/iOS) are hand-authored in assets/shaders/;
# these GLSL sources are the portable equivalents for the Vulkan backend
# (Android/Linux). Requires glslangValidator (brew install glslang).
#
#   ./shaders/compile.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/../assets/shaders"
glsl="${GLSLANG:-glslangValidator}"

compile() { # <src> <stage-name>
  "$glsl" -V "$here/$1" -o "$out/$2.spv"
  echo "  $1 -> assets/shaders/$2.spv"
}

compile mesh.vert     mesh.vertex
compile mesh.frag     mesh.fragment
compile blob.vert     blob.vertex
compile blob.frag     blob.fragment
compile particle.vert particle.vertex
compile particle.frag particle.fragment
compile dof.vert      dof.vertex
compile dof.frag      dof.fragment
compile skinned.vert  skinned.vertex
echo "SPIR-V shaders rebuilt."
