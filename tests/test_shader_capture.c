/* Run this Windows executable under Wine. It tests shader capture with a fake
 * Direct3D 11 device. It neither loads the game nor calls setup(). */
#include <assert.h>
#include "../dll/src/sc6vrr.c"

/* A minimal DXBC container: header, one chunk offset, and a SHEX chunk whose
 * first token is vs_5_0 (program type 1, model 5.0). */
static void make_dxbc(uint8_t *out, uint32_t total, uint32_t token, uint8_t fill) {
  memset(out, fill, total);
  memcpy(out, "DXBC", 4);
  uint32_t version = 1, count = 1, off = 36, size = total - 44;
  memcpy(out + 20, &version, 4);
  memcpy(out + 24, &total, 4);
  memcpy(out + 28, &count, 4);
  memcpy(out + 32, &off, 4);
  memcpy(out + 36, "SHEX", 4);
  memcpy(out + 40, &size, 4);
  memcpy(out + 44, &token, 4);
}

static unsigned fake_calls[STAGE_COUNT];
static unsigned fake_objects;
/* Each fake creation returns a new address, as a real device would. */
#define NEW_OBJECT() ((void *)(uintptr_t)(0x10000 + 0x100 * ++fake_objects))
static HRESULT fake_result = S_OK;

#define FAKE_CREATE(stage)                                                                               \
  static __attribute__((noinline)) HRESULT WINAPI fake_create_##stage(void *self, const void *code, SIZE_T length, void *linkage, \
                                            void **out) {                                               \
    (void)self, (void)code, (void)length, (void)linkage;                                                \
    fake_calls[STAGE_##stage]++;                                                                        \
    if (out) *out = NEW_OBJECT();                                                                       \
    return fake_result;                                                                                 \
  }
FAKE_CREATE(VS)
FAKE_CREATE(GS)
FAKE_CREATE(PS)
FAKE_CREATE(HS)
FAKE_CREATE(DS)
FAKE_CREATE(CS)

static __attribute__((noinline)) HRESULT WINAPI fake_create_GS_SO(void *self, const void *code, SIZE_T length, const void *entries,
                                        UINT entry_count, const UINT *strides, UINT stride_count,
                                        UINT rasterized_stream, void *linkage, void **out) {
  (void)self, (void)code, (void)length, (void)entries, (void)strides, (void)linkage;
  assert(entry_count == 2 && stride_count == 1 && rasterized_stream == 3);
  fake_calls[STAGE_GS_SO]++;
  if (out) *out = (void *)0x5678;
  return S_OK;
}

static __attribute__((noinline)) HRESULT WINAPI fake_create_input_layout(void *self, const D3D11_INPUT_ELEMENT_DESC *e,
                                                                         UINT n, const void *code, SIZE_T length,
                                                                         void **out) {
  (void)self, (void)e, (void)n, (void)code, (void)length;
  if (out) *out = NEW_OBJECT();
  return S_OK;
}

/* A fake immediate context: the Set methods and draws count their calls. */
static void *volatile fake_context_vtable[120];
static struct { void *volatile *vtable; } fake_context = {fake_context_vtable};
static unsigned context_refs, fake_sets, fake_draws;
static __attribute__((noinline)) ULONG WINAPI fake_release(void *self) { (void)self; return --context_refs; }
static __attribute__((noinline)) void WINAPI fake_get_context(void *device, void **context) {
  (void)device;
  context_refs++;
  *context = &fake_context;
}
static __attribute__((noinline)) void WINAPI fake_set_shader(void *ctx, void *s, void *const *i, UINT n) {
  (void)ctx, (void)s, (void)i, (void)n;
  fake_sets++;
}
static __attribute__((noinline)) void WINAPI fake_set_one(void *ctx, void *value) { (void)ctx, (void)value; fake_sets++; }
static __attribute__((noinline)) void WINAPI fake_draw_indexed(void *ctx, UINT c, UINT s, INT b) {
  (void)ctx, (void)c, (void)s, (void)b;
  fake_draws++;
}
static __attribute__((noinline)) void WINAPI fake_draw(void *ctx, UINT c, UINT s) { (void)ctx, (void)c, (void)s; fake_draws++; }

static __attribute__((noinline)) void WINAPI fake_set_buffers(void *ctx, UINT start, UINT n, void *const *b,
                                                               const UINT *strides, const UINT *offsets) {
  (void)ctx, (void)start, (void)n, (void)b, (void)strides, (void)offsets;
  fake_sets++;
}
static __attribute__((noinline)) void WINAPI fake_set_targets(void *ctx, UINT n, void *const *rtvs, void *dsv) {
  (void)ctx, (void)n, (void)rtvs, (void)dsv;
  fake_sets++;
}
static __attribute__((noinline)) void WINAPI fake_set_targets_uavs(void *ctx, UINT n, void *const *rtvs, void *dsv,
                                                                    UINT us, UINT un, void *const *u, const UINT *c) {
  (void)ctx, (void)n, (void)rtvs, (void)dsv, (void)us, (void)un, (void)u, (void)c;
  fake_sets++;
}
static __attribute__((noinline)) void WINAPI fake_set_blend(void *ctx, void *state, const FLOAT f[4], UINT mask) {
  (void)ctx, (void)state, (void)f, (void)mask;
  fake_sets++;
}
static __attribute__((noinline)) HRESULT WINAPI fake_create_blend_state(void *self, const D3D11_BLEND_DESC *d,
                                                                        void **out) {
  (void)self, (void)d;
  if (out) *out = NEW_OBJECT();
  return S_OK;
}

/* Fake views: GetDesc reports a 16-bit float color target and a 24/8 depth target. */
static __attribute__((noinline)) void WINAPI fake_rtv_desc(void *view, D3D11_RENDER_TARGET_VIEW_DESC *d) {
  (void)view;
  memset(d, 0, sizeof(*d));
  d->Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  d->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
}
static __attribute__((noinline)) void WINAPI fake_dsv_desc(void *view, D3D11_DEPTH_STENCIL_VIEW_DESC *d) {
  (void)view;
  memset(d, 0, sizeof(*d));
  d->Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  d->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
}
static void *volatile fake_rtv_vtable[16], *volatile fake_dsv_vtable[16];
static struct { void *volatile *vtable; } fake_rtv = {fake_rtv_vtable}, fake_dsv = {fake_dsv_vtable};

/* A fake 1920x1080 texture behind the views, for the frame log. */
static unsigned texture_refs;
static __attribute__((noinline)) ULONG WINAPI fake_texture_release(void *t) { (void)t; return --texture_refs; }
static __attribute__((noinline)) void WINAPI fake_texture_type(void *t, D3D11_RESOURCE_DIMENSION *type) {
  (void)t;
  *type = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
}
static __attribute__((noinline)) void WINAPI fake_texture_desc(void *t, D3D11_TEXTURE2D_DESC *d) {
  (void)t;
  memset(d, 0, sizeof(*d));
  d->Width = 1920, d->Height = 1080, d->MipLevels = 1, d->ArraySize = 1;
  d->Format = DXGI_FORMAT_R16G16B16A16_FLOAT, d->SampleDesc.Count = 1;
}
static void *volatile fake_texture_vtable[16];
static struct { void *volatile *vtable; } fake_texture = {fake_texture_vtable};
static __attribute__((noinline)) void WINAPI fake_view_resource(void *view, void **resource) {
  (void)view;
  texture_refs++;
  *resource = &fake_texture;
}

/* Fake context methods that only the frame log hooks. */
static unsigned fake_frame_calls;
static __attribute__((noinline)) void WINAPI fake_clear_rtv(void *ctx, void *rtv, const FLOAT c[4]) {
  (void)ctx, (void)rtv, (void)c;
  fake_frame_calls++;
}
static __attribute__((noinline)) void WINAPI fake_dispatch(void *ctx, UINT x, UINT y, UINT z) {
  (void)ctx, (void)x, (void)y, (void)z;
  fake_frame_calls++;
}

/* A fake DXGI factory and swap chain. */
static unsigned presents;
static __attribute__((noinline)) HRESULT WINAPI fake_present(void *sc, UINT sync, UINT flags) {
  (void)sc, (void)sync, (void)flags;
  presents++;
  return S_OK;
}
static void *volatile fake_swapchain_vtable[32];
static struct { void *volatile *vtable; } fake_swapchain = {fake_swapchain_vtable};
static __attribute__((noinline)) HRESULT WINAPI fake_create_swapchain(void *f, void *dev, DXGI_SWAP_CHAIN_DESC *d,
                                                                      void **out) {
  (void)f, (void)dev, (void)d;
  *out = &fake_swapchain;
  return S_OK;
}
static void *volatile fake_factory_vtable[32];
static struct { void *volatile *vtable; } fake_factory = {fake_factory_vtable};
static __attribute__((noinline)) HRESULT WINAPI fake_create_factory1(REFIID riid, void **out) {
  (void)riid;
  *out = &fake_factory;
  return S_OK;
}
/* Fake state creation and binding, for the depth, rasterizer, and sampler records. */
static __attribute__((noinline)) HRESULT WINAPI fake_create_state(void *self, const void *desc, void **out) {
  (void)self, (void)desc;
  *out = NEW_OBJECT();
  return S_OK;
}
static __attribute__((noinline)) void WINAPI fake_set_depth_state(void *ctx, void *state, UINT ref) {
  (void)ctx, (void)state, (void)ref;
  fake_frame_calls++;
}
static __attribute__((noinline)) void WINAPI fake_set_state(void *ctx, void *state) {
  (void)ctx, (void)state;
  fake_frame_calls++;
}
static __attribute__((noinline)) void WINAPI fake_set_list(void *ctx, UINT start, UINT n, void *const *items) {
  (void)ctx, (void)start, (void)n, (void)items;
  fake_frame_calls++;
}
/* Fake query, Map, and Flush for the wait counters. GetData reports "not
 * ready" fake_not_ready times, then S_OK. */
static unsigned fake_not_ready, fake_maps, fake_flushes;
static __attribute__((noinline)) HRESULT WINAPI fake_get_data(void *ctx, void *q, void *data, UINT size, UINT flags) {
  (void)ctx, (void)q, (void)data, (void)size, (void)flags;
  if (fake_not_ready) {
    fake_not_ready--;
    Sleep(2);
    return S_FALSE;
  }
  return S_OK;
}
static __attribute__((noinline)) HRESULT WINAPI fake_map(void *ctx, void *r, UINT sub, D3D11_MAP type, UINT flags,
                                                         D3D11_MAPPED_SUBRESOURCE *out) {
  (void)ctx, (void)r, (void)sub, (void)type, (void)flags, (void)out;
  fake_maps++;
  return S_OK;
}
static __attribute__((noinline)) void WINAPI fake_flush(void *ctx) { (void)ctx, fake_flushes++; }
/* A fake event query: only GetDesc is needed. */
static __attribute__((noinline)) void WINAPI fake_query_desc(void *q, D3D11_QUERY_DESC *desc) {
  (void)q, desc->Query = D3D11_QUERY_EVENT, desc->MiscFlags = 0;
}
static void *fake_query_vtable[9];
static struct { void **vtable; } fake_query = {fake_query_vtable};
static int fake_key;
static int fake_key_down(void) { return fake_key; }

static void *volatile fake_vtable[43];
static struct { void *volatile *vtable; } fake_device = {fake_vtable};
static unsigned device_creations;

/* noinline and volatile calls: an inlined copy would bypass the MinHook patch. */
static __attribute__((noinline)) HRESULT WINAPI fake_create_device(void *adapter, int driver_type, HMODULE software, UINT flags,
                                         const void *levels, UINT level_count, UINT sdk_version, void **device,
                                         void *level, void **context) {
  (void)adapter, (void)driver_type, (void)software, (void)flags, (void)levels, (void)level_count;
  (void)sdk_version, (void)level, (void)context;
  device_creations++;
  if (device) *device = &fake_device;
  return S_OK;
}

static int count_lines(const char *path) {
  FILE *f = fopen(path, "r");
  int lines = 0, c;
  assert(f);
  while ((c = fgetc(f)) != EOF)
    lines += c == '\n';
  fclose(f);
  return lines;
}

static long file_size(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fclose(f);
  return n;
}

int main(void) {
  /* SHA-256 known-answer test ("abc", FIPS 180-2). */
  char hex[65];
  assert(sha256_hex("abc", 3, hex));
  assert(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

  /* Parser: valid SM5 vertex shader, SM4 pixel shader, and malformed inputs. */
  uint8_t vs[96], ps[80], bad[96];
  char model[16];
  make_dxbc(vs, sizeof(vs), 0x00010050, 0xA1);
  make_dxbc(ps, sizeof(ps), 0x00000040, 0xB2);
  memcpy(ps + 36, "SHDR", 4);
  dxbc_model_name(parse_dxbc(vs, sizeof(vs)), model);
  assert(!strcmp(model, "vs_5_0"));
  dxbc_model_name(parse_dxbc(ps, sizeof(ps)), model);
  assert(!strcmp(model, "ps_4_0"));
  assert(!parse_dxbc(vs, 31).valid);                 /* shorter than the header */
  assert(!parse_dxbc(vs, sizeof(vs) - 1).valid);     /* total size exceeds the buffer */
  memcpy(bad, vs, sizeof(bad));
  uint32_t far_offset = 0xFFFFFFF0u;
  memcpy(bad + 32, &far_offset, 4);
  assert(!parse_dxbc(bad, sizeof(bad)).valid);       /* chunk offset past the end */
  memcpy(bad, vs, sizeof(bad));
  uint32_t huge_count = 0x40000000u;
  memcpy(bad + 28, &huge_count, 4);
  assert(!parse_dxbc(bad, sizeof(bad)).valid);       /* chunk table past the end */
  memcpy(bad, "DXIL", 4);
  assert(!parse_dxbc(bad, sizeof(bad)).valid);       /* not a DXBC container */

  /* Capture through hooks on a fake device. */
  void *fakes[STAGE_COUNT] = {fake_create_VS, fake_create_GS, fake_create_GS_SO, fake_create_PS,
                              fake_create_HS, fake_create_DS, fake_create_CS};
  for (int i = 0; i < STAGE_COUNT; i++)
    fake_vtable[kStageSlots[i]] = fakes[i];
  fake_vtable[DEVICE_SLOT(CreateInputLayout)] = fake_create_input_layout;
  fake_vtable[DEVICE_SLOT(GetImmediateContext)] = fake_get_context;
  fake_context_vtable[2] = fake_release;
  fake_context_vtable[CONTEXT_SLOT(VSSetShader)] = fake_set_shader;
  fake_context_vtable[CONTEXT_SLOT(GSSetShader)] = fake_set_shader;
  fake_context_vtable[CONTEXT_SLOT(PSSetShader)] = fake_set_shader;
  fake_context_vtable[CONTEXT_SLOT(IASetInputLayout)] = fake_set_one;
  fake_context_vtable[CONTEXT_SLOT(IASetPrimitiveTopology)] = fake_set_one;
  fake_context_vtable[CONTEXT_SLOT(DrawIndexed)] = fake_draw_indexed;
  fake_context_vtable[CONTEXT_SLOT(Draw)] = fake_draw;
  fake_context_vtable[CONTEXT_SLOT(IASetVertexBuffers)] = fake_set_buffers;
  fake_context_vtable[CONTEXT_SLOT(OMSetRenderTargets)] = fake_set_targets;
  fake_context_vtable[CONTEXT_SLOT(OMSetRenderTargetsAndUnorderedAccessViews)] = fake_set_targets_uavs;
  fake_context_vtable[CONTEXT_SLOT(OMSetBlendState)] = fake_set_blend;
  fake_vtable[DEVICE_SLOT(CreateBlendState)] = fake_create_blend_state;
  fake_rtv_vtable[offsetof(ID3D11RenderTargetViewVtbl, GetDesc) / sizeof(void *)] = fake_rtv_desc;
  fake_dsv_vtable[offsetof(ID3D11DepthStencilViewVtbl, GetDesc) / sizeof(void *)] = fake_dsv_desc;
  fake_rtv_vtable[offsetof(ID3D11ViewVtbl, GetResource) / sizeof(void *)] = fake_view_resource;
  fake_dsv_vtable[offsetof(ID3D11ViewVtbl, GetResource) / sizeof(void *)] = fake_view_resource;
  fake_texture_vtable[offsetof(ID3D11ResourceVtbl, Release) / sizeof(void *)] = fake_texture_release;
  fake_texture_vtable[offsetof(ID3D11ResourceVtbl, GetType) / sizeof(void *)] = fake_texture_type;
  fake_texture_vtable[offsetof(ID3D11Texture2DVtbl, GetDesc) / sizeof(void *)] = fake_texture_desc;
  fake_context_vtable[CONTEXT_SLOT(ClearRenderTargetView)] = fake_clear_rtv;
  fake_context_vtable[CONTEXT_SLOT(Dispatch)] = fake_dispatch;
  fake_swapchain_vtable[offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *)] = fake_present;
  fake_vtable[DEVICE_SLOT(CreateDepthStencilState)] = fake_create_state;
  fake_vtable[DEVICE_SLOT(CreateRasterizerState)] = fake_create_state;
  fake_vtable[DEVICE_SLOT(CreateSamplerState)] = fake_create_state;
  fake_context_vtable[CONTEXT_SLOT(OMSetDepthStencilState)] = fake_set_depth_state;
  fake_context_vtable[CONTEXT_SLOT(RSSetState)] = fake_set_state;
  fake_context_vtable[CONTEXT_SLOT(VSSetShaderResources)] = fake_set_list;
  fake_context_vtable[CONTEXT_SLOT(PSSetSamplers)] = fake_set_list;
  fake_context_vtable[CONTEXT_SLOT(GetData)] = fake_get_data;
  fake_context_vtable[CONTEXT_SLOT(Map)] = fake_map;
  fake_context_vtable[CONTEXT_SLOT(Flush)] = fake_flush;
  fake_factory_vtable[offsetof(IDXGIFactoryVtbl, CreateSwapChain) / sizeof(void *)] = fake_create_swapchain;

  char dir[MAX_PATH], path[MAX_PATH + 128];
  GetTempPathA(sizeof(dir) - 40, dir);
  /* Wine reuses process IDs, so add the time to keep each run's folder new. */
  snprintf(dir + strlen(dir), 40, "sc6vrr-shader-test-%lu-%llu", GetCurrentProcessId(),
           (unsigned long long)GetTickCount64());
  assert(MH_Initialize() == MH_OK);
  assert(shader_capture_open(dir));
  assert(install_shader_capture((void *)fake_create_device, NULL));

  void *device = NULL;
  create_device_fn volatile create_device = fake_create_device;
  assert(create_device(NULL, 1, NULL, 0, NULL, 0, 7, &device, NULL, NULL) == S_OK);
  assert(device == &fake_device && device_creations == 1 && shader_hooks_installed == 1);
  assert(context_refs == 0);   /* the hook released the context it fetched */
  /* A second device must not install the hooks twice. */
  assert(create_device(NULL, 1, NULL, 0, NULL, 0, 7, &device, NULL, NULL) == S_OK);

  void *volatile *vt = fake_device.vtable;
  void *out = NULL;
  create_shader_fn create_vs = (create_shader_fn)vt[12], create_ps = (create_shader_fn)vt[15];
  assert(create_vs(device, vs, sizeof(vs), NULL, &out) == S_OK && out);
  void *vs_object = out;
  assert(create_vs(device, vs, sizeof(vs), NULL, &out) == S_OK);   /* duplicate */
  fake_result = E_INVALIDARG;
  assert(create_ps(device, ps, sizeof(ps), NULL, &out) == E_INVALIDARG);   /* result passes through */
  fake_result = S_OK;
  UINT strides[1] = {16};
  out = NULL;
  assert(((create_gs_so_fn)vt[14])(device, bad, sizeof(bad), (void *)0x1, 2, strides, 1, 3, NULL, &out) == S_OK &&
         out == (void *)0x5678);
  assert(((create_shader_fn)vt[18])(device, NULL, 0, NULL, &out) == S_OK);   /* no bytecode */
  assert(fake_calls[STAGE_VS] == 2 && fake_calls[STAGE_PS] == 1 && fake_calls[STAGE_GS_SO] == 1 &&
         fake_calls[STAGE_CS] == 1);
  assert(shader_calls == 5 && shader_unique == 3);

  /* Each unique blob is stored once with its exact size and bytes. */
  assert(sha256_hex(vs, sizeof(vs), hex));
  snprintf(path, sizeof(path), "%s\\dxbc\\%s.dxbc", dir, hex);
  assert(file_size(path) == sizeof(vs));
  FILE *f = fopen(path, "rb");
  uint8_t back[sizeof(vs)];
  assert(f && fread(back, 1, sizeof(back), f) == sizeof(back) && !memcmp(back, vs, sizeof(vs)));
  fclose(f);
  assert(sha256_hex(ps, sizeof(ps), hex));
  snprintf(path, sizeof(path), "%s\\dxbc\\%s.dxbc", dir, hex);
  assert(file_size(path) == sizeof(ps));

  /* The index has a header and one line per call. */
  WIN32_FIND_DATAA found;
  snprintf(path, sizeof(path), "%s\\index-*.tsv", dir);
  HANDLE h = FindFirstFileA(path, &found);
  assert(h != INVALID_HANDLE_VALUE);
  FindClose(h);
  fflush(shader_index);
  snprintf(path, sizeof(path), "%s\\%s", dir, found.cFileName);
  assert(count_lines(path) == 6);

  /* Input layouts: identical element lists share one file and one identity. */
  D3D11_INPUT_ELEMENT_DESC elements[2] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  create_input_layout_fn create_layout = (create_input_layout_fn)vt[DEVICE_SLOT(CreateInputLayout)];
  void *layout = NULL, *layout2 = NULL;
  assert(create_layout(device, elements, 2, vs, sizeof(vs), &layout) == S_OK && layout);
  assert(create_layout(device, elements, 2, vs, sizeof(vs), &layout2) == S_OK && layout2 != layout);
  snprintf(path, sizeof(path), "%s\\layouts\\*.txt", dir);
  int layout_files = 0;
  for (HANDLE l = FindFirstFileA(path, &found); l != INVALID_HANDLE_VALUE;) {
    layout_files++;
    if (!FindNextFileA(l, &found)) { FindClose(l); break; }
  }
  assert(layout_files == 1);

  /* Bindings: one line per new pipeline combination, resolved to hashes. */
  void *ps_object = NULL, *blend = NULL;
  assert(create_ps(device, ps, sizeof(ps), NULL, &ps_object) == S_OK && ps_object);
  D3D11_BLEND_DESC blend_desc = {0};
  blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  create_blend_state_fn create_blend = (create_blend_state_fn)vt[DEVICE_SLOT(CreateBlendState)];
  assert(create_blend(device, &blend_desc, &blend) == S_OK && blend);

  void *volatile *cvt = fake_context.vtable;
  set_shader_fn vs_set = (set_shader_fn)cvt[CONTEXT_SLOT(VSSetShader)];
  set_shader_fn ps_set = (set_shader_fn)cvt[CONTEXT_SLOT(PSSetShader)];
  set_layout_fn layout_set = (set_layout_fn)cvt[CONTEXT_SLOT(IASetInputLayout)];
  set_topology_fn topology_set = (set_topology_fn)cvt[CONTEXT_SLOT(IASetPrimitiveTopology)];
  set_vertex_buffers_fn buffers_set = (set_vertex_buffers_fn)cvt[CONTEXT_SLOT(IASetVertexBuffers)];
  set_targets_fn targets_set = (set_targets_fn)cvt[CONTEXT_SLOT(OMSetRenderTargets)];
  set_targets_uavs_fn targets_uavs_set =
      (set_targets_uavs_fn)cvt[CONTEXT_SLOT(OMSetRenderTargetsAndUnorderedAccessViews)];
  set_blend_fn blend_set = (set_blend_fn)cvt[CONTEXT_SLOT(OMSetBlendState)];
  draw_indexed_fn draw_indexed = (draw_indexed_fn)cvt[CONTEXT_SLOT(DrawIndexed)];
  draw_fn draw = (draw_fn)cvt[CONTEXT_SLOT(Draw)];
  void *buffers[2] = {(void *)0xB0, (void *)0xB1}, *rtvs[1] = {&fake_rtv};
  UINT strides2[2] = {20, 99}, offsets[2] = {0, 0}, other_stride[1] = {77};

  layout_set(&fake_context, layout);              /* the layout reads slot 0 only */
  topology_set(&fake_context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  buffers_set(&fake_context, 0, 2, buffers, strides2, offsets);
  targets_set(&fake_context, 1, rtvs, &fake_dsv);
  blend_set(&fake_context, blend, NULL, 0xFFFFFFFF);
  vs_set(&fake_context, vs_object, NULL, 0);
  ps_set(&fake_context, ps_object, NULL, 0);
  draw_indexed(&fake_context, 3, 0, 0);
  draw_indexed(&fake_context, 3, 0, 0);          /* same combination: no new line */
  buffers_set(&fake_context, 1, 1, &buffers[1], other_stride, offsets);
  draw_indexed(&fake_context, 3, 0, 0);          /* slot 1 is unused by the layout: no new line */
  targets_uavs_set(&fake_context, D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, NULL, NULL, 0, 0, NULL, NULL);
  draw_indexed(&fake_context, 3, 0, 0);          /* targets kept: no new line */
  layout_set(&fake_context, layout2);
  draw_indexed(&fake_context, 3, 0, 0);          /* equal layout at a new address: no new line */
  ps_set(&fake_context, NULL, NULL, 0);
  draw(&fake_context, 3, 0);                      /* pixel shader "none" */
  ps_set(&fake_context, (void *)0xDEAD00, NULL, 0);
  draw(&fake_context, 3, 0);                      /* never created here: "unknown" */
  assert(fake_sets == 12 && fake_draws == 7 && draw_calls == 7 && binding_count == 3);

  fflush(binding_file);
  snprintf(path, sizeof(path), "%s\\bindings-*.tsv", dir);
  h = FindFirstFileA(path, &found);
  assert(h != INVALID_HANDLE_VALUE);
  FindClose(h);
  snprintf(path, sizeof(path), "%s\\%s", dir, found.cFileName);
  f = fopen(path, "r");
  char line[1024], vs_hex[65], ps_hex[65], layout_hex[65], blend_hex[65], expect[600];
  assert(f && fgets(line, sizeof(line), f));     /* header */
  assert(sha256_hex(vs, sizeof(vs), vs_hex) && sha256_hex(ps, sizeof(ps), ps_hex));
  EnterCriticalSection(&shader_lock);
  id_hex(object_lookup(layout, NULL), layout_hex);
  id_hex(object_lookup(blend, NULL), blend_hex);
  LeaveCriticalSection(&shader_lock);
  /* Format 10 is R16G16B16A16_FLOAT and 45 is D24_UNORM_S8_UINT. */
  snprintf(expect, sizeof(expect), "\t4\t%s\t%s\tnone\t%s\t%s\t10\t45\t0x0\t0:20\n", layout_hex, vs_hex, ps_hex,
           blend_hex);
  assert(fgets(line, sizeof(line), f) && strstr(line, expect));
  snprintf(expect, sizeof(expect), "\t%s\tnone\tnone\t%s\t10\t45\t0x0\t0:20\n", vs_hex, blend_hex);
  assert(fgets(line, sizeof(line), f) && strstr(line, expect));
  snprintf(expect, sizeof(expect), "\t%s\tnone\tunknown\t%s\t", vs_hex, blend_hex);
  assert(fgets(line, sizeof(line), f) && strstr(line, expect));
  assert(!fgets(line, sizeof(line), f));
  fclose(f);
  snprintf(path, sizeof(path), "%s\\blend\\%s.txt", dir, blend_hex);
  assert(file_size(path) > 0);

  /* Frame log: F10 at one Present arms it, the next Present starts it, and the
   * Present after that ends it. Calls outside that frame are not logged. */
  frame_key_down = fake_key_down;
  assert(install_frame_log(NULL, (void *)fake_create_factory1));
  create_factory_fn volatile create_factory1 = fake_create_factory1;
  void *factory = NULL, *swapchain = NULL;
  static const IID any_iid = {0};                  /* the fake factory ignores it */
  assert(create_factory1(&any_iid, &factory) == S_OK && factory == &fake_factory && factory_hooked);
  void *volatile *fvt = fake_factory.vtable;
  assert(((create_swapchain_fn)fvt[offsetof(IDXGIFactoryVtbl, CreateSwapChain) / sizeof(void *)])(
             factory, device, NULL, &swapchain) == S_OK && swapchain == &fake_swapchain && present_hooked);
  present_fn present = (present_fn)fake_swapchain.vtable[offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *)];
  clear_rtv_fn clear_rtv = (clear_rtv_fn)cvt[CONTEXT_SLOT(ClearRenderTargetView)];
  dispatch_fn dispatch = (dispatch_fn)cvt[CONTEXT_SLOT(Dispatch)];
  FLOAT black[4] = {0, 0, 0, 1};

  /* States: each description is stored once, and draws name the bound ones. */
  D3D11_DEPTH_STENCIL_DESC depth_desc = {0};
  depth_desc.DepthEnable = TRUE, depth_desc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
  D3D11_RASTERIZER_DESC raster_desc = {0};
  raster_desc.FillMode = D3D11_FILL_SOLID, raster_desc.CullMode = D3D11_CULL_BACK;
  D3D11_SAMPLER_DESC sampler_desc = {0};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  void *depth_state = NULL, *raster_state = NULL, *sampler = NULL;
  assert(((create_depth_state_fn)vt[DEVICE_SLOT(CreateDepthStencilState)])(device, &depth_desc, &depth_state) == S_OK);
  assert(((create_raster_state_fn)vt[DEVICE_SLOT(CreateRasterizerState)])(device, &raster_desc, &raster_state) == S_OK);
  assert(((create_sampler_state_fn)vt[DEVICE_SLOT(CreateSamplerState)])(device, &sampler_desc, &sampler) == S_OK);
  char depth_hex[65], raster_hex[65], sampler_hex[65];
  id_hex(object_lookup(depth_state, NULL), depth_hex);
  id_hex(object_lookup(raster_state, NULL), raster_hex);
  id_hex(object_lookup(sampler, NULL), sampler_hex);
  depth_hex[16] = raster_hex[16] = sampler_hex[16] = 0;
  snprintf(path, sizeof(path), "%s\\depth\\%s*.txt", dir, depth_hex);
  h = FindFirstFileA(path, &found);
  assert(h != INVALID_HANDLE_VALUE);
  FindClose(h);
  set_depth_state_fn depth_set = (set_depth_state_fn)cvt[CONTEXT_SLOT(OMSetDepthStencilState)];
  set_raster_state_fn raster_set = (set_raster_state_fn)cvt[CONTEXT_SLOT(RSSetState)];
  set_views_fn vs_srv_set = (set_views_fn)cvt[CONTEXT_SLOT(VSSetShaderResources)];
  set_samplers_fn ps_samplers_set = (set_samplers_fn)cvt[CONTEXT_SLOT(PSSetSamplers)];
  void *vs_views[1] = {&fake_rtv};               /* any view whose resource is the fake texture */

  assert(present(&fake_swapchain, 1, 0) == S_OK && frame_state == FRAME_IDLE);
  clear_rtv(&fake_context, &fake_rtv, black);      /* not logged: no frame armed */
  fake_key = 1;
  present(&fake_swapchain, 1, 0);                   /* F10 pressed: armed */
  assert(frame_state == FRAME_ARMED);
  present(&fake_swapchain, 1, 0);                   /* key held: logging starts, no second arm */
  assert(frame_state == FRAME_LOGGING && frame_file);
  fake_key = 0;
  ps_set(&fake_context, ps_object, NULL, 0);
  targets_set(&fake_context, 1, rtvs, &fake_dsv);
  clear_rtv(&fake_context, &fake_rtv, black);
  depth_set(&fake_context, depth_state, 7);         /* not logged by itself; named at the draw */
  raster_set(&fake_context, raster_state);
  vs_srv_set(&fake_context, 0, 1, vs_views);
  ps_samplers_set(&fake_context, 2, 1, &sampler);
  draw_indexed(&fake_context, 36, 0, 0);
  dispatch(&fake_context, 8, 4, 1);
  present(&fake_swapchain, 1, 0);                   /* frame ends */
  assert(frame_state == FRAME_IDLE && !frame_file && frames_logged == 1);
  present(&fake_swapchain, 1, 0);
  assert(frame_state == FRAME_IDLE && frames_logged == 1 && presents == 5 && fake_frame_calls == 7);
  assert(texture_refs == 0);                        /* every GetResource was released */

  snprintf(path, sizeof(path), "%s\\frames\\frame-*.tsv", dir);
  h = FindFirstFileA(path, &found);
  assert(h != INVALID_HANDLE_VALUE);
  FindClose(h);
  snprintf(path, sizeof(path), "%s\\frames\\%s", dir, found.cFileName);
  f = fopen(path, "r");
  const char *expected[] = {
      "# One frame",
      "0\tFrameStart\t",
      "1\tOMSetRenderTargets\tcount=1\trt0=tex2d:",
      "2\tClearRenderTargetView\ttex2d:",
      "3\tVSSetShaderResources\tstart=0\tcount=1\t0=tex2d:",
      "4\tPSSetSamplers\tstart=2\tcount=1\t2=",
      "5\tDrawIndexed\tcount=36 start=0 base=0\ttopology=4\t",
      "6\tDispatch\tgroups=8,4,1\tcs=none",
      "7\tPresent",
  };
  for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
    assert(fgets(line, sizeof(line), f));
    assert(!strncmp(line, expected[i], strlen(expected[i])));
    if (i == 2)
      assert(strstr(line, ":1920x1080:format=10:mips=1:array=1:samples=1") && strstr(line, "\tdepth=tex2d:"));
    /* expected[0] is the header, so entry i holds log line i - 1. */
    if (i == 5)
      assert(strstr(line, sampler_hex));
    if (i == 6) {
      char states[200];
      snprintf(states, sizeof(states), "\tdepth_state=%s\tstencil_ref=7\traster=%s", depth_hex, raster_hex);
      assert(strstr(line, states));
      char vs_short[17], ps_short[17], expect_ids[64];
      memcpy(vs_short, vs_hex, 16), vs_short[16] = 0;
      memcpy(ps_short, ps_hex, 16), ps_short[16] = 0;
      snprintf(expect_ids, sizeof(expect_ids), "\tvs=%s\tgs=none\tps=%s\t", vs_short, ps_short);
      assert(strstr(line, expect_ids) && strstr(line, "\trt_formats=10\tdsv_format=45"));
    }
  }
  assert(!fgets(line, sizeof(line), f));
  fclose(f);

  /* SC6VRR_SKIP_GS_DRAWS: draws with a geometry shader bound are dropped only
   * when the option is on. */
  set_shader_fn gs_set = (set_shader_fn)cvt[CONTEXT_SLOT(GSSetShader)];
  unsigned draws_before = fake_draws;
  gs_set(&fake_context, (void *)0xBEEF00, NULL, 0);
  draw(&fake_context, 3, 0);                        /* option off: passes through */
  assert(fake_draws == draws_before + 1);
  cfg_skip_gs_draws = 1;
  draw(&fake_context, 3, 0);                        /* option on, geometry shader bound: dropped */
  draw_indexed(&fake_context, 3, 0, 0);
  assert(fake_draws == draws_before + 1 && gs_draws_skipped == 2);
  gs_set(&fake_context, NULL, NULL, 0);
  draw(&fake_context, 3, 0);                        /* no geometry shader: passes through */
  assert(fake_draws == draws_before + 2);
  cfg_skip_gs_draws = 0;

  /* Wait counters: a query polled until ready counts one wait from its first
   * "not ready" result; Present ends a wait that is still open. */
  get_data_fn get_data = (get_data_fn)cvt[CONTEXT_SLOT(GetData)];
  map_fn map = (map_fn)cvt[CONTEXT_SLOT(Map)];
  flush_fn flush = (flush_fn)cvt[CONTEXT_SLOT(Flush)];
  memset(&waits, 0, sizeof(waits));
  fake_query_vtable[8] = fake_query_desc;
  void *query = &fake_query;
  fake_not_ready = 3;
  while (get_data(&fake_context, query, NULL, 0, 0) == S_FALSE) {}
  assert(waits.polls == 4 && waits.not_ready == 3 && waits.query_waits == 1);
  assert(waits.query_ms >= 5.0 && waits.query_max_ms == waits.query_ms);
  assert(waits.kind_waits[D3D11_QUERY_EVENT] == 1 && waits.kind_ms[D3D11_QUERY_EVENT] == waits.query_ms);
  assert(get_data(&fake_context, query, NULL, 0, 0) == S_OK && waits.query_waits == 1); /* ready at once: no wait */
  fake_not_ready = 1;
  assert(get_data(&fake_context, query, NULL, 0, 0) == S_FALSE);
  present(&fake_swapchain, 1, 0);                   /* the open wait ends uncounted */
  assert(get_data(&fake_context, query, NULL, 0, 0) == S_OK && waits.query_waits == 1);
  D3D11_MAPPED_SUBRESOURCE mapped;
  assert(map(&fake_context, (void *)0xABC, 0, D3D11_MAP_READ, 0, &mapped) == S_OK);
  flush(&fake_context);
  assert(fake_maps == 1 && waits.maps == 1 && fake_flushes == 1 && waits.flushes == 1 && waits.frames == 1);

  printf("shader capture test passed: %s\n", dir);
  return 0;
}
