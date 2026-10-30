# Source this file to turn the [launch] section of sc6vrr.ini into environment
# variables. It needs only zsh and awk, so the app bundle carries a copy.
# Set sc6_ini to the file first. Without the file, every key takes its default.
# Result: the array sc6_launch_env ("KEY=value" items).
sc6_ini_get() {  # section, key, default; an inline "; comment" is ignored
  local v=
  [[ -f $sc6_ini ]] && v=$(awk -v s="[$1]" -v k="$2" '
    { sub(/\r$/, "") }
    /^[ \t]*\[/ { gsub(/[ \t]/, ""); ins = (tolower($0) == tolower(s)); next }
    ins { line = $0; sub(/;.*/, "", line); n = index(line, "=")
          if (n) { key = substr(line, 1, n - 1); gsub(/[ \t]/, "", key)
                   if (tolower(key) == tolower(k)) { v = substr(line, n + 1)
                     gsub(/^[ \t]+|[ \t]+$/, "", v); print v; exit } } }' "$sc6_ini")
  print -r -- "${v:-$3}"
}
sc6_ini_tf() { [[ $(sc6_ini_get launch "$1" "$2") == 1 ]] && print True || print False; }

sc6_launch_env=(
  "DXMT_CONFIG=dxgi.nativeFullscreen=$(sc6_ini_tf NativeFullscreen 1);dxgi.metalLayerPixelFormat=$([[ $(sc6_ini_get launch Bgr10a2Layer 1) == 1 ]] && print bgr10a2 || print auto);d3d11.metalSpatialUpscale=$(sc6_ini_tf MetalFXUpscale 0);d3d11.metalSpatialUpscaleFactor=$(sc6_ini_get launch MetalFXUpscaleFactor 2.0);d3d11.preferredMaxFrameRate=$(sc6_ini_get launch DxmtMaxFrameRate 0);d3d11.pipelineArchive=$(sc6_ini_tf PipelineArchive 0);dxgi.metalDisplaySync=$(sc6_ini_tf DisplaySync 0);"
  "DXMT_METALFX_SPATIAL_SWAPCHAIN=0"  # MetalFXUpscale covers the same switch
  "DXMT_ENABLE_NVEXT=$(sc6_ini_get launch NvExt 0)"
  "DXMT_DUMP_LAYERS=${SC6_PRESENT_DIAGNOSTICS:-$(sc6_ini_get launch DumpLayers 1)}"
  "MTL_HUD_ENABLED=${SC6_METAL_HUD:-$(sc6_ini_get launch MetalHUD 1)}"
)
[[ $(sc6_ini_get launch DxmtLog 1) == 1 ]] || sc6_launch_env+=("DXMT_LOG_LEVEL=none" "DXMT_LOG_PATH=none")
[[ $(sc6_ini_get launch WineLog 1) == 1 ]] || sc6_launch_env+=("WINEDEBUG=-all")
# Keys that the [launch] section owns, whether set or not, so callers can
# remove stale copies (for example DXMT_LOG_LEVEL after DxmtLog=1).
sc6_launch_keys=(DXMT_CONFIG DXMT_METALFX_SPATIAL_SWAPCHAIN DXMT_ENABLE_NVEXT DXMT_DUMP_LAYERS
                 MTL_HUD_ENABLED DXMT_LOG_LEVEL DXMT_LOG_PATH WINEDEBUG)
