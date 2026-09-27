// Host check of DlssNr_AutoTrimDefault.h: the Automatic exposure defaults a game gets from the known-game list. No GPU
// and no game needed.
// cl /std:c++20 /EHsc tests/nr_auto_trim_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_AutoTrimDefault.h"

#include <cmath>
#include <cstdio>

using namespace DlssNrAutoTrim;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

int main()
{
    // Known unexposed games, by exe name: any case, with or without a path; anything else is not on the list.
    {
        CHECK(IsKnownUnexposedGame("RDR2.exe"));
        CHECK(IsKnownUnexposedGame("rdr2.exe"));
        CHECK(IsKnownUnexposedGame("PlayRDR2.exe"));
        CHECK(IsKnownUnexposedGame("J:\\Games\\Red Dead Redemption 2\\RDR2.exe"));
        CHECK(IsKnownUnexposedGame("C:/Games/RDR2/rdr2.exe"));
        CHECK(!IsKnownUnexposedGame("rdr.exe")); // Red Dead Redemption 1: not measured
        CHECK(!IsKnownUnexposedGame("nba2k27.exe"));
        CHECK(!IsKnownUnexposedGame("xrdr2.exe"));
        CHECK(!IsKnownUnexposedGame("rdr2.exe.bak"));
        CHECK(!IsKnownUnexposedGame(""));
    }
    // One default for every game: +1.5 EV on the menu's scale, EV = -log2(trim / 5).
    {
        CHECK(std::fabs(-std::log2(kDefaultTrim / 5.0f) - kDefaultEv) < 1e-4f);
        CHECK(std::fabs(kDefaultEv - 1.5f) < 1e-6f);
    }
    // The user's Trim always wins; without one the default applies.
    {
        CHECK(Effective(std::nullopt) == kDefaultTrim);
        CHECK(Effective(std::optional<float>(3.0f)) == 3.0f);
    }
    // Following the game's exposure: on by default only for a known unexposed game; the user's choice wins both ways.
    {
        CHECK(FollowGame(std::nullopt, true));
        CHECK(!FollowGame(std::nullopt, false));
        CHECK(FollowGame(std::optional<bool>(true), false));  // ticked by hand for an unlisted unexposed game
        CHECK(!FollowGame(std::optional<bool>(false), true)); // unticked in RDR2
    }

    printf(fails ? "FAILED: %d\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
