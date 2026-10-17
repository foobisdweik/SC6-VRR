#!/bin/zsh
# Record every shader SC6 creates (notes/native-metal.md, step 1).
#
# Usage: tests/shader-capture.sh install   copy the sc6vrr DLL into the game folder
#        tests/shader-capture.sh launch    start SC6 with capture on
#        tests/shader-capture.sh launch-fast  the same, with the frame rate and
#                                          the battle speed uncapped
#        tests/shader-capture.sh summary   count the recorded shaders
#        tests/shader-capture.sh remove    delete the DLL from the game folder
#
# Runs in the CrossOver Preview x86-64 bottle (Rosetta 2), the only test bottle
# since 2026-10-16. SC6_BOTTLE selects another. The DLL changes no frame timing:
# SC6VRR_FRAME_MODS is set only by launch-fast. Output goes to the private
# investigation directory, outside Git, because it is game material.
set -euo pipefail

here=${0:A:h}
repo=${here:h}
wine=${CROSSOVER_APP:-/Applications/CrossOver Preview.app}/Contents/SharedSupport/CrossOver/bin/wine
bottle=${SC6_BOTTLE:-SC6-Rosetta}
win64="$HOME/Library/Application Support/CrossOver/Bottles/$bottle/drive_c/Games/SoulCalibur 6/SoulcaliburVI/Binaries/Win64"
exe=$win64/SoulcaliburVI.exe
dll=$win64/dinput8.dll
# One folder per bottle, so that FEX and Rosetta runs can be compared.
out="$HOME/Library/Application Support/SC6-VRR/native-investigation/shaders/$bottle"
to_windows() { print -r -- "Z:${1//\//\\}"; }

game_running() { pgrep -f -q 'SoulcaliburVI'; }
# Any build of this DLL contains this variable name; other DLLs (for example,
# the Codex recorder) do not.
is_sc6vrr() { [[ -f $1 ]] && grep -q -a SC6VRR_SHADER_DIR "$1"; }

case ${1:-} in
  install)
    game_running && { print "SC6 is running. Quit it first."; exit 1; }
    [[ -f $dll ]] && ! is_sc6vrr $dll && {
      print "A different dinput8.dll is installed (for example, the Codex recorder). Remove it first:"
      print "  $dll"; exit 1; }
    $repo/dll/build.sh >/dev/null
    cp $repo/dll/build/dinput8.dll $dll
    print "installed: $dll"
    print "sha256: $(shasum -a 256 $dll | cut -d' ' -f1)"
    ;;
  launch|launch-fast)
    game_running && { print "SC6 is already running."; exit 1; }
    cmp -s $dll $repo/dll/build/dinput8.dll || { print "Run '$0 install' first."; exit 1; }
    mkdir -p $out
    stamp=$(date +%Y%m%d-%H%M%S)
    env_arg="SC6VRR_SHADER_DIR=\"$(to_windows $out)\" SC6VRR_LOG=\"$(to_windows $out/sc6vrr-$stamp.log)\""
    # launch-fast removes the 60 fps cap without the 60 Hz battle gate. The
    # battle advances one step per rendered frame, so it runs faster than
    # normal, limited by how fast the game renders (about 80 fps measured).
    [[ $1 == launch-fast ]] && env_arg+=" SC6VRR_FRAME_MODS=1 SC6VRR_UNCAP=1 SC6VRR_MAX_FPS=240"
    env_arg=${env_arg//\\/\\\\}
    "$wine" --bottle $bottle --env "$env_arg" --workdir "${exe:h}" --start "$exe" > $out/wine-$stamp.log 2>&1 &
    print "Launched in $bottle. Log: $out/sc6vrr-$stamp.log"
    print "Visit the scenes to record: menus, Training on several stages, and several characters."
    ;;
  summary)
    python3 -I - $out <<'PY'
import collections, csv, pathlib, sys
out = pathlib.Path(sys.argv[1])
indexes = sorted(out.glob('index-*.tsv'))
if not indexes:
    sys.exit('No index files yet.')
calls, failed, unique, layouts, blends = 0, 0, {}, set(), set()
for path in indexes:
    with path.open(newline='') as f:
        for row in csv.DictReader(f, delimiter='\t'):
            if row['stage'] == 'il':
                layouts.add(row['sha256'])
                continue
            if row['stage'] == 'bs':
                blends.add(row['sha256'])
                continue
            calls += 1
            failed += int(row['hresult'], 16) >= 0x80000000
            unique.setdefault(row['sha256'], (row['stage'], row['model']))
models = collections.Counter(unique.values())
files = list((out / 'dxbc').glob('*.dxbc'))
print(f'{len(indexes)} runs, {calls} shader create calls, {failed} failed calls')
print(f'{len(unique)} unique shaders in the index, {len(files)} files in dxbc/')
print(f'{sum(p.stat().st_size for p in files) / 1e6:.1f} MB of bytecode')
for (stage, model), n in sorted(models.items()):
    print(f'  {stage:6} {model:10} {n}')
print(f'{len(layouts)} unique input layouts, {len(blends)} unique blend states')

combos, pairs = set(), set()
for path in sorted(out.glob('bindings-*.tsv')):
    with path.open(newline='') as f:
        for row in csv.DictReader(f, delimiter='\t'):
            combos.add(tuple(v for k, v in row.items() if k not in ('seconds', 'thread')))
            if row['vs'] not in ('none', 'unknown') and row['ps'] not in ('none', 'unknown'):
                pairs.add((row['vs'], row['ps']))
if combos:
    ps_all = {h for h, (st, _) in unique.items() if st == 'ps'}
    vs_all = {h for h, (st, _) in unique.items() if st == 'vs'}
    ps_bound = {p for _, p in pairs}
    vs_bound = {v for v, _ in pairs}
    unknown = sum(1 for c in combos if 'unknown' in c)
    print(f'{len(combos)} unique pipeline combinations, {len(pairs)} vertex/pixel shader pairs')
    print(f'pixel shaders drawn in a pair: {len(ps_bound & ps_all)} of {len(ps_all)}')
    print(f'vertex shaders drawn in a pair: {len(vs_bound & vs_all)} of {len(vs_all)}')
    print(f'combinations with an unknown object: {unknown}')
PY
    ;;
  remove)
    game_running && { print "SC6 is running. Quit it first."; exit 1; }
    is_sc6vrr $dll || { print "The installed dinput8.dll is not an sc6vrr build; not removing it."; exit 1; }
    rm $dll
    print "removed: $dll"
    ;;
  *)
    sed -n '2,10p' $0; exit 1 ;;
esac
