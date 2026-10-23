#pragma once
#include <cstdint>
#include <dxgi.h>
#include <vector>

namespace encounters::questicons {
enum class Icon : unsigned { Event, Destination, Offer };
struct Point {
    float x, y, scale;
    Icon icon;
};
// The game supplies positions after its native map/nameplate projection.
// All rendering is a single batch at Present, outside native UI list ownership.
void nameplate(uint64_t id, float x, float y, Icon icon);
void map(const std::vector<Point> &points);
bool ready();
void present(IDXGISwapChain *swapChain, bool mapOpen, bool playing);
} // namespace encounters::questicons
