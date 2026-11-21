// gesher: the GPU thread's Direct3D 11 objects (ADR 0089; gpu_interop.h).
// For the files that draw or copy with them (gpu_interop.cc,
// hardware_buffer.cc, surface_control.cc).
#pragma once

#include "gpu_interop.h"

#include <d3d11.h>

namespace gesher::bionic::gpu {

// A vertex of a composed quad: target pixels (origin top left) and texture
// coordinates (origin top left: row 0 of a buffer is its top row).
struct Vertex {
  float x, y, u, v;
};

// The pixel shader's constants (a multiple of 16 bytes).
struct Constants {
  float viewport[2];
  float mode;    // 0: the texture, 1: color
  float alpha;   // plane alpha
  float color[4];  // premultiplied
  float opaque;  // alpha taken as 1
  float pad[3];
};

struct Thread::Pipeline {
  ID3D11VertexShader* vertex_shader = nullptr;
  ID3D11PixelShader* pixel_shader = nullptr;
  ID3D11InputLayout* layout = nullptr;
  ID3D11Buffer* vertices = nullptr;   // 4 vertices, dynamic
  ID3D11Buffer* constants = nullptr;  // Constants, dynamic
  ID3D11SamplerState* sampler = nullptr;
  ID3D11BlendState* premultiplied = nullptr;
  ID3D11BlendState* replace = nullptr;
  ID3D11RasterizerState* raster = nullptr;  // scissored, no culling
};

}  // namespace gesher::bionic::gpu
