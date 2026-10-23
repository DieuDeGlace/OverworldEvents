#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "QuestIcons.h"
#include "QuestAtlas.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace encounters::questicons {
namespace {
using Microsoft::WRL::ComPtr;
constexpr size_t MaxIcons = 256;
struct TimedPoint { Point point; ULONGLONG until; };
std::mutex gate;
std::unordered_map<uint64_t, TimedPoint> heads;
std::vector<Point> mapPoints;
std::atomic<bool> available{false};

// Exact crop bounds in the supplied 320x80 PNG, including the white outlines.
struct Crop { float x, y, width, height; };
constexpr Crop Crops[] = {{16, 16, 48, 52}, {96, 16, 48, 52}, {188, 16, 24, 48}};
struct Vertex { float x, y, u, v; };
constexpr char Shader[] = R"(
Texture2D atlas : register(t0);
SamplerState nearestPixel : register(s0);
struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };
V vs(float2 position : POSITION, float2 uv : TEXCOORD0) {
    V o; o.position = float4(position, 0, 1); o.uv = uv; return o;
}
float4 ps(V input) : SV_Target {
    float3 c = atlas.Sample(nearestPixel, input.uv).rgb;
    // The reference has a slightly noisy green background. Color-key only
    // that hue; preserve every pixel of the orange, grey and white artwork.
    clip(max(c.r, c.b) * 1.3 - c.g);
    return float4(c, 1);
}
)";

struct Renderer {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3DDeviceContextState> state;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vertices;
    ComPtr<ID3D11ShaderResourceView> atlasView;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11RasterizerState> raster;

    bool initialize(ID3D11Device *next) {
        device = next; // Remember a failed device; retry only after device recreation.
        ComPtr<ID3D11Device1> device1;
        ComPtr<ID3D11DeviceContext> immediate;
        if (FAILED(next->QueryInterface(IID_PPV_ARGS(&device1))))
            return false;
        next->GetImmediateContext(&immediate);
        if (FAILED(immediate.As(&context)))
            return false;
        D3D_FEATURE_LEVEL level = next->GetFeatureLevel(), selected;
        if (FAILED(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
                __uuidof(ID3D11Device), &selected, &state)))
            return false;
        ComPtr<ID3DBlob> vertexCode, pixelCode;
        if (FAILED(D3DCompile(Shader, sizeof(Shader) - 1, nullptr, nullptr, nullptr, "vs",
                "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vertexCode, nullptr)) ||
            FAILED(D3DCompile(Shader, sizeof(Shader) - 1, nullptr, nullptr, nullptr, "ps",
                "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pixelCode, nullptr)))
            return false;
        if (FAILED(next->CreateVertexShader(vertexCode->GetBufferPointer(),
                vertexCode->GetBufferSize(), nullptr, &vs)) ||
            FAILED(next->CreatePixelShader(pixelCode->GetBufferPointer(),
                pixelCode->GetBufferSize(), nullptr, &ps)))
            return false;
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        if (FAILED(next->CreateInputLayout(elements, 2, vertexCode->GetBufferPointer(),
                vertexCode->GetBufferSize(), &layout)))
            return false;
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = UINT(MaxIcons * 6 * sizeof(Vertex));
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(next->CreateBuffer(&buffer, nullptr, &vertices)))
            return false;
        std::vector<uint32_t> pixels;
        pixels.reserve(atlas::Width * atlas::Height);
        for (const auto &run : atlas::Pixels)
            pixels.insert(pixels.end(), run.count,
                          uint32_t(run.r) | uint32_t(run.g) << 8 | uint32_t(run.b) << 16 | 0xff000000u);
        if (pixels.size() != atlas::Width * atlas::Height)
            return false;
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = atlas::Width; texture.Height = atlas::Height;
        texture.MipLevels = texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_IMMUTABLE;
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA upload{pixels.data(), atlas::Width * 4, 0};
        ComPtr<ID3D11Texture2D> image;
        if (FAILED(next->CreateTexture2D(&texture, &upload, &image)) ||
            FAILED(next->CreateShaderResourceView(image.Get(), nullptr, &atlasView)))
            return false;
        D3D11_SAMPLER_DESC sampling{};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(next->CreateSamplerState(&sampling, &sampler)))
            return false;
        D3D11_BLEND_DESC blending{};
        blending.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(next->CreateBlendState(&blending, &blend)))
            return false;
        D3D11_DEPTH_STENCIL_DESC testing{};
        testing.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if (FAILED(next->CreateDepthStencilState(&testing, &depth)))
            return false;
        D3D11_RASTERIZER_DESC rasterizing{};
        rasterizing.FillMode = D3D11_FILL_SOLID;
        rasterizing.CullMode = D3D11_CULL_NONE;
        rasterizing.DepthClipEnable = TRUE;
        if (FAILED(next->CreateRasterizerState(&rasterizing, &raster)))
            return false;
        device = next;
        return true;
    }

    bool draw(ID3D11Texture2D *target, const std::vector<Point> &points) {
        if (points.empty())
            return true;
        D3D11_TEXTURE2D_DESC size{};
        target->GetDesc(&size);
        ComPtr<ID3D11RenderTargetView> output;
        if (!size.Width || !size.Height ||
            FAILED(device->CreateRenderTargetView(target, nullptr, &output)))
            return false;
        std::vector<Vertex> batch;
        batch.reserve(std::min(points.size(), MaxIcons) * 6);
        for (const auto &point : points) {
            if (batch.size() >= MaxIcons * 6 || unsigned(point.icon) >= std::size(Crops) ||
                !std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.scale))
                continue;
            auto crop = Crops[unsigned(point.icon)];
            float scale = std::clamp(point.scale, .25f, 2.f);
            float width = crop.width * scale, height = crop.height * scale;
            if (point.x < -width || point.x > size.Width + width ||
                point.y < 0 || point.y > size.Height + height)
                continue;
            // Coordinates anchor the bottom-center at the map location/head.
            float x0 = (point.x - width * .5f) * 2 / size.Width - 1;
            float x1 = (point.x + width * .5f) * 2 / size.Width - 1;
            float y0 = 1 - (point.y - height) * 2 / size.Height;
            float y1 = 1 - point.y * 2 / size.Height;
            float u0 = crop.x / atlas::Width, u1 = (crop.x + crop.width) / atlas::Width;
            float v0 = crop.y / atlas::Height, v1 = (crop.y + crop.height) / atlas::Height;
            const Vertex quad[] = {{x0,y0,u0,v0}, {x1,y0,u1,v0}, {x0,y1,u0,v1},
                                   {x0,y1,u0,v1}, {x1,y0,u1,v0}, {x1,y1,u1,v1}};
            batch.insert(batch.end(), std::begin(quad), std::end(quad));
        }
        if (batch.empty())
            return true;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
        std::memcpy(mapped.pData, batch.data(), batch.size() * sizeof(Vertex));
        context->Unmap(vertices.Get(), 0);
        ComPtr<ID3DDeviceContextState> saved;
        context->SwapDeviceContextState(state.Get(), &saved);
        auto rt = output.Get();
        context->OMSetRenderTargets(1, &rt, nullptr);
        context->OMSetBlendState(blend.Get(), nullptr, ~0u);
        context->OMSetDepthStencilState(depth.Get(), 0);
        context->RSSetState(raster.Get());
        D3D11_VIEWPORT viewport{0, 0, float(size.Width), float(size.Height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        auto vb = vertices.Get(); UINT stride = sizeof(Vertex), offset = 0;
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        auto texture = atlasView.Get(); auto nearest = sampler.Get();
        context->PSSetShaderResources(0, 1, &texture);
        context->PSSetSamplers(0, 1, &nearest);
        context->Draw(UINT(batch.size()), 0);
        // Release target/SRV bindings from our parked state before restoring the
        // game's entire pipeline. ResizeBuffers must not retain the backbuffer.
        context->ClearState();
        context->SwapDeviceContextState(saved.Get(), nullptr);
        return true;
    }
};
Renderer renderer;
} // namespace

void nameplate(uint64_t id, float x, float y, Icon icon) {
    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock.owns_lock() || !std::isfinite(x) || !std::isfinite(y))
        return;
    if (heads.size() >= MaxIcons && !heads.count(id))
        return;
    heads[id] = {{x, y - 22.f, .5f, icon}, GetTickCount64() + 150};
}
void map(const std::vector<Point> &points) {
    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (lock.owns_lock())
        mapPoints.assign(points.begin(), points.begin() + std::min(points.size(), MaxIcons));
}
bool ready() { return available.load(std::memory_order_acquire); }
void present(IDXGISwapChain *swapChain, bool mapOpen, bool playing) {
    if (!swapChain)
        return;
    ComPtr<ID3D11Device> device;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))))
        return;
    if (renderer.device.Get() != device.Get()) {
        available.store(false, std::memory_order_release);
        renderer = {};
        if (!renderer.initialize(device.Get()))
            return;
        available.store(true, std::memory_order_release);
    }
    if (!available.load(std::memory_order_acquire))
        return;
    std::vector<Point> points;
    {
        std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        auto now = GetTickCount64();
        for (auto it = heads.begin(); it != heads.end();) {
            if (!playing || now >= it->second.until)
                it = heads.erase(it);
            else {
                if (!mapOpen)
                    points.push_back(it->second.point);
                ++it;
            }
        }
        if (playing && mapOpen)
            points.swap(mapPoints);
        mapPoints.clear(); // Captured anew for every native map frame.
    }
    ComPtr<ID3D11Texture2D> target;
    if (!points.empty() && SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&target))))
        renderer.draw(target.Get(), points);
}
} // namespace encounters::questicons
