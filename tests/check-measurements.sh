#!/bin/zsh
# Check the summaries and build the DLL. Add --wine to execute the x64 code test.
set -euo pipefail
repo=${0:A:h:h}
cd "$repo"
python3 -I -m unittest discover -s tests -p 'test_*.py' -v
zsh -n tests/run.sh tests/summarize.sh tests/mark.sh
dll/build.sh
clang -std=c11 -Wall -Wextra -Werror tests/test_durations.c -o dll/build/test-durations
dll/build/test-durations
x86_64-w64-mingw32-gcc -O2 -Wall -o dll/build/test-clock-cave.exe \
  tests/test_clock_cave.c third_party/minhook/src/buffer.c third_party/minhook/src/hook.c \
  third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde64.c \
  -Ithird_party/minhook/include -lbcrypt -static-libgcc
x86_64-w64-mingw32-gcc -O2 -Wall -o dll/build/test-shader-capture.exe \
  tests/test_shader_capture.c third_party/minhook/src/buffer.c third_party/minhook/src/hook.c \
  third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde64.c \
  -Ithird_party/minhook/include -lbcrypt -static-libgcc
if [[ ${1:-} == --wine ]]; then
  # Tests run in the CrossOver Preview x86-64 bottle (Rosetta 2). Its startup
  # list is empty, so starting it does not launch Steam.
  wine=${CROSSOVER_APP:-/Applications/CrossOver Preview.app}/Contents/SharedSupport/CrossOver/bin/wine
  "$wine" --bottle ${SC6_BOTTLE:-SC6-Rosetta} "$repo/dll/build/test-clock-cave.exe"
  "$wine" --bottle ${SC6_BOTTLE:-SC6-Rosetta} "$repo/dll/build/test-shader-capture.exe"
fi
