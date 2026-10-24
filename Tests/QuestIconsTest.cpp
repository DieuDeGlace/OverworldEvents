#include "../src/QuestIcons.cpp"
#include <cassert>
#include <cstdio>

int main() {
    using namespace encounters::questicons;
    nameplate(123, 50, 120, Icon::Offer);
    auto overhead = takeFrame(false, true, GetTickCount64());
    assert(overhead.size() == 1 && overhead[0].x == 50 && overhead[0].y == 66);
    heads.clear();
    const Point destination{100, 40, .6f, Icon::Destination};
    // The small map and NPC markers share a frame. The full map hides NPC
    // markers but keeps objectives; leaving play clears both kinds.
    heads[1] = {{20, 30, .5f, Icon::Offer}, 150};
    map({destination});
    auto smallMap = takeFrame(false, true, 100);
    assert(smallMap.size() == 2 && smallMap[0].icon == Icon::Destination);
    assert(mapPoints.empty());
    map({destination});
    auto fullMap = takeFrame(true, true, 100);
    assert(fullMap.size() == 1 && fullMap[0].icon == Icon::Destination);
    assert(takeFrame(false, true, 150).empty() && heads.empty());
    heads[1] = {{20, 30, .5f, Icon::Offer}, 200};
    map({destination});
    assert(takeFrame(false, false, 100).empty());
    assert(heads.empty() && mapPoints.empty());
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &device, &level, &context)));
    Renderer test;
    assert(test.initialize(device.Get()));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 160;
    desc.Height = 80;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> image, staging;
    ComPtr<ID3D11RenderTargetView> target;
    assert(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &image)));
    assert(SUCCEEDED(device->CreateRenderTargetView(image.Get(), nullptr, &target)));
    const float background[] = {.2f, .1f, .7f, 1};
    context->ClearRenderTargetView(target.Get(), background);
    // Sentinels prove the whole game pipeline survives UI rendering.
    auto sentinel = target.Get();
    context->OMSetRenderTargets(1, &sentinel, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    const std::vector<Point> points = {
        {32, 68, 1, Icon::Event}, {88, 68, 1, Icon::Destination}, {132, 64, 1, Icon::Offer}};
    assert(test.draw(image.Get(), points));
    ComPtr<ID3D11RenderTargetView> restored;
    context->OMGetRenderTargets(1, &restored, nullptr);
    assert(restored.Get() == target.Get());
    D3D11_PRIMITIVE_TOPOLOGY topology;
    context->IAGetPrimitiveTopology(&topology);
    assert(topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
    context->CopyResource(staging.Get(), image.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    assert(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    auto color = [&](int x, int y) {
        return static_cast<unsigned char *>(mapped.pData) + y * mapped.RowPitch + x * 4;
    };
    unsigned orange = 0, grey = 0, white = 0, black = 0, leakedGreen = 0;
    for (int y = 0; y < 80; ++y)
        for (int x = 0; x < 160; ++x) {
            auto c = color(x, y);
            orange += c[0] > 200 && c[1] > 50 && c[1] < 200 && c[2] < 40;
            grey +=
                c[0] > 65 && c[0] < 200 && abs(int(c[0]) - c[1]) < 10 && abs(int(c[0]) - c[2]) < 10;
            white += c[0] > 240 && c[1] > 240 && c[2] > 240;
            black += c[0] < 15 && c[1] < 15 && c[2] < 15;
            leakedGreen += c[1] > c[0] * 1.3 && c[1] > c[2] * 1.3;
        }
    assert(orange > 100 && grey > 100 && white > 100 && black > 100 && !leakedGreen);
    // A hole in the question mark must expose the original background.
    auto hole = color(12, 60);
    assert(abs(int(hole[0]) - 51) <= 1 && abs(int(hole[2]) - 178) <= 1);
    context->Unmap(staging.Get(), 0);
    // Our parked state must hold no target; otherwise ResizeBuffers can fail.
    ComPtr<ID3DDeviceContextState> saved;
    test.context->SwapDeviceContextState(test.state.Get(), &saved);
    ComPtr<ID3D11RenderTargetView> parked;
    test.context->OMGetRenderTargets(1, &parked, nullptr);
    assert(!parked);
    test.context->SwapDeviceContextState(saved.Get(), nullptr);
    printf("Quest sprite colors, transparency, batching and pipeline restoration: PASS\n");
}
