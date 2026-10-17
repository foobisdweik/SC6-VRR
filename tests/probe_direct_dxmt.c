/* Create a real device so Wine must load both halves of the private DXMT build. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

int main(void) {
  ID3D11Device *device = NULL;
  ID3D11DeviceContext *context = NULL;
  D3D_FEATURE_LEVEL level;
  HRESULT result = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                    NULL, 0, D3D11_SDK_VERSION, &device, &level, &context);
  printf("D3D11CreateDevice: 0x%08lx; feature level: 0x%x\n", (unsigned long)result, (unsigned)level);
  if (FAILED(result))
    return 1;
  const char *names[] = {"d3d11.dll", "dxgi.dll", "winemetal.dll"};
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    char path[2048];
    DWORD size = GetModuleFileNameA(GetModuleHandleA(names[i]), path, sizeof(path));
    printf("%s: %s\n", names[i], size && size < sizeof(path) ? path : "unavailable");
  }
  typedef int (*fullscreen_fn)(intptr_t, bool);
  FARPROC procedure = GetProcAddress(GetModuleHandleA("winemetal.dll"), "WMTRequestNativeFullscreen");
  fullscreen_fn fullscreen;
  memcpy(&fullscreen, &procedure, sizeof(fullscreen));
  if (!fullscreen) {
    fprintf(stderr, "The custom fullscreen export is missing.\n");
    return 2;
  }
  /* The call returns the previous state. The second call must report unavailable. */
  fullscreen(0, false);
  int state = fullscreen(0, false);
  printf("Native fullscreen probe for a null window: %d\n", state);
  if (state != 1)
    return 3;
  ID3D11DeviceContext_Release(context);
  ID3D11Device_Release(device);
  return 0;
}
