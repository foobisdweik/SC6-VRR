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

  char dir[MAX_PATH], path[MAX_PATH + 128];
  GetTempPathA(sizeof(dir) - 40, dir);
  snprintf(dir + strlen(dir), 40, "sc6vrr-shader-test-%lu", GetCurrentProcessId());
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

  printf("shader capture test passed: %s\n", dir);
  return 0;
}
