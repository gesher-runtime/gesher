// gesher: the process's GPU thread (ADR 0089). See gpu_interop.h.
#include "gpu_d3d.h"
#include "gpu_keepalive.h"

#include <gesher/egl_angle.h>

#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifndef EGL_DEVICE_EXT
#define EGL_DEVICE_EXT 0x322C
#endif
#ifndef EGL_D3D11_DEVICE_ANGLE
#define EGL_D3D11_DEVICE_ANGLE 0x33A1
#endif

namespace gesher::bionic::gpu {

namespace {

void Log(const char* text) {
  std::fprintf(stderr, "gesher GPU thread: %s\n", text);
  std::fflush(stderr);
}

// Window pixels (origin top left) to clip space; the texture as sampled, or
// the color; alpha 1 for opaque content; plane alpha (premultiplied).
const char kShaders[] =
    "cbuffer Params : register(b0) {\n"
    "  float2 viewport; float mode; float alpha;\n"
    "  float4 color;\n"
    "  float opaque; float3 pad;\n"
    "};\n"
    "struct VsIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VsOut vs_main(VsIn i) {\n"
    "  VsOut o;\n"
    "  float2 ndc = i.pos / viewport * 2.0 - 1.0;\n"
    "  o.pos = float4(ndc.x, -ndc.y, 0.0, 1.0);\n"
    "  o.uv = i.uv;\n"
    "  return o;\n"
    "}\n"
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "float4 ps_main(VsOut i) : SV_Target {\n"
    "  float4 c = mode > 0.5 ? color : tex.Sample(smp, i.uv);\n"
    "  if (opaque > 0.5) c.a = 1.0;\n"
    "  return c * alpha;\n"
    "}\n";

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                      LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

ID3DBlob* Compile(D3DCompileFn compile, const char* entry, const char* target) {
  ID3DBlob* code = nullptr;
  ID3DBlob* errors = nullptr;
  HRESULT hr = compile(kShaders, sizeof kShaders - 1, "gesher_compose", nullptr, nullptr, entry, target,
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
  if (FAILED(hr)) {
    std::fprintf(stderr, "gesher GPU thread: %s: %s\n", entry,
                 errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
    std::fflush(stderr);
  }
  if (errors != nullptr) errors->Release();
  return SUCCEEDED(hr) ? code : nullptr;
}

}  // namespace

const Gl* Functions() {
  static const Gl* functions = [] {
    auto* gl = new Gl();
    bool ok = true;
#define GESHER_GPU_RESOLVE(name)                                                     \
  gl->name = reinterpret_cast<decltype(&::name)>(gesher_egl_proc(#name));            \
  if (gl->name == nullptr) {                                                         \
    std::fprintf(stderr, "gesher GPU thread: ANGLE lacks %s\n", #name);              \
    ok = false;                                                                      \
  }
    GESHER_GPU_EGL_FUNCTIONS(GESHER_GPU_RESOLVE)
#undef GESHER_GPU_RESOLVE
    if (!ok) {
      std::fflush(stderr);
      delete gl;
      return static_cast<Gl*>(nullptr);
    }
    return gl;
  }();
  return functions;
}

struct Thread::Queue {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::function<void()>> tasks;
  std::thread::id id;
  bool started = false, ok = false;
};

Thread* Thread::Get() {
  static Thread* thread = [] {
    auto* t = new Thread();
    if (!t->Start()) {
      // Kept allocated: a failed start may leave the thread's queue referenced.
      return static_cast<Thread*>(nullptr);
    }
    return t;
  }();
  return thread;
}

bool Thread::Start() {
  gl_ = Functions();
  if (gl_ == nullptr) {
    Log("ANGLE is not available; AHardwareBuffer GPU usage and SurfaceControl composition are not");
    return false;
  }
  queue_ = new Queue();
  std::thread([this] { Main(); }).detach();
  std::unique_lock<std::mutex> lock(queue_->mutex);
  queue_->cv.wait(lock, [this] { return queue_->started; });
  return queue_->ok;
}

bool Thread::InitOnThread() {
  const Gl& g = *gl_;
  display_ = static_cast<EGLDisplay>(gesher_egl_default_display());
  if (display_ == EGL_NO_DISPLAY) {
    Log("no EGL display (no Direct3D 11 device)");
    return false;
  }
  // ANGLE's device (EGL_EXT_device_query, EGL_ANGLE_device_d3d), and its adapter.
  EGLAttrib device = 0, d3d = 0;
  if (g.eglQueryDisplayAttribEXT(display_, EGL_DEVICE_EXT, &device) != EGL_TRUE ||
      g.eglQueryDeviceAttribEXT(reinterpret_cast<EGLDeviceEXT>(device), EGL_D3D11_DEVICE_ANGLE, &d3d) !=
          EGL_TRUE ||
      d3d == 0) {
    Log("ANGLE does not give its Direct3D 11 device");
    return false;
  }
  angle_device_ = reinterpret_cast<ID3D11Device*>(d3d);
  angle_device_->AddRef();
  IDXGIDevice* dxgi_device = nullptr;
  IDXGIAdapter* adapter = nullptr;
  if (FAILED(angle_device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device))) ||
      FAILED(dxgi_device->GetAdapter(&adapter))) {
    if (dxgi_device != nullptr) dxgi_device->Release();
    Log("no adapter for ANGLE's device");
    return false;
  }
  dxgi_device->Release();
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_0;
  HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels,
                                 sizeof levels / sizeof levels[0], D3D11_SDK_VERSION, &device_, &level, &context_);
  if (hr == E_INVALIDARG) {  // no 11.1 runtime
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels + 1,
                           sizeof levels / sizeof levels[0] - 1, D3D11_SDK_VERSION, &device_, &level, &context_);
  }
  adapter->Release();
  if (FAILED(hr)) {
    std::fprintf(stderr, "gesher GPU thread: D3D11CreateDevice failed: 0x%08lx\n", static_cast<unsigned long>(hr));
    return false;
  }
  max_texture_size_ = level >= D3D_FEATURE_LEVEL_11_0 ? D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION : 8192;
  UINT support = 0;
  color_buffer_float_ = SUCCEEDED(device_->CheckFormatSupport(DXGI_FORMAT_R16G16B16A16_FLOAT, &support)) &&
                        (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0;

  // The composing pipeline.
  HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = compiler != nullptr ? reinterpret_cast<D3DCompileFn>(
                                           reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile")))
                                     : nullptr;
  if (compile == nullptr) {
    Log("no D3DCompile (d3dcompiler_47.dll)");
    return false;
  }
  ID3DBlob* vs = Compile(compile, "vs_main", "vs_4_0");
  ID3DBlob* ps = Compile(compile, "ps_main", "ps_4_0");
  if (vs == nullptr || ps == nullptr) return false;
  auto* p = new Pipeline();
  pipeline_ = p;
  const D3D11_INPUT_ELEMENT_DESC inputs[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  bool ok =
      SUCCEEDED(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
                                            &p->vertex_shader)) &&
      SUCCEEDED(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &p->pixel_shader)) &&
      SUCCEEDED(device_->CreateInputLayout(inputs, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &p->layout));
  vs->Release();
  ps->Release();
  D3D11_BUFFER_DESC vb = {};
  vb.ByteWidth = sizeof(Vertex) * 4;
  vb.Usage = D3D11_USAGE_DYNAMIC;
  vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  D3D11_BUFFER_DESC cb = vb;
  cb.ByteWidth = sizeof(Constants);
  cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ok = ok && SUCCEEDED(device_->CreateBuffer(&vb, nullptr, &p->vertices)) &&
       SUCCEEDED(device_->CreateBuffer(&cb, nullptr, &p->constants));
  D3D11_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
  ok = ok && SUCCEEDED(device_->CreateSamplerState(&sampler, &p->sampler));
  D3D11_BLEND_DESC blend = {};
  blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  ok = ok && SUCCEEDED(device_->CreateBlendState(&blend, &p->replace));
  blend.RenderTarget[0].BlendEnable = TRUE;
  blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  ok = ok && SUCCEEDED(device_->CreateBlendState(&blend, &p->premultiplied));
  D3D11_RASTERIZER_DESC raster = {};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  raster.ScissorEnable = TRUE;
  ok = ok && SUCCEEDED(device_->CreateRasterizerState(&raster, &p->raster));
  if (!ok) Log("the composing pipeline could not be made");
  return ok;
}

void Thread::Main() {
  bool ok = InitOnThread();
  {
    std::lock_guard<std::mutex> lock(queue_->mutex);
    queue_->id = std::this_thread::get_id();
    queue_->ok = ok;
    queue_->started = true;
  }
  queue_->cv.notify_all();
  if (!ok) return;
  Log("Direct3D 11 device ready beside ANGLE's (AHardwareBuffer textures, SurfaceControl composition)");
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(queue_->mutex);
      queue_->cv.wait(lock, [this] { return !queue_->tasks.empty(); });
      task = std::move(queue_->tasks.front());
      queue_->tasks.pop_front();
    }
    task();
  }
}

bool Thread::OnThread() const { return std::this_thread::get_id() == queue_->id; }

void Thread::Post(std::function<void()> fn) {
  if (OnThread()) {
    fn();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(queue_->mutex);
    queue_->tasks.push_back(std::move(fn));
  }
  queue_->cv.notify_all();
}

void Thread::Run(const std::function<void()>& fn) {
  if (OnThread()) {
    fn();
    return;
  }
  struct Done {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
  } done;
  {
    std::lock_guard<std::mutex> lock(queue_->mutex);
    queue_->tasks.push_back([&fn, &done] {
      fn();
      std::lock_guard<std::mutex> l(done.mutex);
      done.done = true;
      done.cv.notify_all();
    });
  }
  queue_->cv.notify_all();
  std::unique_lock<std::mutex> lock(done.mutex);
  done.cv.wait(lock, [&done] { return done.done; });
}

void Thread::Finish() {
  D3D11_QUERY_DESC desc = {D3D11_QUERY_EVENT, 0};
  ID3D11Query* query = nullptr;
  if (FAILED(device_->CreateQuery(&desc, &query))) {
    context_->Flush();
    return;
  }
  context_->End(query);
  context_->Flush();
  BOOL done = FALSE;
  while (context_->GetData(query, &done, sizeof done, 0) == S_FALSE) SwitchToThread();
  query->Release();
}

// --- textures ------------------------------------------------------------------------------------

struct Texture {
  TextureFormat format;
  int width = 0, height = 0;
  ID3D11Texture2D* texture = nullptr;  // the thread's device's
  ID3D11ShaderResourceView* view = nullptr;
  ID3D11Texture2D* angle = nullptr;    // the same, opened on ANGLE's device
};

namespace {

DXGI_FORMAT Dxgi(TextureFormat f) {
  switch (f) {
    case TextureFormat::kRgba8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::kRgba16f: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case TextureFormat::kRgb10a2: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case TextureFormat::kR8: return DXGI_FORMAT_R8_UNORM;
  }
  return DXGI_FORMAT_UNKNOWN;
}

void LogHr(const char* what, HRESULT hr) {
  std::fprintf(stderr, "gesher GPU thread: %s failed: 0x%08lx\n", what, static_cast<unsigned long>(hr));
  std::fflush(stderr);
}

}  // namespace

Texture* CreateTexture(TextureFormat format, int width, int height, const void* data, size_t row_bytes) {
  Thread* thread = Thread::Get();
  ID3D11Device* device = thread->device();
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = static_cast<UINT>(width);
  desc.Height = static_cast<UINT>(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = Dxgi(format);
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  D3D11_SUBRESOURCE_DATA initial = {data, static_cast<UINT>(row_bytes), 0};
  auto* t = new Texture();
  t->format = format;
  t->width = width;
  t->height = height;
  HRESULT hr = device->CreateTexture2D(&desc, data != nullptr ? &initial : nullptr, &t->texture);
  if (FAILED(hr)) {
    LogHr("CreateTexture2D", hr);
    delete t;
    return nullptr;
  }
  IDXGIResource* resource = nullptr;
  HANDLE shared = nullptr;
  hr = t->texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&resource));
  if (SUCCEEDED(hr)) {
    hr = resource->GetSharedHandle(&shared);
    resource->Release();
  }
  if (SUCCEEDED(hr)) {
    // ANGLE's device is free-threaded (no D3D11_CREATE_DEVICE_SINGLETHREADED).
    hr = thread->angle_device()->OpenSharedResource(shared, __uuidof(ID3D11Texture2D),
                                                      reinterpret_cast<void**>(&t->angle));
  }
  if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(t->texture, nullptr, &t->view);
  if (FAILED(hr)) {
    LogHr("sharing a texture with ANGLE's device", hr);
    DestroyTexture(t);
    return nullptr;
  }
  thread->Finish();  // the contents are there before ANGLE's device reads them
  return t;
}

void UpdateTexture(Texture* t, int x, int y, int width, int height, const void* data, size_t row_bytes) {
  Thread* thread = Thread::Get();
  D3D11_BOX box = {static_cast<UINT>(x), static_cast<UINT>(y), 0, static_cast<UINT>(x + width),
                   static_cast<UINT>(y + height), 1};
  thread->context()->UpdateSubresource(t->texture, 0, &box, data, static_cast<UINT>(row_bytes), 0);
  thread->Finish();
}

bool ReadTexture(Texture* t, int x, int y, int width, int height, void* out, size_t row_bytes) {
  Thread* thread = Thread::Get();
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = static_cast<UINT>(width);
  desc.Height = static_cast<UINT>(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = Dxgi(t->format);
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* staging = nullptr;
  HRESULT hr = thread->device()->CreateTexture2D(&desc, nullptr, &staging);
  if (FAILED(hr)) {
    LogHr("a staging texture", hr);
    return false;
  }
  D3D11_BOX box = {static_cast<UINT>(x), static_cast<UINT>(y), 0, static_cast<UINT>(x + width),
                   static_cast<UINT>(y + height), 1};
  ID3D11DeviceContext* context = thread->context();
  context->CopySubresourceRegion(staging, 0, 0, 0, 0, t->texture, 0, &box);
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    LogHr("mapping a staging texture", hr);
    staging->Release();
    return false;
  }
  const size_t bytes = std::min<size_t>(row_bytes, mapped.RowPitch);
  for (int row = 0; row < height; ++row) {
    std::memcpy(static_cast<uint8_t*>(out) + static_cast<size_t>(row) * row_bytes,
                static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch, bytes);
  }
  context->Unmap(staging, 0);
  staging->Release();
  return true;
}

void* AngleTexture(Texture* t) { return t->angle; }

void DestroyTexture(Texture* t) {
  if (t == nullptr) return;
  if (t->view != nullptr) t->view->Release();
  if (t->angle != nullptr) t->angle->Release();
  if (t->texture != nullptr) t->texture->Release();
  delete t;
}

// --- composition ---------------------------------------------------------------------------------

namespace {

struct Target {
  int width = 0, height = 0;
  ID3D11Texture2D* texture = nullptr;
  ID3D11RenderTargetView* view = nullptr;
  ID3D11Texture2D* staging = nullptr;

  void Release() {
    if (view != nullptr) view->Release();
    if (texture != nullptr) texture->Release();
    if (staging != nullptr) staging->Release();
    *this = Target();
  }
};

std::unordered_map<const void*, Target>& Targets() {  // GPU thread only
  static auto* map = new std::unordered_map<const void*, Target>();
  return *map;
}

bool EnsureTarget(Thread* thread, Target* t, int width, int height) {
  if (t->texture != nullptr && t->width == width && t->height == height) return true;
  t->Release();
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = static_cast<UINT>(width);
  desc.Height = static_cast<UINT>(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  HRESULT hr = thread->device()->CreateTexture2D(&desc, nullptr, &t->texture);
  if (SUCCEEDED(hr)) hr = thread->device()->CreateRenderTargetView(t->texture, nullptr, &t->view);
  if (SUCCEEDED(hr)) {
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = thread->device()->CreateTexture2D(&desc, nullptr, &t->staging);
  }
  if (FAILED(hr)) {
    LogHr("a composition target", hr);
    t->Release();
    return false;
  }
  t->width = width;
  t->height = height;
  return true;
}

}  // namespace

bool Compose(const void* key, int width, int height, const ComposeOp* ops, size_t count, uint8_t* out,
             int out_stride, int out_width, int out_height, bool bgra) {
  Thread* thread = Thread::Get();
  Target& t = Targets()[key];
  if (!EnsureTarget(thread, &t, width, height)) return false;
  ID3D11DeviceContext* c = thread->context();
  const Thread::Pipeline& p = thread->pipeline();
  const float clear[4] = {0, 0, 0, 0};
  c->ClearRenderTargetView(t.view, clear);
  c->OMSetRenderTargets(1, &t.view, nullptr);
  D3D11_VIEWPORT viewport = {0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
  c->RSSetViewports(1, &viewport);
  c->RSSetState(p.raster);
  c->IASetInputLayout(p.layout);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  const UINT stride = sizeof(Vertex), offset = 0;
  c->IASetVertexBuffers(0, 1, &p.vertices, &stride, &offset);
  c->VSSetShader(p.vertex_shader, nullptr, 0);
  c->VSSetConstantBuffers(0, 1, &p.constants);
  c->PSSetShader(p.pixel_shader, nullptr, 0);
  c->PSSetConstantBuffers(0, 1, &p.constants);
  c->PSSetSamplers(0, 1, &p.sampler);
  for (size_t i = 0; i < count; ++i) {
    const ComposeOp& op = ops[i];
    if (op.scissor[2] <= 0 || op.scissor[3] <= 0) continue;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(c->Map(p.vertices, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) continue;
    auto* v = static_cast<Vertex*>(mapped.pData);
    for (int k = 0; k < 4; ++k) v[k] = Vertex{op.quad[k][0], op.quad[k][1], op.quad[k][2], op.quad[k][3]};
    c->Unmap(p.vertices, 0);
    if (FAILED(c->Map(p.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) continue;
    Constants k = {};
    k.viewport[0] = static_cast<float>(width);
    k.viewport[1] = static_cast<float>(height);
    k.mode = op.texture != nullptr ? 0.0f : 1.0f;
    k.alpha = op.alpha;
    std::memcpy(k.color, op.color, sizeof k.color);
    k.opaque = op.opaque ? 1.0f : 0.0f;
    std::memcpy(mapped.pData, &k, sizeof k);
    c->Unmap(p.constants, 0);
    ID3D11ShaderResourceView* view = op.texture != nullptr ? op.texture->view : nullptr;
    c->PSSetShaderResources(0, 1, &view);
    c->OMSetBlendState(op.opaque && op.alpha >= 1.0f ? p.replace : p.premultiplied, nullptr, 0xffffffffu);
    D3D11_RECT scissor = {op.scissor[0], op.scissor[1], op.scissor[0] + op.scissor[2],
                          op.scissor[1] + op.scissor[3]};
    c->RSSetScissorRects(1, &scissor);
    c->Draw(4, 0);
  }
  ID3D11ShaderResourceView* none = nullptr;
  c->PSSetShaderResources(0, 1, &none);
  c->OMSetRenderTargets(0, nullptr, nullptr);
  c->CopyResource(t.staging, t.texture);
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(c->Map(t.staging, 0, D3D11_MAP_READ, 0, &mapped))) return false;
  const int rows = std::min(height, out_height), columns = std::min(width, out_width);
  for (int y = 0; y < rows; ++y) {
    const uint8_t* s = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
    uint8_t* d = out + static_cast<size_t>(y) * out_stride * 4;
    if (!bgra) {
      std::memcpy(d, s, static_cast<size_t>(columns) * 4);
      continue;
    }
    for (int x = 0; x < columns; ++x) {
      d[x * 4 + 0] = s[x * 4 + 2];
      d[x * 4 + 1] = s[x * 4 + 1];
      d[x * 4 + 2] = s[x * 4 + 0];
      d[x * 4 + 3] = s[x * 4 + 3];
    }
  }
  c->Unmap(t.staging, 0);
  // The next composition's read-back must not find the GPU asleep (gpu_keepalive.h).
  GpuKeepAliveAfterFrameOn(thread->device());
  return true;
}

void ForgetTarget(const void* key) {
  auto it = Targets().find(key);
  if (it == Targets().end()) return;
  it->second.Release();
  Targets().erase(it);
}

}  // namespace gesher::bionic::gpu
