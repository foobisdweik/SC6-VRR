# Source this file after setting cx_root and repo. It prepares Wine's --env value.
sc6_dxmt_runtime=${SC6_DXMT_RUNTIME:-$HOME/Library/Application Support/SC6-VRR/runtimes/dxmt-direct-x86_64}
sc6_gamemode_helper=${SC6_GAMEMODE_HELPER:-$HOME/.local/bin/crossover-gamemode}
[[ -f $sc6_dxmt_runtime/x86_64-unix/winemetal.so ]] || {
  print -u2 "Run $repo/tools/gamemode/install-direct-dxmt.sh first."; return 1; }
[[ -x $sc6_gamemode_helper ]] || {
  print -u2 "The Game Mode helper is missing: $sc6_gamemode_helper"; return 1; }
for sc6_dll in d3d10core.dll d3d11.dll dxgi.dll winemetal.dll nvapi64.dll nvngx.dll; do
  cmp -s "$sc6_dxmt_runtime/x86_64-windows/$sc6_dll" "$cx_root/lib/dxmt/x86_64-windows/$sc6_dll" || {
    print -u2 "Preview's DXMT differs from the staged build. Run $repo/tools/gamemode/install-direct-dxmt.sh."
    return 1
  }
done
cmp -s "$sc6_dxmt_runtime/x86_64-unix/winemetal.so" "$cx_root/lib/dxmt/x86_64-unix/winemetal.so" || {
  print -u2 "Preview's winemetal.so differs from the staged build. Run $repo/tools/gamemode/install-direct-dxmt.sh."
  return 1
}

sc6_env_list=(
  "WINEDLLOVERRIDES=d3d11,dxgi,winemetal=b"
  "WINELOADER=$sc6_gamemode_helper"
  "CX_GAMEMODE_PROCESS=SoulcaliburVI.exe"
  "CX_GAMEMODE_DRY_RUN=0"
  "CX_GRAPHICS_BACKEND=dxmt"
  "DXMT_CONFIG=dxgi.nativeFullscreen=True;dxgi.metalLayerPixelFormat=bgr10a2;d3d11.metalSpatialUpscale=False;d3d11.preferredMaxFrameRate=0;d3d11.pipelineArchive=False;"
  "DXMT_METALFX_SPATIAL_SWAPCHAIN=0"
  "DXMT_DUMP_LAYERS=${SC6_PRESENT_DIAGNOSTICS:-1}"
  "MTL_HUD_ENABLED=${SC6_METAL_HUD:-1}"
)

# CrossOver parses quoted assignments itself. Escape their values before passing them.
sc6_encode_env() {
  local kv key value
  sc6_env_arg=""
  for kv in "$@"; do
    key=${kv%%=*}; value=${kv#*=}
    value=${value//\\/\\\\}; value=${value//\"/\\\"}
    sc6_env_arg+="$key=\"$value\" "
  done
}
