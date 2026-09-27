// Host check of DlssNrVitReuse.h: which launches of one NR evaluation are dropped when the ViT bottleneck is reused. No GPU and no game needed.
// cl /std:c++20 /EHsc tests/nr_vit_reuse_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNrVitReuse.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace DlssNrVitReuse;

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

using Seq = std::vector<Role>;
// what one evaluation launches: other work, the ViT range, other work
static const Seq kEval = { Role::None, Role::Start, Role::Inner, Role::Inner, Role::Inner, Role::End, Role::None };

struct Result
{
    std::vector<bool> dropped;
    bool wellFormed = true;
};

static Result Eval(Filter& f, const void* feature, bool reset, unsigned every, const Seq& seq = kEval, long long slot = -1,
                   KernelSet set = KernelSet::Fp8, unsigned everyPlain = 0)
{
    Result r;
    f.Begin(feature, reset, every, slot, everyPlain);
    for (Role role : seq)
        r.dropped.push_back(f.Drop(role, set));
    r.wellFormed = f.End();
    return r;
}

static bool Dropped(const Result& r) // the ViT range was dropped, everything else kept
{
    return r.dropped == std::vector<bool> { false, true, true, true, true, true, false };
}

static bool Kept(const Result& r)
{
    for (bool d : r.dropped)
        if (d)
            return false;
    return true;
}

int main()
{
    int a = 0, b = 0;

    // kernel names of both variants (fp8 chained and the plain fp16 ones) map to the range roles
    CHECK(RoleOf("cc_vit_1d_repack_2d_to_1d_fp8") == Role::Start);
    CHECK(RoleOf("cc_vit_1d_repack_2d_to_1d") == Role::Start);
    CHECK(RoleOf("cc_vit_1d_repack_1d_to_2d_fp8") == Role::End);
    CHECK(RoleOf("cc_vit_1d_repack_1d_to_2d") == Role::End);
    CHECK(RoleOf("cc_vit_1d_ffn_expand_publish_fp8") == Role::Inner);
    CHECK(RoleOf("cc_vit_1d_projection_wait_fp8") == Role::Inner);
    CHECK(RoleOf("cc_cb_clear") == Role::None);
    CHECK(RoleOf("cc_vit_ffn_expand") == Role::None); // the 2D ViT kernels are not the bottleneck range
    CHECK(RoleOf(nullptr) == Role::None);

    // the kernel set comes from the name: the fp8 kernels carry the _fp8 suffix
    CHECK(SetOf("cc_vit_1d_repack_2d_to_1d_fp8") == KernelSet::Fp8);
    CHECK(SetOf("cc_vit_1d_repack_2d_to_1d") == KernelSet::Plain);
    CHECK(SetOf("cc_vit_1d_fp8_projection") == KernelSet::Plain); // only a suffix counts
    CHECK(SetOf(nullptr) == KernelSet::Unknown);

    { // each kernel set follows its own rate: fp8 at 2, plain at 1
        Filter f;
        CHECK(f.LastSet() == KernelSet::Unknown);
        for (int i = 0; i < 4; ++i)
            CHECK(Kept(Eval(f, &a, false, 2, kEval, -1, KernelSet::Plain, 1)));
        CHECK(f.LastSet() == KernelSet::Plain);
        CHECK(Kept(Eval(f, &b, false, 2, kEval, -1, KernelSet::Fp8, 1)));
        CHECK(Dropped(Eval(f, &b, false, 2, kEval, -1, KernelSet::Fp8, 1)));
        CHECK(f.LastSet() == KernelSet::Fp8);
    }

    { // and the other way round: plain at 2 reuses, fp8 at 1 never does
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 1, kEval, -1, KernelSet::Plain, 2)));
        CHECK(Dropped(Eval(f, &a, false, 1, kEval, -1, KernelSet::Plain, 2)));
        CHECK(Kept(Eval(f, &b, false, 1, kEval, -1, KernelSet::Fp8, 2)));
        CHECK(Kept(Eval(f, &b, false, 1, kEval, -1, KernelSet::Fp8, 2)));
    }

    { // everyPlain 0 = the same rate as fp8 (callers that do not tell the sets apart)
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 2, kEval, -1, KernelSet::Plain)));
        CHECK(Dropped(Eval(f, &a, false, 2, kEval, -1, KernelSet::Plain)));
    }

    { // outside an evaluation nothing is touched, whatever the setting
        Filter f;
        CHECK(!f.Evaluating());
        CHECK(!f.Drop(Role::Start) && !f.Drop(Role::Inner) && !f.Drop(Role::End));
    }

    { // N = 1 never drops but keeps the cache warm
        Filter f;
        for (int i = 0; i < 4; ++i)
            CHECK(Kept(Eval(f, &a, false, 1)));
        CHECK(f.Computed() == 4 && f.Reused() == 0);
        CHECK(Dropped(Eval(f, &a, false, 2))); // raising N takes effect on the next evaluation, no warm-up
    }

    { // N = 2: full, reuse, full, reuse
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 2)));
        CHECK(Dropped(Eval(f, &a, false, 2)));
        CHECK(Kept(Eval(f, &a, false, 2)));
        CHECK(Dropped(Eval(f, &a, false, 2)));
        CHECK(f.Computed() == 2 && f.Reused() == 2 && !f.Disabled());
    }

    { // N = 3: full, reuse, reuse, full
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 3)));
        CHECK(Dropped(Eval(f, &a, false, 3)));
        CHECK(Dropped(Eval(f, &a, false, 3)));
        CHECK(Kept(Eval(f, &a, false, 3)));
        CHECK(Dropped(Eval(f, &a, false, 3)));
    }

    { // lowering N mid-cycle computes at once
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 3)));
        CHECK(Dropped(Eval(f, &a, false, 3)));
        CHECK(Kept(Eval(f, &a, false, 1)));
        CHECK(Kept(Eval(f, &a, false, 1)));
    }

    { // a reset always computes in full, and restarts the cycle
        Filter f;
        Eval(f, &a, false, 2);
        CHECK(Kept(Eval(f, &a, true, 2)));
        CHECK(Dropped(Eval(f, &a, false, 2)));
    }

    { // each feature (each pass) has its own cache and cycle
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 2)));
        CHECK(Kept(Eval(f, &b, false, 2))); // b has never been computed
        CHECK(Dropped(Eval(f, &a, false, 2)));
        CHECK(Dropped(Eval(f, &b, false, 2)));
        CHECK(Kept(Eval(f, &a, false, 2)));
    }

    { // passes share the frame's slot, N = 2, 3 passes: all compute on even frames, all reuse on odd ones
        Filter f;
        int p[3];
        for (int frame = 0; frame < 9; ++frame)
        {
            int computed = 0;
            for (int pass = 0; pass < 3; ++pass)
                computed += Kept(Eval(f, &p[pass], false, 2, kEval, frame));
            CHECK(computed == (frame % 2 ? 0 : 3));
        }
    }

    { // a pass that starts over on an off frame (reset of that pass alone) is back in the frame's phase on the next frame
        Filter f;
        int p[3];
        std::vector<std::string> seen;
        for (int frame = 0; frame < 8; ++frame)
        {
            std::string s;
            for (int pass = 0; pass < 3; ++pass)
                s += Kept(Eval(f, &p[pass], pass == 1 && frame == 5, 2, kEval, frame)) ? 'V' : '-';
            seen.push_back(s);
        }
        CHECK(seen[4] == "VVV" && seen[5] == "-V-" && seen[6] == "VVV" && seen[7] == "---");
    }

    { // anchored or not, a pass never waits longer than N - 1 reused evaluations, for N = 2 and 3, any phase, with a reset
        for (unsigned every = 2; every <= 3; ++every)
            for (int pass = -1; pass < 4; ++pass)
            {
                Filter f;
                int run = 0, worst = 0, computed = 0;
                for (int frame = 0; frame < 12; ++frame)
                {
                    if (Kept(Eval(f, &a, frame == 5, every, kEval, pass < 0 ? -1 : frame + pass)))
                        run = 0, ++computed;
                    else
                        worst = std::max(worst, ++run);
                }
                CHECK(worst <= (int) every - 1);
                CHECK(computed <= 12 / (int) every + 2); // still reuses: at most the first frame and the reset extra
            }
    }

    { // destroying the modules invalidates every cache
        Filter f;
        Eval(f, &a, false, 2);
        f.Clear();
        CHECK(Kept(Eval(f, &a, false, 2)));
        CHECK(Dropped(Eval(f, &a, false, 2)));
    }

    { // a range that is never closed: the feature turns itself off
        Filter f;
        Eval(f, &a, false, 2);
        auto r = Eval(f, &a, false, 2, { Role::None, Role::Start, Role::Inner });
        CHECK(!r.wellFormed && f.Disabled());
        CHECK(Kept(Eval(f, &a, false, 2)));
        CHECK(Kept(Eval(f, &a, false, 2)));
    }

    { // an unclosed full range is not a valid cache
        Filter f;
        f.Begin(&a, false, 2);
        f.Drop(Role::Start);
        CHECK(!f.End());
        f.Clear();
    }

    { // a foreign launch inside a skipped range is kept, the rest of the range is still dropped, then the feature is off
        Filter f;
        Eval(f, &a, false, 2);
        auto r = Eval(f, &a, false, 2, { Role::Start, Role::Inner, Role::None, Role::Inner, Role::End });
        CHECK((r.dropped == std::vector<bool> { true, true, false, true, true }));
        CHECK(!r.wellFormed && f.Disabled());
        CHECK(Kept(Eval(f, &a, false, 2)));
    }

    { // End without Start
        Filter f;
        auto r = Eval(f, &a, false, 2, { Role::None, Role::End });
        CHECK((r.dropped == std::vector<bool> { false, false }) && !r.wellFormed && f.Disabled());
    }

    { // a second Start inside a range
        Filter f;
        auto r = Eval(f, &a, false, 2, { Role::Start, Role::Start, Role::End });
        CHECK(!r.wellFormed && f.Disabled());
    }

    { // the evaluation context belongs to its thread: a launch from another thread is not part of it
        Filter f;
        Eval(f, &a, false, 2);
        f.Begin(&a, false, 2);
        bool other = true;
        std::thread([&] { other = f.Evaluating() || f.Drop(Role::Start); }).join();
        CHECK(!other);
        f.End();
    }

    printf(fails ? "nr_vit_reuse_smoke: %d FAILED\n" : "nr_vit_reuse_smoke: all passed\n", fails);
    return fails != 0;
}
