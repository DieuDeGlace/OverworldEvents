#include "../src/DialogueRules.h"
#include <cassert>
#include <cstdio>
#include <cwchar>
#include <set>
#include <string>

using namespace encounters;
using namespace encounters::dialogue;

// Number of printf conversions in a line ("%%" is not one).
static int conversions(const wchar_t *text) {
    int count = 0;
    for (const wchar_t *c = text; *c; ++c)
        if (*c == L'%') {
            if (c[1] == L'%') {
                ++c;
                continue;
            }
            ++count;
        }
    return count;
}

int main() {
    const Kind kinds[] = {Kind::Battle, Kind::Rescue, Kind::Ambush};
    for (Kind kind : kinds)
        for (Topic topic : {Topic::Offer, Topic::Reminder, Topic::Reward, Topic::Thanks, Topic::Over}) {
            const auto &lines = pool(topic, kind);
            std::set<std::wstring> distinct;
            for (auto text : lines) {
                assert(text && *text);
                distinct.insert(text);
                bool distance = topic == Topic::Offer || topic == Topic::Reminder;
                // Offer/reminder lines take exactly one distance (%.0f); others none.
                assert(conversions(text) == (distance ? 1 : 0));
                if (distance)
                    assert(wcsstr(text, L"%.0f"));
                // Formatted with a large distance, every line fits the message buffer
                // and stays short enough for the speech bubble.
                wchar_t formatted[320];
                int length = distance ? swprintf(formatted, 320, text, 123456.0)
                                      : swprintf(formatted, 320, L"%ls", text);
                assert(length > 0 && length < 220);
            }
            assert(distinct.size() == Lines); // five different lines
        }
    // Offers differ between event kinds.
    assert(std::wstring(offers(Kind::Battle)[0]) != offers(Kind::Rescue)[0]);
    assert(std::wstring(offers(Kind::Rescue)[0]) != offers(Kind::Ambush)[0]);

    // Random choice covers all five lines and never repeats a line back to back
    // for the same speaker, topic and kind.
    Picker picker;
    std::set<std::size_t> seen;
    std::size_t previous = Lines;
    uint64_t random = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 1000; ++i) {
        random = mix(random + i);
        auto index = picker.pick(42, Topic::Offer, Kind::Rescue, random);
        assert(index < Lines && index != previous);
        seen.insert(index);
        previous = index;
    }
    assert(seen.size() == Lines);
    // The same random value repeated is still not repeated back to back.
    Picker fixed;
    auto first = fixed.pick(7, Topic::Thanks, Kind::Battle, 3);
    auto second = fixed.pick(7, Topic::Thanks, Kind::Battle, 3);
    assert(first != second);
    // Different speakers keep separate histories.
    assert(fixed.pick(8, Topic::Thanks, Kind::Battle, 3) == 3);
    puts("Dialogue rules PASS");
}
