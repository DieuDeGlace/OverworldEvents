#include "../src/EventHealthRules.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
bool near(float actual, float expected) {
    return std::fabs(actual - expected) <= std::max(1.f, std::fabs(expected)) * 1e-5f;
}
} // namespace

int main() {
    using namespace encounters::health;
    constexpr unsigned combinations[] = {
        0, Named, Mini, Boss, Named | Mini, Named | Boss, Mini | Boss, Named | Mini | Boss};
    constexpr float ordinary = 120.f;
    for (unsigned flags : combinations) {
        float native = ordinary * float(nativeBossMultiplier(flags));
        assert(near(normalizedMaximumHP(native, flags, true), ordinary * 3.f));
        assert(normalizedMaximumHP(native, flags, false) == native);
        assert(near(normalizedMaximumHP(native, flags | (1u << 15), true), ordinary * 3.f));
    }
    assert(nativeBossMultiplier(Named | Boss) == 150.0);
    assert(near(normalizedMaximumHP(150.f, Named | Boss, true), 3.f));
    assert(normalizedMaximumHP(ordinary, 0, false) == ordinary);

    // Pyro's numeric-level multiplier and CubeMega's beginner multiplier may
    // run before or after this callback; their combined effect must survive.
    for (float pyro : {.1f, .73f, 2.f, 19.f}) {
        for (float beginner : {1.f / 3.f, .5f, 1.f}) {
            float native = ordinary * 150.f;
            float first = normalizedMaximumHP(native, Named | Boss, true) * pyro * beginner;
            float last = normalizedMaximumHP(native * pyro * beginner, Named | Boss, true);
            float middle = normalizedMaximumHP(native * pyro, Named | Boss, true) * beginner;
            assert(near(first, ordinary * 3.f * pyro * beginner));
            assert(near(first, last));
            assert(near(first, middle));
        }
    }

    constexpr float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (float invalid : {0.f, -1.f, -infinity, infinity, nan}) {
        float result = normalizedMaximumHP(invalid, Named | Boss, true);
        assert(std::isfinite(result) && result > 0.f && result == 1.f);
    }
    // The helper cannot sanitize unrelated creatures without changing them.
    assert(normalizedMaximumHP(-1.f, 0, false) == -1.f);
    assert(std::isnan(normalizedMaximumHP(nan, 0, false)));
    float huge = normalizedMaximumHP(std::numeric_limits<float>::max(), 0, true);
    assert(std::isfinite(huge) && huge == std::numeric_limits<float>::max());
    float tiny =
        normalizedMaximumHP(std::numeric_limits<float>::denorm_min(), Named | Mini | Boss, true);
    assert(std::isfinite(tiny) && tiny > 0.f);
    std::puts("Event guardian maximum-HP normalization passed");
}
