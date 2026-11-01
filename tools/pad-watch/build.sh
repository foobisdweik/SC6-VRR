#!/bin/zsh
# Build pad-watch, the macOS controller input logger (see pad-watch.swift).
set -euo pipefail
here=${0:A:h}
/usr/bin/swiftc -O -framework IOKit -framework GameController -o "$here/pad-watch" "$here/pad-watch.swift"
print "built: $here/pad-watch"
