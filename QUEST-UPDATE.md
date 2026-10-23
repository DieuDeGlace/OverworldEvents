# Quest dialogue and icons — October 23, 2026

A villager could display an offer marker based on an unused slot, then fall back to vanilla dialogue when no safe outside-town terrain was loaded. Offers now reserve dry procedural destinations and spawn on approach. Marker and talk share their readiness check. Accepted quests survive session reloads and retain the one-reward claim.

The supplied orange exclamation/orange question/grey question artwork replaces name punctuation and full-map fallback sprites. PC and mobile share gameplay, persistence, atlas and rendering code; executable-identity guards retain their platform-specific values.

Regression coverage includes state migration, pending quests and session reloads, invalid-state preservation, shoreline/slope rejection, bounded difficulty, sprite colors/transparency and full D3D11 state restoration (WARP), plus the existing encounter/dialogue/landmark tests.
