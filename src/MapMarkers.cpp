#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "MapMarkers.h"
#include "NativeWorld.h"
#include "cwsdk.h"
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace encounters::mapmarkers {
namespace {
// Inspected Steam map routine (RVA 0x2829c0). Every icon category has been
// pushed into the icon vector at rbp-0x48 by 0x285a35; it is then sorted
// (0x281ec0, 0x90-byte elements) and drawn. 0x285a35 is where the loop path
// and its skip branch (jg at 0x285508) meet; no branch enters the 14 bytes after.
constexpr uintptr_t HookRva = 0x285a35, ResumeRva = 0x285a43;
constexpr uintptr_t PushRva = 0x286b90; // push_back(vector *, const icon *)
// xor eax,eax; mov r10,[rbp-0x40]; mov [rbp+0x50],r10; mov rcx,[rbp-0x48]
constexpr unsigned char Original[14] = {0x33, 0xc0, 0x4c, 0x8b, 0x55, 0xc0, 0x4c,
                                        0x89, 0x55, 0x50, 0x48, 0x8b, 0x4d, 0xb8};
constexpr unsigned char PushEntry[6] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x30};

// Native icon layout, from the map routine's own push sites.
constexpr size_t IconSize = 0x90;
constexpr size_t FirstText = 0x00, SecondText = 0x20; // std::wstring, 0x20 bytes each.
constexpr size_t Color = 0x44;                        // RGBA floats.
constexpr size_t Position = 0x60;                     // int32 x, y, z in blocks.
constexpr size_t Sprite = 0x70;                       // plasma::Node *.
constexpr size_t SpriteSpare = 0x78, Tail = 0x88;     // Zeroed by every native push.
constexpr ptrdiff_t VectorBegin = -0x48, VectorEnd = -0x40;

std::mutex gate;
std::vector<Marker> markers;
cube::Game *game = nullptr; // Set with the markers by the game tick.
std::atomic<bool> installed{false};

void emptyText(unsigned char *text) {
    // MSVC std::wstring: 8-wchar inline buffer, size, capacity 7 when inline.
    std::memset(text, 0, 0x20);
    const uint64_t capacity = 7;
    std::memcpy(text + 0x18, &capacity, sizeof(capacity));
}
// The native sprites an icon in this list can carry; one proves the layout.
bool nativeSprite(const cube::GUI &gui, const void *sprite) {
    const void *known[] = {gui.enemy_node,       gui.monster_node,      gui.npc_node,
                           gui.object_node,      gui.star_node,         gui.cross_node,
                           gui.flight_point_node, gui.home_node,         gui.mapposition_node,
                           gui.direction_node,   gui.missiontag_white_node};
    for (auto node : known)
        if (node && node == sprite)
            return true;
    return false;
}
// Called from the hook with the map routine's frame pointer.
void draw(unsigned char *frame) {
    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock.owns_lock() || markers.empty())
        return; // Never block the render path.
    if (!game || !game->gui.missiontag_white_node)
        return;
    unsigned char *begin, *end;
    std::memcpy(&begin, frame + VectorBegin, sizeof(begin));
    std::memcpy(&end, frame + VectorEnd, sizeof(end));
    if (!begin || end < begin || (end - begin) % IconSize || size_t(end - begin) / IconSize > 8192)
        return;
    // Use a native icon of this frame as the template for scale, offsets and
    // flags; its sprite must be a known GUI node, which validates the layout.
    const unsigned char *templateIcon = nullptr;
    for (auto icon = begin; icon < end && !templateIcon; icon += IconSize) {
        void *sprite;
        std::memcpy(&sprite, icon + Sprite, sizeof(sprite));
        if (nativeSprite(game->gui, sprite))
            templateIcon = icon;
    }
    if (!templateIcon)
        return;
    alignas(16) unsigned char icon[IconSize];
    std::memcpy(icon, templateIcon, IconSize);
    emptyText(icon + FirstText);
    emptyText(icon + SecondText);
    std::memset(icon + SpriteSpare, 0, 8);
    std::memset(icon + Tail, 0, 8);
    void *sprite = game->gui.missiontag_white_node;
    std::memcpy(icon + Sprite, &sprite, sizeof(sprite));
    const float yellow[4] = {1.f, .82f, .12f, 1.f};
    std::memcpy(icon + Color, yellow, sizeof(yellow));
    auto push = reinterpret_cast<void (*)(void *, const void *)>(native::base() + PushRva);
    for (const auto &marker : markers) {
        const int32_t position[3] = {marker.x, marker.y, marker.z};
        std::memcpy(icon + Position, position, sizeof(position));
        push(frame + VectorBegin, icon); // Copies; the inline empty strings own nothing.
    }
}

// Machine code: save volatile registers, call draw(rbp), restore, then run the
// four displaced instructions (they re-read the possibly grown vector) and
// jump back.
bool buildStub(unsigned char *out, size_t &size) {
    std::vector<unsigned char> code;
    auto bytes = [&](std::initializer_list<unsigned char> list) {
        code.insert(code.end(), list.begin(), list.end());
    };
    auto imm64 = [&](uint64_t value) {
        for (int i = 0; i < 8; ++i)
            code.push_back((unsigned char)(value >> (8 * i)));
    };
    bytes({0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53}); // push rax..r11
    bytes({0x49, 0x89, 0xe3});                                                 // mov r11, rsp
    bytes({0x48, 0x83, 0xe4, 0xf0});                                           // and rsp, -16
    bytes({0x48, 0x81, 0xec, 0x90, 0x00, 0x00, 0x00});                         // sub rsp, 0x90
    bytes({0x4c, 0x89, 0x9c, 0x24, 0x80, 0x00, 0x00, 0x00}); // mov [rsp+0x80], r11
    const unsigned char save[6] = {0x44, 0x4c, 0x54, 0x5c, 0x64, 0x6c};
    for (int i = 0; i < 6; ++i) // movdqu [rsp+0x20+16i], xmm0..xmm5
        bytes({0xf3, 0x0f, 0x7f, save[i], 0x24, (unsigned char)(0x20 + 16 * i)});
    bytes({0x48, 0x89, 0xe9}); // mov rcx, rbp
    bytes({0x48, 0xb8});       // mov rax, draw
    imm64(uint64_t(&draw));
    bytes({0xff, 0xd0}); // call rax
    for (int i = 0; i < 6; ++i) // movdqu xmm0..xmm5, [rsp+0x20+16i]
        bytes({0xf3, 0x0f, 0x6f, save[i], 0x24, (unsigned char)(0x20 + 16 * i)});
    bytes({0x48, 0x8b, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); // mov rsp, [rsp+0x80]
    bytes({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x58}); // pop r11..rax
    code.insert(code.end(), Original, Original + sizeof(Original)); // Displaced code.
    bytes({0xff, 0x25, 0, 0, 0, 0});                                // jmp [rip]
    imm64(uint64_t(native::base() + ResumeRva));
    if (code.size() > size)
        return false;
    std::memcpy(out, code.data(), code.size());
    size = code.size();
    return true;
}
bool fail(std::string *error, const char *message) {
    if (error)
        *error = message;
    return false;
}
} // namespace

bool initialize(std::string *error) {
    if (installed)
        return true;
    if (!native::supported())
        return fail(error, "Map markers require the supported Steam executable.");
    auto site = reinterpret_cast<unsigned char *>(native::base() + HookRva);
    if (std::memcmp(site, Original, sizeof(Original)) != 0 ||
        std::memcmp(reinterpret_cast<void *>(native::base() + PushRva), PushEntry,
                    sizeof(PushEntry)) != 0)
        return fail(error, "Map marker hook site differs; native chest map icons kept.");
    auto stub = static_cast<unsigned char *>(
        VirtualAlloc(nullptr, 256, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    size_t size = 256;
    if (!stub || !buildStub(stub, size)) {
        if (stub)
            VirtualFree(stub, 0, MEM_RELEASE);
        return fail(error, "Map marker stub allocation failed.");
    }
    DWORD previous;
    if (!VirtualProtect(stub, 256, PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), stub, 256)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return fail(error, "Map marker stub protection failed.");
    }
    unsigned char patch[sizeof(Original)]; // jmp [rip]; dq stub - exactly 14 bytes.
    static_assert(sizeof(Original) == 14, "absolute jump size");
    const unsigned char jump[6] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(patch, jump, sizeof(jump));
    auto target = uint64_t(stub);
    std::memcpy(patch + 6, &target, sizeof(target));
    if (!VirtualProtect(site, sizeof(patch), PAGE_EXECUTE_READWRITE, &previous)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return fail(error, "Map marker hook protection failed.");
    }
    std::memcpy(site, patch, sizeof(patch));
    VirtualProtect(site, sizeof(patch), previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch));
    installed = true;
    return true;
}
bool active() {
    return installed;
}
void publish(cube::Game *current, const std::vector<Marker> &next) {
    std::lock_guard<std::mutex> lock(gate);
    game = current;
    markers = current ? next : std::vector<Marker>{};
}
} // namespace encounters::mapmarkers
