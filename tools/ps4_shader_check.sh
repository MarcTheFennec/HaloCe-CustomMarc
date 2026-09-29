#!/bin/sh
# Validates the PS4 host's GLSL ES 3.00 -> 1.00 shader translation
# (port/ps4/host/host_shader.c) on shaders from the real generators
# (tools/ps4_shader_harness.c), with glslangValidator.
#
# usage: tools/ps4_shader_check.sh [count] [seed]
# needs: a native C compiler (CC, default cc) and glslangValidator on PATH
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
count=${1:-300}
seed=${2:-0x5eed}
out=${OUT:-$root/build/ps4/shader_check}
cc=${CC:-clang}

rm -rf "$out"
mkdir -p "$out/shaders"
cd "$root"
$cc ${HARNESS_LDFLAGS:--static} -O2 -g -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all -DHALO_ANDROID=1 -fms-extensions -fshort-wchar -fcommon -std=gnu11 -D_GNU_SOURCE \
	-DHALO_LINUX_PLATFORM_LAYER -w \
	-include port/linux/include/halo_linux_prefix.h \
	-include build/linux/platform_msvc_semantics.h \
	-Iport/linux/src -Iport/linux/include -Iport/android/guest/runtime -Iport/ps4/host \
	-Iport/third_party/tomlc17 -Isource -Isource/cseries \
	-Ibuild/ps4/third_party/khronos/OpenGL-Registry/api -Ibuild/ps4/third_party/khronos/EGL-Registry/api \
	-Ibuild/ps4/third_party/SDL3/include -idirafter port/include/xdk \
	tools/ps4_shader_harness.c port/linux/src/nv2a_vsh.c port/linux/src/nv2a_psh.c port/linux/src/xgpu_text.c \
	port/ps4/host/host_shader.c -o "$out/ps4_shader_harness"
"$out/ps4_shader_harness" "$out/shaders" "$count" "$seed"

# the random keys make some shaders the generators themselves get wrong
# (combinations the game never sets); only a translation that breaks a valid
# ES 3.00 shader counts as a failure
failed=0
checked=0
skipped=0
for es3 in "$out"/shaders/*.es3.*; do
	es2=$(echo "$es3" | sed 's/\.es3\./.es2./')
	if ! glslangValidator "$es3" > /dev/null 2>&1; then
		skipped=$((skipped + 1))
		continue
	fi
	checked=$((checked + 1))
	if ! glslangValidator "$es2" > "$out/last.log" 2>&1; then
		failed=$((failed + 1))
		if [ $failed -le 5 ]; then
			echo "FAIL $es2"
			sed -n '1,20p' "$out/last.log"
		fi
	fi
done
echo "GLSL ES 1.00: $checked valid ES 3.00 shaders translated, $failed failed ($skipped invalid originals skipped)"
[ $failed -eq 0 ]
