#!/bin/zsh
# Build the sc6vrr proxy dinput8.dll with mingw-w64 and MinHook.
# --no-insert-timestamp makes rebuilds of the same source byte-identical, so
# tests/shader-capture.sh can recognize an installed build by comparing bytes.
set -euo pipefail
here=${0:A:h}
mh=$here/../third_party/minhook
mkdir -p $here/build
x86_64-w64-mingw32-gcc -O2 -Wall -shared -o $here/build/dinput8.dll \
  $here/src/sc6vrr.c $here/dinput8.def \
  $mh/src/buffer.c $mh/src/hook.c $mh/src/trampoline.c $mh/src/hde/hde64.c \
  -I$mh/include -lbcrypt -lws2_32 -static-libgcc -Wl,--enable-stdcall-fixup -Wl,--no-insert-timestamp
x86_64-w64-mingw32-objdump -p $here/build/dinput8.dll | sed -n '/Export Address Table/,/^$/p;/\[Ordinal\/Name Pointer\] Table/,/^$/p'
print "built: $here/build/dinput8.dll"
