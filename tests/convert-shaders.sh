#!/bin/zsh
# Convert captured SC6 shaders to Metal (notes/native-metal.md, step 3, path 1).
#
# Usage: tests/convert-shaders.sh            convert every captured shader
#        tests/convert-shaders.sh summary    report the last results
#
# For each DXBC file it runs two stages and records each result:
#   1. DXMT's airconv converts DXBC to AIR (Apple's LLVM-based shader format).
#   2. The Metal Toolchain's `metallib` links the AIR into a metal library.
# A later Metal runtime check (tests/load-metallibs.swift) loads each library.
# Geometry shaders are not converted alone: airconv converts them only together
# with their vertex shader, and alone it aborts, which opens a macOS crash dialog
# each time. Instead, tests/gs_variants.py converts each vertex and geometry
# shader combination that the game drew, with its input layout, as DXMT does.
# Inputs and outputs are game material, so they stay in the private directory.
set -euo pipefail

here=${0:A:h}
bottle=${SC6_BOTTLE:-SC6-Rosetta}
airconv=${AIRCONV:-$HOME/Projects/dxmt-SC6/build/src/airconv/darwin/airconv}
src="$HOME/Library/Application Support/SC6-VRR/native-investigation/shaders/$bottle"
out=$src/convert-airconv
jobs=${JOBS:-8}

convert_one() {
  local dxbc=$1 out=$2 airconv=$3 name=${1:t:r}
  local air=$out/air/$name.air lib=$out/metallib/$name.metallib log=$out/logs/$name.txt
  local stage1=fail stage2=skip
  if "$airconv" "$dxbc" -o "$air" > "$log" 2>&1 && [[ -s $air ]]; then
    stage1=ok
    if xcrun -sdk macosx metallib "$air" -o "$lib" >> "$log" 2>&1 && [[ -s $lib ]]; then
      stage2=ok
    else
      stage2=fail
    fi
  fi
  print -r -- "$name	$stage1	$stage2"
}

if [[ ${1:-} != summary ]]; then
  [[ -x $airconv ]] || { print "airconv not found: $airconv"; exit 1; }
  rm -rf $out
  mkdir -p $out/air $out/metallib $out/logs
  print "airconv: $airconv"
  print "metal: $(xcrun -sdk macosx metal --version 2>&1 | head -1)"
  # Each worker writes one result line; xargs runs $jobs workers at a time.
  functions -x convert_one 2>/dev/null || true
  # The index files name each shader's stage; the DLL writes Windows line endings.
  cat $src/index-*.tsv | tr -d '\r' | awk -F'\t' '$3 == "gs" || $3 == "gs_so" { print $6 }' | sort -u > $out/gs.txt
  for name in $(cat $out/gs.txt); do print -r -- "$name	skip	skip"; done > $out/results.tsv
  find $src/dxbc -name '*.dxbc' | grep -v -F -f $out/gs.txt | tr '\n' '\0' |
    xargs -0 -n 1 -P $jobs zsh -c "$(functions convert_one); convert_one \"\$1\" \"$out\" \"$airconv\"" _ \
    >> $out/results.tsv

  python3 -I $here/gs_variants.py $src $airconv
fi

python3 -I - "$src" "$out" <<'PY'
import collections, csv, pathlib, sys
src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
stage = {}
for path in sorted(src.glob('index-*.tsv')):
    with path.open(newline='') as f:
        for row in csv.DictReader(f, delimiter='\t'):
            stage.setdefault(row['sha256'], row['stage'])
rows = [line.split('\t') for line in (out / 'results.tsv').read_text().splitlines() if line]
total = collections.Counter()
ok1 = collections.Counter()
ok2 = collections.Counter()
for name, s1, s2 in rows:
    st = stage.get(name, '?')
    total[st] += 1
    ok1[st] += s1 == 'ok'
    ok2[st] += s2 == 'ok'
print(f'{len(rows)} shaders')
print(f'{"stage":6} {"total":>6} {"to AIR":>8} {"linked":>8}')
for st in sorted(total):
    print(f'{st:6} {total[st]:6} {ok1[st]:8} {ok2[st]:8}')
print(f'{"all":6} {sum(total.values()):6} {sum(ok1.values()):8} {sum(ok2.values()):8}')
print(f'skipped (geometry shaders, need their vertex shader): {sum(1 for r in rows if r[1] == "skip")}')
pairs = [l.split('\t') for l in (out / 'gs-variants.tsv').read_text().splitlines() if l] if (out / 'gs-variants.tsv').exists() else []
if pairs:
    print(f'vertex+geometry variants: {len(pairs)}, vertex half linked {sum(p[1] == "ok" for p in pairs)}, '
          f'geometry half linked {sum(p[2] == "ok" for p in pairs)}')
failed = [name for name, s1, s2 in rows if s1 not in ('ok', 'skip') or s2 not in ('ok', 'skip')]
if failed:
    print('First failures (logs in convert-airconv/logs):')
    for name in failed[:10]:
        first = (out / 'logs' / f'{name}.txt').read_text(errors='replace').strip().splitlines()[:1]
        print(f'  {stage.get(name, "?")} {name[:16]}  {first[0] if first else "(no output)"}')
PY
