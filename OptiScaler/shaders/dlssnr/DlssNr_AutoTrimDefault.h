#pragma once

// The Automatic exposure Trim every game gets while the user has not set one (ini AutoExposureTrim = auto), and
// whether it follows the game's own exposure by default.
//
// The Trim default is one value for every game, +1.5 EV on the menu's scale (user decision 2026-09-26, for simplicity).
// Measured before (FrameStats, model-input median): RDR2 0.48 at +2.3 EV and 0.77-0.80 at +4.3 EV; NBA 2K27 0.41 at
// +2.3 EV and yellow highlights in its player shadows at +4.3 EV. The slider sets any game's own value.
//
// Following the game's exposure is right only for a game that hands over its frame before applying its own exposure
// (unexposed: RDR2's raw scene luminance, the game's exposure of ~0.006-0.013 comes later); for the usual pre-exposed
// frame (NBA 2K27, Cyberpunk 2077, The Witcher 3) it would apply the game's exposure a second time. Which kind a game is
// cannot be read off the frame: a dark unexposed scene has the numbers of a pre-exposed one (RDR2 read 12-116 in dim
// scenes on 2026-09-26, 900-1700 in daylight), and the DLSS inputs are the same (NBA passes an exposure texture,
// PreExposure 1 and the flag like RDR2). So the games measured to be unexposed are listed by exe and follow by default;
// the user's Follow checkbox overrides it both ways.
//
// Header-only and free of D3D/Vulkan types so it can be exercised on the host (tests/nr_auto_trim_smoke.cpp).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>

namespace DlssNrAutoTrim
{
constexpr float kDefaultEv = 1.5f;        // on the menu's scale: EV = -log2(trim / 5), 0 EV = Trim 5
constexpr float kDefaultTrim = 1.767767f; // 5 * 2^-1.5

// Games measured to hand over unexposed frames, by exe name (lower case). Kept here rather than as a GameQuirk in
// misc/Quirks.h: upstream OptiScaler owns those lines (rdr2.exe already has compatibility quirks there), and a DLSS-NR
// flag on them would conflict on every sync. Add a game only with a FrameStats measurement behind it.
constexpr const char* kUnexposedGames[] = {
    "rdr2.exe",     // Red Dead Redemption 2: scene median 150-350 in daylight, game exposure ~0.006-0.013 (2026-09-25)
    "playrdr2.exe", // its launcher-started exe
};

// Whether the game's exe (name or full path, any case) is on kUnexposedGames.
inline bool IsKnownUnexposedGame(const std::string& exe)
{
    const size_t slash = exe.find_last_of("\\/");
    std::string name = slash == std::string::npos ? exe : exe.substr(slash + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char) std::tolower(c); });

    for (const char* known : kUnexposedGames)
        if (name == known)
            return true;

    return false;
}

// The Trim in force: the user's own when they set one, otherwise the default.
inline float Effective(const std::optional<float>& userTrim) { return userTrim.has_value() ? *userTrim : kDefaultTrim; }

// Whether Automatic follows the game's exposure: the user's choice when they made one, otherwise on for a known
// unexposed game only.
inline bool FollowGame(const std::optional<bool>& userFollow, bool knownUnexposed)
{
    return userFollow.has_value() ? *userFollow : knownUnexposed;
}
} // namespace DlssNrAutoTrim
