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
constexpr size_t Scale = 0x40;                        // float icon scale.
constexpr size_t Screen = 0x54;                       // float x, y, depth: where it is drawn.
constexpr ptrdiff_t VectorBegin = -0x48, VectorEnd = -0x40;

// Native projection of a map icon, from the treasure-icon push (0x2855b0-0x2859aa):
//   rel   = (dots - map center) * trunc(65536 / zoom) / 65536, then / 65536 as float
//   view  = Transform(matrices + 0x58, rel)     (behind the camera: not drawn)
//   ndc   = Transform(matrices + 0x18, view)    (outside [-1, 1]: not drawn)
//   x, y  = ndc * (+W/2, -H/2) + (W/2, H/2);   depth = ndc.z / 2
//   scale = clamp(64 / zoom * focus / 2.5, 0.66, 1); below 0.3 it is not drawn
// map object -> its sprite/camera holder. The native map reads every icon
// sprite from this table (treasure at +0x4a0); the native sprite-name loader
// fills it at the same offsets. The SDK's cube::Game::gui does not line up with
// it, so sprites are always taken from here.
constexpr size_t MapHolder = 0x1a8;
constexpr size_t MapScreen = 0x8;    // map object -> screen with width/height
constexpr size_t ScreenWidth = 0x1c4, ScreenHeight = 0x1c8;
constexpr size_t MapCenter = 0x1e20; // holder: int64 x, y, z in dots
constexpr size_t MapFocus = 0x1e50;  // holder: float
constexpr size_t Matrices = 0x10, ViewMatrix = 0x58, ProjectionMatrix = 0x18;
constexpr ptrdiff_t ZoomFrame = -0x78; // int, the map routine's zoom (r12 in the loop)
constexpr uintptr_t TransformRva = 0x2b680; // (matrix, out vec3, in vec3) -> out

// GUI sprite slots the map draws icons with (verified against the native
// sprite-name loader: enemy 0x348 ... demonportal 0x4b0, plus the location
// icons up to 0x5a8). A queued icon whose sprite is one of these proves the
// icon layout before anything is added.
constexpr size_t FirstMapSprite = 0x338, MapSpriteEnd = 0x5a8;
// Selectable marker sprites (GUI offsets): MapMarkerSprite in the ini.
struct NamedSprite {
    const wchar_t *name;
    size_t offset;
};
constexpr NamedSprite Sprites[] = {{L"missiontag", 0x2a0}, {L"destination", 0x458},
                                   {L"star", 0x388},       {L"cross", 0x390},
                                   {L"key", 0x498},        {L"treasure", 0x4a0}};

std::mutex gate;
std::vector<Marker> markers;
cube::Game *game = nullptr; // Set with the markers by the game tick.
std::atomic<bool> installed{false};
size_t spriteOffset = 0x2a0;
const char *spriteName = "missiontag";

void emptyText(unsigned char *text) {
    // MSVC std::wstring: 8-wchar inline buffer, size, capacity 7 when inline.
    std::memset(text, 0, 0x20);
    const uint64_t capacity = 7;
    std::memcpy(text + 0x18, &capacity, sizeof(capacity));
}
void *slot(const unsigned char *holder, size_t offset) {
    void *value;
    std::memcpy(&value, holder + offset, sizeof(value));
    return value;
}
bool mapSprite(const unsigned char *holder, const void *sprite) {
    if (!sprite)
        return false;
    for (size_t offset = FirstMapSprite; offset < MapSpriteEnd; offset += sizeof(void *))
        if (slot(holder, offset) == sprite)
            return true;
    return false;
}
// One log line whenever what the hook sees changes (render thread; rare).
void diagnose(int state, size_t icons, size_t added) {
    static std::atomic<int> last{-1};
    if (last.exchange(state) == state)
        return;
    static const char *text[] = {"no markers to draw", "marker sprite missing",
                                 "icon list rejected", "no native template icon in list",
                                 "markers added", "map sprite table mismatch",
                                 "markers outside the visible map"};
    char message[192];
    snprintf(message, sizeof(message), "Map icons: %s (native icons=%zu, added=%zu, sprite=%s).",
             text[state], icons, added, spriteName);
    FILE *file = nullptr;
    fopen_s(&file, "Mods/OverworldEvents.log", "a");
    if (file) {
        fprintf(file, "%s\n", message);
        fclose(file);
    }
}
template <class T> T read(const unsigned char *at) {
    T value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}
// Screen position, depth and scale of a world point, exactly as the native map
// computes them for its own icons. False when the point is not visible.
bool project(const unsigned char *map, const unsigned char *holder, int zoom,
             const Marker &marker, float screen[3], float &scale) {
    auto view = read<const unsigned char *>(map + MapScreen);
    auto matrices = read<unsigned char *>(holder + Matrices);
    if (!view || !matrices || zoom <= 0 || zoom > (1 << 20))
        return false;
    const int64_t world[3] = {int64_t(marker.x) * 65536 + 32768,
                              int64_t(marker.y) * 65536 + 32768, int64_t(marker.z) * 65536};
    auto factor = int64_t(1.0 / zoom * 65536.0);
    float relative[3];
    for (int i = 0; i < 3; ++i) {
        int64_t center = read<int64_t>(holder + MapCenter + 8 * i);
        relative[i] = float((world[i] - center) * factor / 65536) * (1.f / 65536.f);
    }
    using Transform = float *(*)(void *, float *, const float *);
    auto transform = reinterpret_cast<Transform>(native::base() + TransformRva);
    float eye[3], ndc[3];
    const float *p = transform(matrices + ViewMatrix, eye, relative);
    if (!p || !(p[2] > 0.f))
        return false;
    std::memcpy(eye, p, sizeof(eye));
    p = transform(matrices + ProjectionMatrix, ndc, eye);
    if (!p || p[0] < -1.f || p[0] > 1.f || p[1] < -1.f || p[1] > 1.f)
        return false;
    float width = float(read<int>(view + ScreenWidth)) * .5f,
          height = float(read<int>(view + ScreenHeight)) * .5f;
    screen[0] = p[0] * width + width;
    screen[1] = p[1] * -height + height;
    screen[2] = p[2] * .5f;
    float size = 64.f / float(zoom) * read<float>(holder + MapFocus) / 2.5f;
    if (size < .3f)
        return false;
    scale = std::min(1.f, std::max(.66f, size));
    return true;
}
// Called from the hook with the map routine's frame pointer and map object.
void draw(unsigned char *frame, unsigned char *map) {
    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock.owns_lock())
        return; // Never block the render path.
    if (markers.empty() || !game)
        return diagnose(0, 0, 0);
    auto holder = map ? read<unsigned char *>(map + MapHolder) : nullptr;
    if (holder && questicons::ready()) {
        std::vector<questicons::Point> points;
        for (const auto &marker : markers) {
            float screen[3], scale;
            if (project(map, holder, read<int>(frame + ZoomFrame), marker, screen, scale))
                points.push_back({screen[0], screen[1], scale * .6f, marker.icon});
        }
        questicons::map(points);
        return diagnose(points.empty() ? 6 : 4, 0, points.size());
    }
    void *marker = holder ? slot(holder, spriteOffset) : nullptr;
    if (!marker)
        return diagnose(1, 0, 0);
    unsigned char *begin, *end;
    std::memcpy(&begin, frame + VectorBegin, sizeof(begin));
    std::memcpy(&end, frame + VectorEnd, sizeof(end));
    if (!begin || end < begin || (end - begin) % IconSize || size_t(end - begin) / IconSize > 8192)
        return diagnose(2, 0, 0);
    size_t icons = size_t(end - begin) / IconSize;
    // Use a native icon of this frame as the template for flags; its sprite
    // must be one of the holder's map sprites, which validates the layout.
    const unsigned char *templateIcon = nullptr;
    for (auto icon = begin; icon < end && !templateIcon; icon += IconSize) {
        void *sprite;
        std::memcpy(&sprite, icon + Sprite, sizeof(sprite));
        if (mapSprite(holder, sprite))
            templateIcon = icon;
    }
    if (!templateIcon)
        return diagnose(3, icons, 0);
    int zoom = read<int>(frame + ZoomFrame);
    alignas(16) unsigned char icon[IconSize];
    std::memcpy(icon, templateIcon, IconSize);
    emptyText(icon + FirstText);
    emptyText(icon + SecondText);
    std::memset(icon + SpriteSpare, 0, 8);
    std::memset(icon + Tail, 0, 8);
    std::memcpy(icon + Sprite, &marker, sizeof(marker));
    const float yellow[4] = {1.f, .82f, .12f, 1.f};
    std::memcpy(icon + Color, yellow, sizeof(yellow));
    auto push = reinterpret_cast<void (*)(void *, const void *)>(native::base() + PushRva);
    size_t added = 0;
    for (const auto &marker : markers) {
        float screen[3], scale;
        if (!project(map, holder, zoom, marker, screen, scale))
            continue; // Off the visible map, as native icons are.
        const int32_t position[3] = {marker.x, marker.y, marker.z};
        std::memcpy(icon + Position, position, sizeof(position));
        std::memcpy(icon + Screen, screen, sizeof(screen));
        std::memcpy(icon + Scale, &scale, sizeof(scale));
        push(frame + VectorBegin, icon); // Copies; the inline empty strings own nothing.
        ++added;
    }
    diagnose(added ? 4 : 6, icons, added);
}

// Machine code: save volatile registers, call draw(rbp, r13), restore, then run the
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
    bytes({0x4c, 0x89, 0xea}); // mov rdx, r13 (the map object, kept by the routine)
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

bool initialize(const wchar_t *sprite, std::string *error) {
    if (installed)
        return true;
    for (const auto &entry : Sprites)
        if (sprite && _wcsicmp(sprite, entry.name) == 0) {
            spriteOffset = entry.offset;
            static char name[32];
            WideCharToMultiByte(CP_UTF8, 0, entry.name, -1, name, sizeof(name), nullptr, nullptr);
            spriteName = name;
        }
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
