// Local regression checks for DXMT issues 204, 228, 230 and PR 222.
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <d2d1.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static pD3DCompile compileShader;
static int failures;
#define CHECK(call) do { HRESULT hr_ = (call); if (FAILED(hr_)) { \
    std::printf("FAIL %s: 0x%08lx\n", #call, (unsigned long)hr_); std::exit(2); } } while (0)
static void check(bool ok, const char *label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
static ID3D11ComputeShader *shader(const char *source) {
    ID3DBlob *code = nullptr, *error = nullptr;
    HRESULT hr = compileShader(source, std::strlen(source), "regression", nullptr, nullptr,
        "main", "cs_5_0", 0, 0, &code, &error);
    if (FAILED(hr)) {
        std::printf("Shader failed: %s\n", error ? (char *)error->GetBufferPointer() : "unknown");
        std::exit(2);
    }
    ID3D11ComputeShader *result;
    CHECK(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &result));
    code->Release();
    if (error) error->Release();
    return result;
}
struct Output {
    ID3D11Buffer *gpu, *cpu;
    ID3D11UnorderedAccessView *view;
    Output(UINT words) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = words * 4; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        CHECK(dev->CreateBuffer(&bd, nullptr, &gpu));
        bd.BindFlags = 0; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CHECK(dev->CreateBuffer(&bd, nullptr, &cpu));
        D3D11_UNORDERED_ACCESS_VIEW_DESC vd = {};
        vd.Format = DXGI_FORMAT_R32_UINT; vd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        vd.Buffer.NumElements = words;
        CHECK(dev->CreateUnorderedAccessView(gpu, &vd, &view));
    }
    std::vector<UINT> read(UINT words) {
        ID3D11UnorderedAccessView *none = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
        ctx->CopyResource(cpu, gpu);
        D3D11_MAPPED_SUBRESOURCE map;
        CHECK(ctx->Map(cpu, 0, D3D11_MAP_READ, 0, &map));
        auto p = (UINT *)map.pData;
        std::vector<UINT> result(p, p + words);
        ctx->Unmap(cpu, 0);
        return result;
    }
    ~Output() { view->Release(); cpu->Release(); gpu->Release(); }
};
static void rgb32() {
    struct Case { DXGI_FORMAT format; const char *type; int components; bool floating; bool negative; };
    const Case cases[] = {
        {DXGI_FORMAT_R32G32B32_FLOAT, "float3", 3, true, false},
        {DXGI_FORMAT_R32G32B32_UINT, "uint3", 3, false, false},
        {DXGI_FORMAT_R32G32B32_SINT, "int3", 3, false, true},
        {DXGI_FORMAT_R32_FLOAT, "float", 1, true, false},
        {DXGI_FORMAT_R32G32_FLOAT, "float2", 2, true, false},
        {DXGI_FORMAT_R32G32B32A32_FLOAT, "float4", 4, true, false},
    };
    for (auto c : cases) {
        char source[2048];
        const char *conversion = c.components == 1 ? "uint4(src[i],0,0,1)" :
            c.components == 2 ? "uint4(src[i],0,1)" : c.components == 3 ? "uint4(src[i],1)" : "uint4(src[i])";
        std::snprintf(source, sizeof(source),
            "Buffer<%s> src : register(t0); RWBuffer<uint> dst : register(u0);"
            "[numthreads(8,1,1)] void main(uint3 id:SV_DispatchThreadID) {"
            "uint i=id.x; uint n=4; uint4 v=%s;"
            "dst[i*5]=v.z; dst[i*5+1]=v.x; dst[i*5+2]=v.y; dst[i*5+3]=v.w; dst[i*5+4]=n; }",
            c.type, conversion);
        auto cs = shader(source);
        UINT input[24];
        for (UINT i = 0; i < 24; ++i) {
            if (c.floating) { float v = float(i + 1); std::memcpy(&input[i], &v, 4); }
            else input[i] = c.negative ? UINT(-int(i + 1)) : i + 1;
        }
        D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = sizeof(input); bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial = {input, 0, 0};
        ID3D11Buffer *buffer; CHECK(dev->CreateBuffer(&bd, &initial, &buffer));
        for (UINT first : {0u, 1u, 2u}) {
            D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
            vd.Format = c.format; vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            vd.Buffer.FirstElement = first; vd.Buffer.NumElements = 4;
            ID3D11ShaderResourceView *view; CHECK(dev->CreateShaderResourceView(buffer, &vd, &view));
            Output out(40);
            ctx->CSSetShader(cs, nullptr, 0); ctx->CSSetShaderResources(0, 1, &view);
            ctx->CSSetUnorderedAccessViews(0, 1, &out.view, nullptr); ctx->Dispatch(1, 1, 1);
            auto values = out.read(40);
            bool ok = true;
            for (UINT i = 0; i < 8; ++i) {
                UINT expected[4] = {0,0,0,1};
                for (int component = 0; component < c.components; ++component) {
                    UINT index = (first + i) * c.components + component;
                    expected[component] = c.negative ? UINT(-int(index + 1)) : index + 1;
                }
                if (i >= 4) { for (auto &v : expected) v = 0; if (c.components < 4) expected[3] = 1; }
                UINT swizzle[4] = {expected[2], expected[0], expected[1], expected[3]};
                for (UINT component = 0; component < 4; ++component)
                    ok &= values[i*5+component] == swizzle[component];
                ok &= values[i*5+4] == 4;
            }
            char label[160]; std::snprintf(label, sizeof(label), "%s FirstElement=%u values, swizzle and bounds", c.type, first);
            check(ok, label);
            ID3D11ShaderResourceView *none = nullptr; ctx->CSSetShaderResources(0, 1, &none);
            view->Release();
        }
        buffer->Release(); cs->Release();
    }
}
static void bufferReplay() {
    auto cs = shader("cbuffer Args:register(b0){ uint value; uint slot; uint2 pad; };"
        "RWBuffer<uint> dst:register(u0); [numthreads(1,1,1)] void main(){dst[slot]=value;}");
    Output out(256);
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 16; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer *cb; CHECK(dev->CreateBuffer(&bd, nullptr, &cb));
    ID3D11DeviceContext *deferred; CHECK(dev->CreateDeferredContext(0, &deferred));
    deferred->CSSetShader(cs, nullptr, 0); deferred->CSSetConstantBuffers(0, 1, &cb);
    deferred->CSSetUnorderedAccessViews(0, 1, &out.view, nullptr);
    UINT args[4] = {123, 0, 0, 0};
    deferred->UpdateSubresource(cb, 0, nullptr, args, 0, 0); deferred->Dispatch(1,1,1);
    ID3D11CommandList *list; CHECK(deferred->FinishCommandList(FALSE, &list));
    for (UINT i=1; i<256; ++i) {
        ctx->ExecuteCommandList(list, FALSE);
        ctx->CSSetShader(cs, nullptr, 0); ctx->CSSetConstantBuffers(0, 1, &cb);
        ctx->CSSetUnorderedAccessViews(0, 1, &out.view, nullptr);
        args[0] = 1000+i; args[1] = i;
        ctx->UpdateSubresource(cb, 0, nullptr, args, 0, 0); ctx->Dispatch(1,1,1);
    }
    auto values = out.read(256);
    bool ok = values[0] == 123;
    for (UINT i=1; i<256; ++i) ok &= values[i] == 1000+i;
    check(ok, "DEFAULT constant buffer: 255 command-list replays with intervening updates");
    list->Release(); deferred->Release(); cb->Release(); cs->Release();
}
static void timestamps() {
    D3D11_QUERY_DESC td = {D3D11_QUERY_TIMESTAMP, 0}, ed = {D3D11_QUERY_EVENT, 0};
    ID3D11Query *timestamp, *event;
    CHECK(dev->CreateQuery(&td, &timestamp)); CHECK(dev->CreateQuery(&ed, &event));
    bool ok = true;
    UINT64 previous = 0;
    int earlyFailures=0, lateFailures=0;
    for (int i=0; i<200; ++i) {
        ctx->End(timestamp);
        // An unsubmitted timestamp with DONOTFLUSH must remain nonblocking.
        UINT64 value = 0; ULONGLONG start = GetTickCount64();
        HRESULT early = ctx->GetData(timestamp, &value, sizeof(value), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        bool earlyOK = early == S_FALSE && GetTickCount64()-start < 1000;
        earlyFailures += !earlyOK; ok &= earlyOK;
        ctx->End(event); ctx->Flush();
        BOOL done = FALSE;
        while (ctx->GetData(event, &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || !done) {
            if (GetTickCount64()-start > 10000) { check(false, "timestamp event timeout"); return; }
        }
        HRESULT hr = ctx->GetData(timestamp, &value, sizeof(value), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        bool lateOK = hr == S_OK && value != 0 && value >= previous;
        if (!lateOK) std::printf("timestamp iteration=%d hr=%lx value=%llu previous=%llu\n", i, (unsigned long)hr, value, previous);
        lateFailures += !lateOK; ok &= lateOK;
        previous = value;
    }
    std::printf("timestamp early failures=%d late failures=%d\n", earlyFailures, lateFailures);
    check(ok, "200 reused timestamps: nonblocking before submission, ready after later event");
    event->Release(); timestamp->Release();
}
static void interop() {
    ID3D11Device1 *dev1; ID3D11DeviceContext1 *ctx1;
    CHECK(dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev1));
    CHECK(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx1));
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0, chosen;
    ID3DDeviceContextState *state, *previous = nullptr, *returned = nullptr;
    CHECK(dev1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
        __uuidof(ID3D11Device), &chosen, &state));
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx1->SwapDeviceContextState(state, &previous);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ctx1->SwapDeviceContextState(previous, &returned);
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ctx->IAGetPrimitiveTopology(&topology);
    check(topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, "SwapDeviceContextState restores prior state");
    ctx1->SwapDeviceContextState(returned, nullptr);
    ctx->IAGetPrimitiveTopology(&topology);
    check(topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST, "SwapDeviceContextState preserves alternate state");
    returned->Release(); previous->Release(); state->Release();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width=16; td.Height=8; td.MipLevels=1; td.ArraySize=1; td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_STAGING; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D *texture; CHECK(dev->CreateTexture2D(&td, nullptr, &texture));
    IDXGISurface2 *surface; CHECK(texture->QueryInterface(__uuidof(IDXGISurface2), (void **)&surface));
    DXGI_SURFACE_DESC desc; CHECK(surface->GetDesc(&desc));
    DXGI_MAPPED_RECT mapped; CHECK(surface->Map(&mapped, DXGI_MAP_WRITE));
    std::memset(mapped.pBits, 0x5a, mapped.Pitch*td.Height); CHECK(surface->Unmap());
    CHECK(surface->Map(&mapped, DXGI_MAP_READ));
    check(desc.Width==16 && desc.Height==8 && mapped.pBits[0]==0x5a, "IDXGISurface2 dimensions and mapped contents");
    CHECK(surface->Unmap());
    ID3D11Texture2D *parent; UINT subresource=99;
    CHECK(surface->GetResource(__uuidof(ID3D11Texture2D), (void **)&parent, &subresource));
    check(parent==texture && subresource==0, "IDXGISurface2 returns its resource");
    parent->Release(); surface->Release();
    IDXGIResource1 *resource; CHECK(texture->QueryInterface(__uuidof(IDXGIResource1), (void **)&resource));
    auto refsBefore = texture->AddRef(); texture->Release();
    for (unsigned i=0; i<100; ++i) {
        CHECK(resource->CreateSubresourceSurface(0, &surface));
        CHECK(surface->GetDesc(&desc)); surface->Release();
    }
    auto refsAfter = texture->AddRef(); texture->Release();
    check(refsBefore==refsAfter, "100 subresource surfaces retain and release their parent");
    surface=nullptr;
    check(FAILED(resource->CreateSubresourceSurface(1, &surface)), "Subresource index at the resource count is rejected");
    resource->Release(); texture->Release(); ctx1->Release(); dev1->Release();
}
static void direct2D() {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width=16; td.Height=16; td.MipLevels=1; td.ArraySize=1;
    td.Format=DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count=1;
    td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *texture; CHECK(dev->CreateTexture2D(&td, nullptr, &texture));
    IDXGISurface *surface; CHECK(texture->QueryInterface(__uuidof(IDXGISurface), (void **)&surface));
    ID2D1Factory *factory; CHECK(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory));
    auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1RenderTarget *target;
    CHECK(factory->CreateDxgiSurfaceRenderTarget(surface, &properties, &target));
    target->BeginDraw(); target->Clear(D2D1::ColorF(1,0,0,1)); CHECK(target->EndDraw());
    td.BindFlags=0; td.Usage=D3D11_USAGE_STAGING; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *cpu; CHECK(dev->CreateTexture2D(&td, nullptr, &cpu));
    ctx->CopyResource(cpu, texture);
    D3D11_MAPPED_SUBRESOURCE map; CHECK(ctx->Map(cpu,0,D3D11_MAP_READ,0,&map));
    auto pixel = (const BYTE *)map.pData;
    check(pixel[0]==0 && pixel[1]==0 && pixel[2]==255 && pixel[3]==255,
        "Direct2D clears a DXMT surface and D3D11 reads the red pixel");
    ctx->Unmap(cpu,0); cpu->Release(); target->Release(); factory->Release();
    surface->Release(); texture->Release();
}
int main(int argc, char **argv) {
    auto compiler = LoadLibraryA("d3dcompiler_47.dll");
    compileShader = compiler ? (pD3DCompile)GetProcAddress(compiler, "D3DCompile") : nullptr;
    if (!compileShader) return 2;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    if (argc > 1 && std::strcmp(argv[1], "timestamp") == 0) timestamps();
    else { rgb32(); bufferReplay(); timestamps(); interop(); direct2D(); }
    ctx->ClearState(); ctx->Flush(); ctx->Release(); dev->Release();
    std::printf("Total failures: %d\n", failures);
    return failures != 0;
}
