#!/bin/sh
# Sets up everything the PS4 port builds with, from public sources only, on
# an x86-64 Linux machine without root package installs:
#
#   $PREFIX/py        python venv: ziglang (clang 18 + lld), cmake, ninja,
#                     go-bin (a Go toolchain, for create-fself)
#   $PREFIX/bin       clang, ld.lld, llvm-ar/ranlib/objcopy wrappers,
#                     cmake, ninja, create-fself, glslangValidator
#   $PREFIX/oo/src    OpenOrbis-PS4-Toolchain (headers, link.x)
#   $PREFIX/oo/sysroot  OpenOrbis musl built for the PS4 (libc.a, crt1.o)
#   $PREFIX/oo/ps4libdoc  idc/ps4libdoc 5.05 (library/NID tables for the
#                     import stubs, tools/ps4_stub_libs.py)
#
# usage: tools/ps4_setup_toolchain.sh [prefix]     (default /opt)
# then:  export PATH=<prefix>/bin:$PATH OO_PS4_TOOLCHAIN=<prefix>/oo/src
#
# Everything is fetched from github.com, pypi.org and registry.npmjs.org.
# See port/ps4/BUILDING-PS4.md.
set -eu

PREFIX=${1:-/opt}
PY=$PREFIX/py
BIN=$PREFIX/bin
OO=$PREFIX/oo
mkdir -p "$BIN" "$OO"

if [ ! -x "$PY/bin/python" ]; then
	python3 -m venv "$PY"
fi
"$PY/bin/pip" install -q ziglang==0.13.0 cmake ninja go-bin
ZIG=$(echo "$PY"/lib/python3*/site-packages/ziglang)

cat > "$BIN/clang" <<EOF
#!/bin/sh
exec $ZIG/zig clang -resource-dir $ZIG/lib "\$@"
EOF
cat > "$BIN/ld.lld" <<EOF
#!/bin/sh
exec $ZIG/zig ld.lld "\$@"
EOF
for tool in ar ranlib objcopy; do
	printf '#!/bin/sh\nexec %s/zig %s "$@"\n' "$ZIG" "$tool" > "$BIN/llvm-$tool"
done
chmod +x "$BIN/clang" "$BIN/ld.lld" "$BIN"/llvm-*
ln -sf clang "$BIN/clang-18"
ln -sf "$PY/bin/cmake" "$BIN/cmake"
ln -sf "$PY/bin/ninja" "$BIN/ninja"
export PATH="$BIN:$PY/bin:$PATH"

clone() { # url branch directory
	if [ ! -d "$3/.git" ]; then
		git clone -q --depth 1 ${2:+-b "$2"} "$1" "$3"
	fi
}

clone https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain.git "" "$OO/src"
clone https://github.com/OpenOrbis/create-fself.git "" "$OO/create-fself"
clone https://github.com/OpenOrbis/musl.git "" "$OO/musl"
clone https://github.com/idc/ps4libdoc.git 5.05 "$OO/ps4libdoc"

# create-fself (Go, standard library only)
(cd "$OO/create-fself/cmd/create-fself" &&
	GOPROXY=off GOFLAGS=-mod=mod GOTOOLCHAIN=local CGO_ENABLED=0 GOPATH="$OO/gopath" \
	go build -modfile=go-linux.mod -o "$BIN/create-fself" .)

# the PS4's C library, as OpenOrbis builds it (build/build.sh there)
if [ ! -f "$OO/sysroot/lib/libc.a" ]; then
	(cd "$OO/musl" &&
		CC="clang --target=x86_64-pc-freebsd12-elf" AR=llvm-ar RANLIB=llvm-ranlib \
		./configure --target=x86_64-scei-ps4 --prefix="$OO/sysroot" --disable-shared \
			CFLAGS="-fPIC -DPS4 -D_LIBUNWIND_IS_BAREMETAL=1" > /dev/null &&
		make -j"$(nproc)" > /dev/null && make install > /dev/null)
fi

# glslangValidator (shader checks), from npm since there is no pypi build
if ! command -v glslangValidator > /dev/null 2>&1; then
	mkdir -p "$PREFIX/npm"
	(cd "$PREFIX/npm" && [ -f package.json ] || npm init -y > /dev/null)
	(cd "$PREFIX/npm" && npm install --silent glslang-validator-prebuilt-predownloaded)
	ln -sf "$PREFIX/npm/node_modules/glslang-validator-prebuilt-predownloaded/bin/glslangValidator.linux" \
		"$BIN/glslangValidator"
	chmod +x "$BIN/glslangValidator"
fi

echo "PS4 toolchain ready in $PREFIX:"
echo "  export PATH=$BIN:\$PATH OO_PS4_TOOLCHAIN=$OO/src"
