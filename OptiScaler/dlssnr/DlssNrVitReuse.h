#pragma once

// Reuse of the ViT bottleneck of NVIDIA's own DLSS-NR model.
//
// One NR evaluation launches its kernels on the game's command list through NvAPI. The ViT bottleneck (blocks 31-38, the
// coarsest and most stable level of the network) is one contiguous run of those launches, from cc_vit_1d_repack_2d_to_1d
// through cc_vit_1d_repack_1d_to_2d, and about a fifth of the evaluation. Leaving that run out on some frames leaves the
// previous frame's result in its output buffer, which the next kernel reads.
//
// Why dropping the run is safe (checked on a real capture, fp8 kernels): the run's sync counters are referenced by no kernel outside
// it, so nothing that is kept can wait on something that was dropped; and its output buffer is written only by its last kernel.
// Anything that does not look like that turns the feature off for the session instead of guessing.
// The caller records a UAV barrier where a skipped run ends, so the kernels on either side of the gap cannot overlap on the GPU.
//
// This header is only the decision: which launch of an evaluation is dropped. It knows nothing about NvAPI, so tests/nr_vit_reuse_smoke.cpp
// exercises it on the host.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace DlssNrVitReuse
{
enum class Role : uint8_t
{
    None = 0, // any other kernel
    Start,    // first kernel of the ViT run
    Inner,    // any other kernel of the ViT run
    End,      // last kernel of the ViT run
};

// Names of both kernel variants (the fp8 ones carry a _fp8 suffix), so the plain fp16 kernels match too.
inline Role RoleOf(const char* name)
{
    constexpr char prefix[] = "cc_vit_1d_";
    constexpr size_t prefixLen = sizeof(prefix) - 1;

    if (name == nullptr || strncmp(name, prefix, prefixLen) != 0)
        return Role::None;

    const char* rest = name + prefixLen;

    if (strncmp(rest, "repack_2d_to_1d", 15) == 0)
        return Role::Start;

    if (strncmp(rest, "repack_1d_to_2d", 15) == 0)
        return Role::End;

    return Role::Inner;
}

// Which kernel set a kernel belongs to: the fp8 ones carry the _fp8 suffix, the plain fp16 ones (used by some modified DLSS-NR DLLs) do not.
// Each set has its own Reuse setting.
enum class KernelSet : uint8_t
{
    Unknown = 0, // no ViT run seen yet
    Fp8,
    Plain,
};

inline KernelSet SetOf(const char* name)
{
    if (name == nullptr)
        return KernelSet::Unknown;

    constexpr char suffix[] = "_fp8";
    constexpr size_t suffixLen = sizeof(suffix) - 1;
    const size_t len = strlen(name);
    return len >= suffixLen && strcmp(name + len - suffixLen, suffix) == 0 ? KernelSet::Fp8 : KernelSet::Plain;
}

// Feed it the launches of one model evaluation, in order, between Begin and End. One Filter serves every feature (every
// pass); the evaluation in progress belongs to the calling thread, so launches from any other thread pass untouched.
class Filter
{
  public:
    // Start of one model evaluation of `feature`. `every` is how often the run is computed (1 = always) for the fp8 kernel set,
    // `everyPlain` for the plain fp16 one (0 = the same as `every`); the run's first launch tells which set it is. `reset` forces it.
    // `slot` (the frame number; negative = none) anchors the cycle to the frame: a pass computes when slot % every == 0.
    // Every pass of a frame gets the same slot, so all passes compute on the same frame and all reuse on the next. Offsetting
    // each pass by its index (v0.1.19 pre-release) flickered on camera motion at 3 passes: a pass then built its bottleneck
    // on a pass that was reusing, so the picture used data two frames old, like every = 3.
    // The cycle is anchored to the frame, so a pass that had to start over (reset, lost cache) falls back into its own
    // phase on the next due frame. A pass never goes longer than every - 1 reused evaluations, anchored or not.
    void Begin(const void* feature, bool reset, unsigned every, long long slot = -1, unsigned everyPlain = 0)
    {
        Ctx& c = ctx();
        c = Ctx {};
        c.active = true;
        c.key = feature;
        c.reset = reset;
        c.every = every < 1 ? 1 : every;
        c.everyPlain = everyPlain < 1 ? c.every : everyPlain;
        c.slot = slot;
    }

    bool Evaluating() const { return ctx().active; }

    // True when this launch is to be dropped. Must be called for every launch of the evaluation, in order. `set` is the launch's
    // kernel set; only the run's first launch uses it.
    bool Drop(Role role, KernelSet set = KernelSet::Fp8)
    {
        Ctx& c = ctx();

        if (!c.active)
            return false;

        switch (role)
        {
        case Role::Start:
        {
            if (c.inRange) // a second start: not the order the checks were made for
            {
                c.anomaly = true;
                return c.skipping;
            }

            c.inRange = true;
            lastSet_ = set;

            const unsigned every = set == KernelSet::Plain ? c.everyPlain : c.every;
            std::lock_guard<std::mutex> lock(mutex_);
            Entry& e = entries_[c.key];
            const bool due = c.slot >= 0 && c.slot % every == 0;
            const bool skip = !disabled_ && every > 1 && e.valid && !c.reset && !due && e.skips + 1 < every;

            if (skip)
            {
                ++e.skips;
                c.skipping = true;
                return true;
            }

            // computed in full: the cache is valid again only once the run has been seen to the end
            e.skips = 0;
            e.valid = false;
            return false;
        }
        case Role::Inner:
            return c.inRange && c.skipping;

        case Role::End:
        {
            if (!c.inRange)
            {
                c.anomaly = true;
                return false;
            }

            c.inRange = false;

            if (c.skipping)
            {
                c.skipping = false;
                ++reused_;
                return true;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            entries_[c.key].valid = true;
            ++computed_;
            return false;
        }
        default:
            // Another kernel in the middle of a skipped run. It is kept (it is not ours to remove), the rest of the run is
            // still dropped so nothing waits on a half-dropped chain, and the feature goes off afterwards.
            if (c.skipping)
                c.anomaly = true;

            return false;
        }
    }

    // End of the evaluation. False when what was launched did not look like the expected order; the feature is then off
    // for the rest of the session.
    bool End()
    {
        Ctx& c = ctx();

        if (!c.active)
            return true;

        if (c.inRange)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            entries_[c.key].valid = false;
        }

        const bool wellFormed = !c.inRange && !c.anomaly;

        if (!wellFormed)
            disabled_ = true;

        c = Ctx {};
        return wellFormed;
    }

    // The modules were destroyed: no cached result can be trusted any more.
    void Clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

    bool Disabled() const { return disabled_; }
    KernelSet LastSet() const { return lastSet_; } // the kernel set of the last ViT run seen
    unsigned long long Computed() const { return computed_; }
    unsigned long long Reused() const { return reused_; }

    std::string Status() const
    {
        char buf[128];

        if (disabled_)
            return "off for this session (unexpected NR launch order)";

        snprintf(buf, sizeof buf, "computed %llu, reused %llu", (unsigned long long) computed_,
                 (unsigned long long) reused_);
        return buf;
    }

  private:
    struct Entry
    {
        bool valid = false;  // the output buffer holds a complete result
        unsigned skips = 0;  // consecutive evaluations that reused it
    };

    struct Ctx
    {
        bool active = false;
        const void* key = nullptr;
        bool reset = false;
        unsigned every = 1;
        unsigned everyPlain = 1;
        long long slot = -1;
        bool inRange = false;
        bool skipping = false;
        bool anomaly = false;
    };

    static Ctx& ctx()
    {
        static thread_local Ctx c;
        return c;
    }

    std::mutex mutex_;
    std::unordered_map<const void*, Entry> entries_;
    std::atomic<bool> disabled_ { false };
    std::atomic<KernelSet> lastSet_ { KernelSet::Unknown };
    std::atomic<unsigned long long> computed_ { 0 };
    std::atomic<unsigned long long> reused_ { 0 };
};
} // namespace DlssNrVitReuse
