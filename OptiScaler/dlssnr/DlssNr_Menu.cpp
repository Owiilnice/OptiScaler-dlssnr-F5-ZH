#include "pch.h"
#include "DlssNrFeature_Vk.h"

#include "DlssNr.h"
#include "DlssNr_ExposureScan.h"
#include "DlssNrNative.h"


#include <Config.h>
#include <menu/menu_common.h>

#include <imgui/imgui.h>
#include <shaders/dlssnr/DlssNr_TrimAnchors.h>
#include <shaders/dlssnr/DlssNr_AutoTrimDefault.h>
#include <shaders/dlssnr/DlssNr_FollowGame.h>
#include "DlssNr_GameDefaults.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace DlssNr
{

// Highlight guard's own ceiling -- independent of the model pass limit, which is an
// unrelated setting that happens to share this file.
static constexpr float MaxHighlightGuard = 8.0f;

// Whether this game has ever offered a Game-exposure value. Shared by the White-point-source
// panel's own "No game exposure available" readout and the Optimized Defaults preset, so the
// two stay in agreement if this definition ever changes (e.g. gains a staleness check).
static bool HaveGameExposure()
{
    return DlssNr::IsRunningVk() ? DlssNr::ExposureOfferedVk()
                                  : DlssNr::GameExposureStatus().everOffered;
}

static void HelpMarker(const char* tip);

// Trim multiplies the white point, so a larger Trim darkens the picture NR is shown. The menu shows it in stops
// instead, the other way round (+ = brighter) and centred on each source's own default, which reads as 0 EV.
static float TrimToEv(float trim, float neutral)
{
    // "+ 0.0f" turns the -0.0 that neutral gives into 0.0, so the slider reads "+0.0 EV", not "-0.0 EV".
    return -std::log2(std::max(trim, DlssNrTrim::kMinTrim) / neutral) + 0.0f;
}

static float EvToTrim(float ev, float neutral)
{
    return DlssNrTrim::ClampTrim(neutral * std::exp2(-ev));
}

// The one "Model input brightness" slider (and its Reset) for an exposure source's Trim. `anchorCount` is how
// many Trim anchors the ini holds for that source: they are ini-only now and take over from the slider, so
// with any present the slider is shown disabled and says why. `detectedDefault` is set for a source whose
// default is decided per game (Automatic, see DlssNr_AutoTrimDefault.h): the slider then shows it while the
// user has set nothing, and Reset goes back to it (the ini key back to auto) rather than to `neutral`.
static void RenderTrimEvSlider(CustomOptional<float>& trim, float neutral, size_t anchorCount, const char* idSuffix,
                               const char* tip, std::optional<float> detectedDefault = std::nullopt)
{
    const float minEv = TrimToEv(DlssNrTrim::kMaxTrim, neutral);
    const float maxEv = TrimToEv(DlssNrTrim::kMinTrim, neutral);
    const float shown = trim.has_value() ? trim.value() : detectedDefault.value_or(trim.value_or_default());
    float ev = std::clamp(TrimToEv(shown, neutral), minEv, maxEv);

    ImGui::BeginDisabled(anchorCount > 0);
    const std::string sliderLabel = std::string("Model input brightness##") + idSuffix;
    if (ImGui::SliderFloat(sliderLabel.c_str(), &ev, minEv, maxEv, "%+.1f EV"))
        trim = EvToTrim(ev, neutral);

    ImGui::SameLine();

    // Deliberately always present rather than greyed at 0 EV: the safe value is one click away.
    const std::string resetLabel = std::string("Reset##") + idSuffix;
    if (ImGui::SmallButton(resetLabel.c_str()))
    {
        if (detectedDefault.has_value())
            trim = std::nullopt;
        else
            trim = neutral;
    }
    ImGui::EndDisabled();

    HelpMarker(tip);

    if (anchorCount > 0)
        ImGui::TextDisabled("%u brightness anchor point(s) from the ini are in use; the slider has no effect while they exist.",
                            (unsigned int) anchorCount);
}

// The "(?)" marker every control carries, matching the rest of the menu.
static void HelpMarker(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");

    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// A slider that only writes its value when the handle is released.
//
// Some controls -- intensity, the structure and tone strengths -- are read by the model once, when
// the feature is built, so changing one rebuilds the whole feature. Writing on every pixel of a drag
// meant a rebuild per frame, felt as the picture hitching while you scrub. The slider still tracks
// live under the cursor; only the commit that triggers the rebuild waits for release. Cheap controls
// that are just shader constants (detail, colour, paper white) do not use this -- they can afford to
// apply live.
template <typename Option>
static bool DeferredSlider(const char* label, Option* opt, float mn, float mx,
                           float def, const char* fmt = "%.2f", bool inheritReset = false)
{
    static std::unordered_map<ImGuiID, float> pending;
    const ImGuiID id = ImGui::GetID(label);

    auto it = pending.find(id);
    float value = it != pending.end() ? it->second : (opt->has_value() ? opt->value() : def);
    bool changed = false;

    if (ImGui::SliderFloat(label, &value, mn, mx, fmt))
        pending[id] = value;

    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        auto committed = pending.find(id);

        if (committed != pending.end())
        {
            *opt = std::clamp(committed->second, mn, mx);
            pending.erase(committed);
            changed = true;
        }
    }

    ImGui::SameLine();

    const std::string resetId = std::string("Reset##") + label;
    if (ImGui::SmallButton(resetId.c_str()))
    {
        if (inheritReset)
            *opt = std::optional<float> {};
        else
            *opt = def;
        pending.erase(id);
        changed = true;
    }

    if (std::strcmp(label, "Intensity") == 0)
        HelpMarker("Overall enhancement strength for this pass. 1 = default; results depend on the profile.\nValues above 1 are experimental; the runtime may clamp or ignore them.");
    else if (std::strcmp(label, "Local structure") == 0)
        HelpMarker("Fine detail and local contrast requested from the model (high-frequency structure).\n1 = default; values above 1 are experimental.");
    else if (std::strcmp(label, "Local tone") == 0)
        HelpMarker("Broad brightness and lighting changes requested from the model (low-frequency tone).\nLater passes default to 0. Values above 1 are experimental.");
    else if (std::strcmp(label, "Skin structure") == 0)
        HelpMarker("Fine detail for pixels the model identifies as skin. -1 follows Local structure; 0 reduces skin detail.\nSkin colour is controlled separately. Values above 1 are experimental.");
    return changed;
}

// An absent later-pass setting inherits pass 1. The first combo item represents that absence; the
// remaining items map directly to the model's zero-based profile values.
static bool InheritedProfileCombo(const char* label, CustomOptional<uint32_t, NoDefault>* opt,
                                  const char* const* names, int nameCount)
{
    int selected = 0;

    if (opt->has_value())
        selected = std::clamp((int) opt->value(), 0, nameCount - 2) + 1;

    if (!ImGui::Combo(label, &selected, names, nameCount))
        return false;

    if (selected == 0)
        *opt = std::optional<uint32_t> {};
    else
        *opt = (uint32_t) (selected - 1);

    return true;
}

// Per-pass profile values the pass presets write. One table shared by ApplyPassPreset and
// PassPresetActive (the button highlight), so what a preset sets and what counts as "that preset is
// in effect" can't drift apart.
struct PassProfile
{
    uint32_t style; // 0 Standard, 1 Natural, 2 Cinematic
    float intensity;
    float structure;
    float tone;
    float skin;
};

static constexpr PassProfile PresetPass1 { 1u, 1.8f, 1.8f, 1.8f, -1.0f };    // Natural
static constexpr PassProfile PresetPass2 { 1u, 1.0f, 1.0f, 1.0f, -1.0f };    // Natural
static constexpr PassProfile PresetPass3 { 1u, 0.75f, 1.5f, 0.46f, 1.0f };   // Natural

// This fork's recommended starting points for the "Model passes" slider, one per pass count.
// Each button touches only the settings named below (including Pass 2/3 overrides once the
// preset's pass count reaches them); anything else in the panel (skin/environment sliders,
// Compare, Debug view, Hold frame, Downscaler, exposure-scan settings, etc.) is left exactly as
// the user had it.
static void ApplyPassPreset(Config* config, unsigned int passes)
{
    config->DlssNrEnabled = true;
    config->DlssNrPrecision = 0u; // NVIDIA (FP8)
    config->DlssNrVitEvery = 2u; // Reuse bottleneck on, the default
    config->DlssNrVitEveryPlain = 2u; // the same for the plain FP16 kernels
    config->DlssNrApplyModel = true;
    config->DlssNrUnlockPasses = false;
    config->DlssNrPasses = passes;

    // Upscale Method, Upscale Mode and Final Image Composition are deliberately not set here: the Pre-SR/Post-SR
    // tiers set them, and writing them from a pass preset would clear that tier's highlight.
    config->DlssNrTransferStrength = 1.0f;      // Detail strength
    config->DlssNrColourStrength = 1.0f;
    config->DlssNrStyle = PresetPass1.style;
    config->DlssNrIntensity = PresetPass1.intensity;
    config->DlssNrLocalStructure = PresetPass1.structure;
    config->DlssNrLocalTone = PresetPass1.tone;
    config->DlssNrSkinStructure = PresetPass1.skin;
    config->DlssNrAutoMask = true;
    // Automatic exposure at its default brightness (DlssNr_AutoTrimDefault.h). Game exposure at 1x, which
    // this used to set, gave NBA 2K27 a model input with a median of 0.07-0.32 (measured 2026-09-25).
    config->DlssNrWhitePointSource = 3u;        // Automatic exposure
    config->DlssNrAutoExposureTrim = std::nullopt;
    config->DlssNrMaxRatio = 2.0f;               // Highlight guard

    if (passes >= 2u)
    {
        config->DlssNrPass2Style = PresetPass2.style;
        config->DlssNrPass2Intensity = PresetPass2.intensity;
        config->DlssNrPass2LocalStructure = PresetPass2.structure;
        config->DlssNrPass2LocalTone = PresetPass2.tone;
        config->DlssNrPass2SkinStructure = PresetPass2.skin;
        config->DlssNrPass2AutoMask = true;
    }

    if (passes >= 3u)
    {
        config->DlssNrPass3Style = PresetPass3.style;
        config->DlssNrPass3Intensity = PresetPass3.intensity;
        config->DlssNrPass3LocalStructure = PresetPass3.structure;
        config->DlssNrPass3LocalTone = PresetPass3.tone;
        config->DlssNrPass3SkinStructure = PresetPass3.skin;
        config->DlssNrPass3AutoMask = true;
    }
}

static bool NearlyEqual(float a, float b)
{
    return std::fabs(a - b) < 0.005f;
}

// Pass 2/3 settings are optional (absent = inherit pass 1), so an absent one never matches a preset.
template <typename FloatOpt>
static bool OptionalIs(FloatOpt& opt, float value)
{
    return opt.has_value() && NearlyEqual(opt.value(), value);
}

static bool Pass1Is(Config* config, const PassProfile& p)
{
    return config->DlssNrStyle.value_or_default() == p.style &&
           NearlyEqual(config->DlssNrIntensity.value_or_default(), p.intensity) &&
           NearlyEqual(config->DlssNrLocalStructure.value_or_default(), p.structure) &&
           NearlyEqual(config->DlssNrLocalTone.value_or_default(), p.tone) &&
           NearlyEqual(config->DlssNrSkinStructure.value_or_default(), p.skin);
}

static bool Pass2Is(Config* config, const PassProfile& p)
{
    return config->DlssNrPass2Style.has_value() && config->DlssNrPass2Style.value() == p.style &&
           OptionalIs(config->DlssNrPass2Intensity, p.intensity) &&
           OptionalIs(config->DlssNrPass2LocalStructure, p.structure) &&
           OptionalIs(config->DlssNrPass2LocalTone, p.tone) &&
           OptionalIs(config->DlssNrPass2SkinStructure, p.skin);
}

static bool Pass3Is(Config* config, const PassProfile& p)
{
    return config->DlssNrPass3Style.has_value() && config->DlssNrPass3Style.value() == p.style &&
           OptionalIs(config->DlssNrPass3Intensity, p.intensity) &&
           OptionalIs(config->DlssNrPass3LocalStructure, p.structure) &&
           OptionalIs(config->DlssNrPass3LocalTone, p.tone) &&
           OptionalIs(config->DlssNrPass3SkinStructure, p.skin);
}

// Whether a pass preset is what is currently in effect, for highlighting its button. Derived from
// the config (not remembered), like ResolutionTierActive: the pass count plus each pass's own
// profile must match. Detail/Colour strength, white point and the like are left out on purpose so
// tuning them doesn't drop the highlight.
static bool PassPresetActive(Config* config, unsigned int passes)
{
    return config->DlssNrPasses.value_or_default() == passes && Pass1Is(config, PresetPass1) &&
           (passes < 2u || Pass2Is(config, PresetPass2)) &&
           (passes < 3u || Pass3Is(config, PresetPass3));
}

// A button drawn green while its preset is the one in effect (the overlay's existing success green).
static bool PresetButton(const char* label, bool active)
{
    if (active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.65f, 0.31f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.45f, 0.20f, 1.0f));
    }

    const bool pressed = ImGui::Button(label);

    if (active)
        ImGui::PopStyleColor(3);

    return pressed;
}

// Quality tiers (High/Medium/Low/Potato) for running NR at a reduced model resolution, offered as
// two preset rows that differ only in where NR runs: Pre-SR (Before Super Resolution) and Post-SR
// (After Super Resolution). Each touches only Upscale Mode, Upscale Method, Model resolution,
// Final Image Composition and (where the tier lists one) Restore Sharpness, plus Auto model
// resolution -- turned off because Auto overrides the Model resolution slider when NR runs
// post-SR, which would make the tier's resolution a silent no-op. Each also sets "NR Pass at:",
// which includes turning off Generate-before-SR/apply-after-SR (DLSS): that mode generates pre-SR
// and disables the placement choice, so leaving it on would contradict the row's name.
struct ResolutionTier
{
    const char* name;
    uint32_t upscaleMethod;      // 0 Bilinear, 1 SGSR1
    float workingScale;          // model resolution as a fraction (1.0 = 100%)
    uint32_t composition;        // 1 Reversible curve + composed, 2 Reversible curve + replace
    float restoreSharpness;      // < 0: leave the current value (composed mode hides the slider)
    float detailStrength;        // < 0: leave the current value
    float colourStrength;        // < 0: leave the current value
};

static constexpr ResolutionTier ResolutionTiers[] = {
    { "High",   1u, 1.00f, 1u, -1.0f, 1.0f, 1.0f },
    { "Medium", 1u, 0.80f, 2u,  1.50f, -1.0f, -1.0f },
    { "Low",    0u, 0.65f, 2u,  1.50f, -1.0f, -1.0f },
    { "Potato", 0u, 0.50f, 2u,  1.70f, -1.0f, -1.0f },
};

static void ApplyResolutionTier(Config* config, int& pendingScale, const ResolutionTier& tier,
                                bool beforeSuperResolution)
{
    // Same rule as the "NR Pass at:" combo: leaving Finished Picture clears a session failure.
    if (config->DlssNrFinishedPicture.value_or_default())
        DlssNr::RetryAfterFailure();
    config->DlssNrFinishedPicture = false;
    config->DlssNrRunBeforeSr = beforeSuperResolution;
    config->DlssNrDeferredDlss = false;

    config->DlssNrTransfer = 2u; // Upscale Mode: NVIDIA residual
    config->DlssNrReducedUpscaleMethod = tier.upscaleMethod;
    config->DlssNrModelResolutionAuto = false;
    config->DlssNrWorkingScale = tier.workingScale;
    pendingScale = -1; // clear any in-flight drag
    config->DlssNrReversibleMode = tier.composition;

    if (tier.restoreSharpness >= 0.0f)
        config->DlssNrReplaceDetailStrength = tier.restoreSharpness;

    if (tier.detailStrength >= 0.0f)
        config->DlssNrTransferStrength = tier.detailStrength;

    if (tier.colourStrength >= 0.0f)
        config->DlssNrColourStrength = tier.colourStrength;
}

// Whether a tier's settings are what is currently in effect, for highlighting its button. Derived
// from the config rather than remembered, so it can never claim a preset the settings have since
// drifted from, and it survives a restart. Restore Sharpness is left out of the comparison on
// purpose: fine-tuning it after picking a tier shouldn't drop the highlight. At most one button
// can match -- tiers have distinct resolutions and the two rows differ in placement.
static bool ResolutionTierActive(Config* config, const ResolutionTier& tier, bool beforeSuperResolution)
{
    return !config->DlssNrFinishedPicture.value_or_default() &&
           config->DlssNrRunBeforeSr.value_or_default() == beforeSuperResolution &&
           config->DlssNrTransfer.value_or_default() == 2u &&
           config->DlssNrReducedUpscaleMethod.value_or_default() == tier.upscaleMethod &&
           !config->DlssNrModelResolutionAuto.value_or_default() &&
           std::fabs(config->DlssNrWorkingScale.value_or_default() - tier.workingScale) < 0.005f &&
           config->DlssNrReversibleMode.value_or_default() == tier.composition;
}

void RenderMenu(Config* config, float menuResScale)
{

    // DLSS Neural Rendering -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("DLSS Neural Rendering"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        // Moved up here (out of its original spot just above the Model-resolution slider) so
        // the "Optimized Defaults" preset button, which sits earlier in the panel, can clear
        // an in-flight drag when it overwrites the setting. Same static-local lifetime either
        // way.
        static int pendingScale = -1;

        bool enabled = config->DlssNrEnabled.value_or_default();
        if (ImGui::Checkbox("Enable Neural Rendering", &enabled))
            config->DlssNrEnabled = enabled;

        HelpMarker("Enhance lighting and material appearance with the NR model. Placement selects before or after upscaling.\nRequires nvngx_dlssnr.dll plus the included nvngx.dll_dlssnr.dll helper.");

        // Read early (checkbox itself is drawn down in NR Options) so the running-status block
        // right below can already report finished-picture-specific text.
        bool finishedPicture = config->DlssNrFinishedPicture.value_or_default();

        // Either backend. The two keep separate state, and on a native Vulkan game the D3D12 side
        // is never touched -- so asking only that one reports "waiting for the upscaler" over a pass
        // that is demonstrably running.
        const bool vulkan = DlssNr::IsRunningVk();

        // Turning the pass off does not release the model, so the feature handle stays alive and
        // IsRunning keeps answering yes. Reporting a cost from that was wrong in the way that matters
        // most: the toggle is how anyone A/Bs this, so the one moment the number is read is the one
        // moment it describes the frame before last.
        if (!enabled)
        {
            ImGui::TextDisabled("NR off.");
        }
        else if (!DlssNr::IsRunning() && !vulkan)
        {
            const auto feature = State::Instance().currentFeature;
            const bool nativeVk = feature && feature->Api() == API::Vulkan && !feature->IsWithDx12();
            const char* reason = nativeVk ? DlssNr::FailureReasonVk() : DlssNr::FailureReason();

            if (reason[0] != 0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Off for this session: %s.", reason);
                ImGui::SameLine();

                if (nativeVk)
                    ImGui::TextUnformatted("Restart the game to retry native Vulkan NR.");
                else if (ImGui::SmallButton("Retry"))
                    DlssNr::RetryAfterFailure();
            }
            else if (feature && feature->Api() == API::DX11 && !feature->IsWithDx12())
            {
                ImGui::TextWrapped("NR needs the D3D12 bridge on D3D11. Choose an upscaler marked w/Dx12 and restart.");
            }
            else if (nativeVk && config->DlssNrDeferredDlss.value_or_default())
            {
                ImGui::TextWrapped("Disable Generate before SR, apply after SR (DLSS) to use native Vulkan NR.");
            }
            else if (enabled)
                ImGui::TextUnformatted("Waiting for the upscaler to run.");
        }
        else
        {
            // The elapsed time belongs here rather than only in the upscaler's breakdown: that tooltip needs
            // OptiScaler's own upscaler to have run, and with native DLSS passing through there is
            // nothing in it to hang this off.
            // Either backend's timer. They measure the same thing by different means, and only one
            // of them is running.
            const auto ms = vulkan ? DlssNr::LastGpuTimeVk() : DlssNr::LastGpuTime();

            // With "Apply the model" off the pass STILL RUNS (so Hold-frame A/B can toggle its edit on
            // a frozen frame) -- it only outputs the clean frame.
            // Enable Neural Rendering off stops the work.
            const char* runSuffix =
                !config->DlssNrApplyModel.value_or_default() ? "  (model running, edit hidden)" : "";

            if (ms.has_value())
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running%s - %.2f ms elapsed%s",
                                   vulkan ? " natively on Vulkan" : "", ms.value(), runSuffix);
            else if (vulkan)
                // Measured but not yet read: the first few frames are still in the query ring.
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running natively on Vulkan - %llu frames%s",
                                   DlssNr::FramesVk(), runSuffix);
            else
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running.%s", runSuffix);

            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Time between the start and end of NR on the GPU, including delays while other work runs.\nCompare FPS to check the effect on game performance.");
            if (finishedPicture)
                ImGui::TextDisabled("Includes time shared with other GPU work.");

            if (!vulkan && DlssNr::BackendName()[0] != 0)
                ImGui::TextDisabled("Model backend: %s", DlssNr::BackendName());
        }

        ImGui::SeparatorText("Multipass Presets");
        if (PresetButton("1 Pass", PassPresetActive(config, 1u)))
            ApplyPassPreset(config, 1u);
        ImGui::SameLine();
        if (PresetButton("2 Pass", PassPresetActive(config, 2u)))
            ApplyPassPreset(config, 2u);
        ImGui::SameLine();
        if (PresetButton("3 Pass", PassPresetActive(config, 3u)))
            ApplyPassPreset(config, 3u);
        HelpMarker("Set this fork's recommended starting point for the chosen pass count: FP8 "
                   "precision and game-exposure white point. "
                   "Upscale Method, Upscale Mode and Final Image Composition are left alone; use the Pre-SR or "
                   "Post-SR presets for those. 2 Pass also sets Pass "
                   "2's overrides; 3 Pass sets Pass 2 and Pass 3's overrides. Overwrites the settings "
                   "below; anything not listed here, including NR Pass at:, is left as you have it.\n"
                   "The green button is the pass count currently in effect; changing the pass count "
                   "or a pass's Style, Intensity, Local structure, Local tone or Skin structure clears it.");

        // Directly under the pass presets, since those buttons set this slider's value. The
        // panel-wide item width is pushed further down (after NR Options), so this block pushes
        // its own to keep the slider the same width it had before it moved.
        ImGui::PushItemWidth(220.0f * menuResScale);

        // The checkbox that sets this lives under "Apply the model" (NR Options); it is read here
        // from config, so toggling it takes effect on the next frame.
        bool unlockPasses = config->DlssNrUnlockPasses.value_or_default();
        const unsigned int passLimit = unlockPasses ? MaxPassCount : DefaultMaxPassCount;

        {
            int passes = (int) std::clamp(config->DlssNrPasses.value_or_default(), 1u,
                                          passLimit);
            const ImVec4 colour = passes <= 1   ? ImVec4(0.35f, 0.88f, 0.38f, 1.0f)
                                  : passes == 2 ? ImVec4(0.95f, 0.70f, 0.20f, 1.0f)
                                                : ImVec4(0.92f, 0.30f, 0.25f, 1.0f);

            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, colour);

            if (ImGui::SliderInt("Model passes", &passes, 1, (int) passLimit,
                                 passes == 1 ? "%d (normal)" : "%dx model cost"))
                config->DlssNrPasses = (uint32_t) std::clamp(passes, 1, (int) passLimit);

            ImGui::PopStyleColor(2);

            // Reset's own label stays plain text -- placed after PopStyleColor so the
            // passes-based warning colour above doesn't tint it too.
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##modelpasses"))
                config->DlssNrPasses = 1u;

            HelpMarker("Process the image repeatedly. More passes strengthen the effect and increase GPU cost.\nEach pass has its own settings and history. Start with 1.");
        }

        {
            // Disabled rather than hidden at Passes == 1: the control exists, it just has nothing to
            // do yet (there is no boundary between passes to damp), which is a clearer statement
            // than making it vanish and reappear as Passes changes.
            const bool noBoundary = config->DlssNrPasses.value_or_default() <= 1;
            ImGui::BeginDisabled(noBoundary);
            float feedback = config->DlssNrPassFeedback.value_or_default();
            if (ImGui::SliderFloat("Pass feedback", &feedback, 0.0f, 1.0f,
                                   feedback >= 1.0f ? "%.2f (full, current behaviour)" : "%.2f"))
                config->DlssNrPassFeedback = std::clamp(feedback, 0.0f, 1.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##passfeedback"))
                config->DlssNrPassFeedback = 1.0f;
            ImGui::EndDisabled();

            HelpMarker("How much of an extra pass's raw answer the next pass actually receives.\n\n"
                       "1.0 is what every configuration has always done: the next pass gets the full "
                       "answer. Every pass after the first is already being shown something the model "
                       "was never trained on -- its own previous output instead of a raw frame -- so "
                       "lower values hold each pass closer to that training distribution instead of "
                       "drifting further from it with every extra pass, at the cost of a smaller "
                       "cumulative edit.\n\nNo effect at Passes = 1: there is no boundary to damp.");
        }

        ImGui::PopItemWidth();

        // Both rows share the same button labels, so each gets its own ImGui ID scope.
        const auto tierRow = [&](const char* id, bool beforeSuperResolution) {
            ImGui::PushID(id);
            for (int i = 0; i < IM_ARRAYSIZE(ResolutionTiers); ++i)
            {
                if (i > 0)
                    ImGui::SameLine();
                if (PresetButton(ResolutionTiers[i].name,
                                 ResolutionTierActive(config, ResolutionTiers[i], beforeSuperResolution)))
                    ApplyResolutionTier(config, pendingScale, ResolutionTiers[i], beforeSuperResolution);
            }
            ImGui::PopID();
        };

        ImGui::SeparatorText("Pre-SR Presets");
        tierRow("pre", true);
        HelpMarker("Quality tiers for running NR before Super Resolution at a lower model resolution, from High (100%, best quality) down to Potato (50%, cheapest).\n"
                   "Each sets Upscale Mode, Upscale Method, Model resolution and Final Image Composition (High also sets Detail and Colour strength to 1; Medium, Low and Potato also set Restore Sharpness), and turns Auto model resolution off so the resolution applies.\n"
                   "It also sets NR Pass at: to Before Super Resolution. Anything not listed here is left as you have it.\n"
                   "The green button is the tier currently in effect; changing NR Pass at:, Upscale Mode, Upscale Method, Model resolution or Final Image Composition clears it.");

        ImGui::SeparatorText("Post-SR Presets");
        tierRow("post", false);
        HelpMarker("Quality tiers for running NR after Super Resolution at a lower model resolution, from High (100%, best quality) down to Potato (50%, cheapest).\n"
                   "Each sets Upscale Mode, Upscale Method, Model resolution and Final Image Composition (High also sets Detail and Colour strength to 1; Medium, Low and Potato also set Restore Sharpness), and turns Auto model resolution off so the resolution applies.\n"
                   "It also sets NR Pass at: to After Super Resolution. Anything not listed here is left as you have it.\n"
                   "The green button is the tier currently in effect; changing NR Pass at:, Upscale Mode, Upscale Method, Model resolution or Final Image Composition clears it.");

        ImGui::SeparatorText("HDR Input");
        ImGui::TextDisabled("HDR input settings. Adjust the brightness range presented to NR.");

        {
        // Logarithmic, because the useful range is not linear. A quarter to 240: the low end because
        // a frame the game already tone mapped wants roughly 1, the high end because there is no
        // principled ceiling -- this is a divisor on an open-ended linear buffer, and how far up a
        // given game needs to go is a property of that game's exposure, not of anything we can bound.
        // One tester was still improving at 100. A linear slider over that span would spend nine
        // tenths of its travel on values nobody needs and never reach the ones they do.
        // One dropdown, because there is one answer.
        //
        // This was two checkboxes that could both be on, and every attempt to stop that was a patch
        // on a shape that should not have existed. Greying deadlocked -- each disabled the other, so
        // once both were set the only way out was a button the notice never mentioned. Clearing
        // worked but silently undid a setting somebody had made. Both were ways to stop an illegal
        // state being REACHED; a single choice cannot reach it, because there is only one value to
        // be in.
        //
        // Each option also says whether it can actually do anything in THIS game, in colour, so the
        // choice is made on what is available rather than on what sounds best.
        {
            const bool vk = DlssNr::IsRunningVk();
            const auto ex = vk ? DlssNr::GameExposureStatusVk() : DlssNr::GameExposureStatus();
            const bool haveExposure = HaveGameExposure();

            const float anchorNow = DlssNr::ExposureScan::BestValue();
            const bool haveAnchor = !DlssNr::ExposureScan::Anchors().empty();

            static const char* sourceNames[] = { "Manual paper white", "Game exposure",
                                                 "Scanned exposure (experimental)",
                                                 "Automatic exposure from HDR frame" };

            int source = (int) config->DlssNrWhitePointSource.value_or_default();

            if (source < 0 || source > 3)
                source = 0;

            if (ImGui::Combo("White point source", &source, sourceNames, IM_ARRAYSIZE(sourceNames)))
            {
                config->DlssNrWhitePointSource = (uint32_t) source;

                // Nothing else to set. The scan asks the source whether it is wanted, so choosing
                // it here is the whole of switching it on -- there is no second flag to keep in
                // step, and so no way for the two to disagree.
            }

            HelpMarker("Manual: use Paper white. Game exposure: use exposure supplied by the game.\nScanned exposure: estimate it from game buffers; requires calibration and may select the wrong buffer.\nAutomatic exposure: OptiScaler meters the linear HDR frame itself, so it needs nothing from the game.");

            // Availability, in colour, for the option currently chosen.
            if (source == 1)
            {
                if (!vk && ex.seenFrames == 0)
                    ImGui::TextDisabled("Waiting for a frame...");
                else if (!haveExposure)
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                       "No game exposure available. Using manual paper white.");
                else if (ex.exposure > 1e-6f)
                {
                    const float baseWhitePoint = ex.preExposure / ex.exposure;
                    const auto trimAnchors =
                        DlssNrTrim::Parse(config->DlssNrGameExposureTrimAnchors.value_or_default());
                    const float trim = DlssNrTrim::TrimForKey(
                        baseWhitePoint, config->DlssNrWhitePointTrim.value_or_default(), trimAnchors, false);
                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "Game exposure %.4f  ->  model white at %.2f%s", ex.exposure,
                                       baseWhitePoint * trim,
                                       ex.offeredNow ? "" : "  (held: absent this frame)");
                }
                else
                    ImGui::TextDisabled("Reading exposure...");
            }
            else if (source == 3)
            {
                const auto autoEx = vk ? DlssNr::AutoExposureStatusVk() : DlssNr::AutoExposureStatus();

                if (autoEx.exposure > 1e-8f)
                {
                    const float baseWhitePoint = autoEx.preExposure / autoEx.exposure;
                    const auto trimAnchors =
                        DlssNrTrim::Parse(config->DlssNrAutoExposureTrimAnchors.value_or_default());
                    const float trim = DlssNrTrim::TrimForKey(
                        baseWhitePoint, DlssNr::AutoTrimEffective(*config), trimAnchors, false);
                    // Middle-grey metering, mode 13 in dlssnr.hlsl: exposure = 0.18 / (0.82 * average scene brightness).
                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "Scene brightness %.3f  ->  model white at %.2f",
                                       0.18f / (0.82f * autoEx.exposure), baseWhitePoint * trim);
                    HelpMarker("Measured from the linear HDR frame before NR runs (raw exposure value shown below).\n"
                               "Model white is the brightness level the picture is scaled to: anything at or above it counts as full white.");
                    ImGui::TextDisabled("Automatic exposure %.4f", autoEx.exposure);
                }
                else
                    ImGui::TextDisabled("Calculating automatic exposure...");
            }
            else if (source == 2)
            {
                // "Nothing found" and "found several, none of them moving" are different states,
                // and this said the first for both. In GTA V the log carried eight candidates while
                // the panel claimed there were none, which reads as the scan being broken when what
                // it actually needs is for the light to change.
                if (anchorNow <= 0.0f)
                {
                    const unsigned int watching = (unsigned int) DlssNr::ExposureScan::Report().size();

                    if (watching == 0)
                        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                           "No exposure candidates found.");
                    else
                        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                           "%u candidates; move between bright and dark areas to test them.",
                                           watching);
                }
                else if (!haveAnchor)
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                       "Exposure candidate found. Adjust Paper white, then select Anchor here.");
                // Once anchored, the scan -> white point readout sits above the sliders below; it is
                // not repeated up here.
            }
            else if (haveExposure)
            {
                ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                   "Game exposure is available.");
            }
        }






        // A measured suggestion for paper white used to sit here and has been withdrawn.
        //
        // It took the 90th percentile of per-tile peak luminance from the untouched frame, which is a
        // statement about scene content rather than about the buffer's scale. In Nioh 3, where the
        // right answer is about 240, it offered 8 -- because most tiles are shadow and the percentile
        // sits wherever most tiles are. The guard meant to catch that compared each tile against the
        // frame's own brightest, which is scale-free and therefore passes on a black screen: the same
        // relative-threshold mistake the white point meter was removed for, made a second time.
        //
        // A wrong number offered confidently is worse than no number, so nothing is offered. What
        // replaces it has to be a measurement of the game's own exposure rather than of its scenery:
        // the exposure texture where a game supplies one, and otherwise the ratio between the
        // scene-referred buffer and the finished frame, which is that exposure by definition.

        // Two controls, not one control with two meanings.
        //
        // These are different quantities. The manual path wants an absolute divisor on an open-ended
        // linear buffer -- Nioh 3 needs about 240 -- and the exposure path wants a multiplier on a
        // number the game already supplied, where 1 is correct and anything far from it says the read
        // is wrong rather than that somebody prefers it.
        //
        // They used to share one stored value, narrowed to 0.25..4 when the toggle was on. That kept
        // a ruinous value unreachable but left two worse problems: moving the slider in one mode
        // silently destroyed the number found in the other, and there was no way back to "just take
        // the game's answer" short of knowing that the number for it was 1. Separate values fix both.
        // Switching modes is now non-destructive in both directions.
        // The trim belongs to both automatic sources, since both end in "the game's number times a
        // little". Only the manual source gets the absolute slider.
        // One slider per source, each remembering its own number.
        //
        // A trim on the game's exposure and a trim on a buffer the scan found are trims on different
        // things, and a value found against one means nothing against the other. Sharing them meant
        // changing source silently carried a number across, so a picture that had been tuned came
        // back wrong for a reason nothing on screen explained.
        //
        // The scan before it is anchored is the exception, and it has to be: anchoring captures an
        // absolute white point, so there must be an absolute slider to set. Showing a trim there
        // asked people to "set paper white below" next to a control that was not paper white.
        const int wpSource = (int) config->DlssNrWhitePointSource.value_or_default();

        // Which anchor row the paper-white slider edits, or -1 for the live unanchored point. Menu-
        // local and not persisted; the anchor block below sets it when a row is clicked. Declared
        // here because both the slider (this block) and the table (below) read it in the same frame.
        static int selectedAnchor = -1;
        auto anchors = DlssNr::ExposureScan::Anchors();
        if (selectedAnchor >= (int) anchors.size())
            selectedAnchor = -1;

        if (wpSource == 2)
        {
            const bool editingRow = selectedAnchor >= 0 && selectedAnchor < (int) anchors.size();

            // The single scan -> white point readout, above the sliders it explains.
            if (!anchors.empty())
            {
                const float liveScan = DlssNr::ExposureScan::BestValue();

                if (liveScan > 0.0f)
                {
                    const float w = DlssNr::ExposureScan::AnchoredWhitePoint(
                        liveScan, config->DlssNrScanInverted.value_or_default(),
                        config->DlssNrScanTrim.value_or_default());

                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "Scan %.5f  ->  white point %.2f   (%u point%s)", liveScan, w,
                                       (unsigned) anchors.size(), anchors.size() == 1 ? "" : "s");
                }
            }

            // Paper white shows only when there is a point to set: before the first anchor, or when a
            // row is selected to edit. Once points exist and none is selected, the white point is fixed
            // by the anchors and only the trim adjusts the live picture -- so the trim takes the
            // slider's place, the same shape as the game-exposure source.
            const bool showPaperWhite = anchors.empty() || editingRow;

            if (showPaperWhite)
            {
                float pw = editingRow ? anchors[selectedAnchor].white
                                      : config->DlssNrWhitePointScale.value_or_default();

                char lbl[48];
                if (editingRow)
                    snprintf(lbl, sizeof(lbl), "Paper white (editing point %d)", selectedAnchor + 1);
                else
                    snprintf(lbl, sizeof(lbl), "Paper white");

                if (ImGui::SliderFloat(lbl, &pw, 0.25f, 2000.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
                {
                    if (editingRow)
                    {
                        DlssNr::ExposureScan::AnchorSetWhite(selectedAnchor, pw);
                        config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                    }
                    else
                        config->DlssNrWhitePointScale = pw;
                }

                HelpMarker("Adjust the selected calibration point, or set the value for the next point.\nUse Anchor here to save the current lighting condition.");
            }

            // The trim multiplies the interpolated result, and in the steady state it is the control
            // that stands in for paper white: adjust it until the picture looks right in the current
            // light, then Anchor bakes that trimmed value into a new point and resets the trim to 1.
            if (!anchors.empty())
            {
                float trim = config->DlssNrScanTrim.value_or_default();

                if (ImGui::SliderFloat("Trim (x the scan)", &trim, 0.25f, 4.0f, "%.2fx",
                                       ImGuiSliderFlags_Logarithmic))
                    config->DlssNrScanTrim = std::clamp(trim, 0.25f, 4.0f);

                ImGui::SameLine();

                if (ImGui::SmallButton("Reset##scantrim"))
                    config->DlssNrScanTrim = 1.0f;

                HelpMarker("Multiply the calibrated white point. Anchor here saves the adjusted value and resets this multiplier to 1.");
            }
        }
        else if (wpSource == 1)
        {
            // Up to 50x under the hood: a game's reported exposure scale can sit well below what the picture wants
            // (Marvel's Spider-Man Remastered with XeSS swapped to DLSS is one), so 4x was too tight. Shown as
            // stops around 1x, which is why the slider runs further towards darker than towards brighter.
            RenderTrimEvSlider(config->DlssNrWhitePointTrim, 1.0f,
                               DlssNrTrim::Parse(config->DlssNrGameExposureTrimAnchors.value_or_default()).size(),
                               "gameexposure",
                               "Brightness of the picture handed to NR, relative to the exposure the game reports."
                               "\n+ is brighter, - is darker; 0 EV uses the game's exposure as is."
                               "\nToo bright clips highlights; too dark hides shadow detail.");
        }
        else if (wpSource == 3)
        {
            // The scale stays centred on a 5x Trim (0 EV), the old default from the PR this came from. The default is
            // +1.5 EV for every game; see DlssNr_AutoTrimDefault.h for the measurements. It is independent of the Game exposure Trim.
            RenderTrimEvSlider(config->DlssNrAutoExposureTrim, 5.0f,
                               DlssNrTrim::Parse(config->DlssNrAutoExposureTrimAnchors.value_or_default()).size(),
                               "autoexposure",
                               "Brightness of the picture handed to NR. + is brighter, - is darker."
                               "\nUntil you move it, it is +1.5 EV in every game."
                               "\nReset goes back to that default."
                               "\nToo bright clips highlights or tints shadows; too dark hides shadow detail."
                               "\nOptiScaler meters the linear HDR frame itself before NR runs."
                               "\nAutomatic exposure is available on D3D12 and Vulkan.",
                               DlssNrAutoTrim::kDefaultTrim);

            // Following the game's own exposure (DlssNr_FollowGame.h): on by default for a known unexposed game
            // (DlssNr_GameDefaults.h). Vulkan follows from the host value, a few frames behind the game.
            {
                const bool followVk = DlssNr::IsRunningVk();
                bool follow = DlssNr::FollowGameOn(*config);

                if (ImGui::Checkbox("Follow the game's exposure", &follow))
                    config->DlssNrAutoExposureFollowGame = follow;

                HelpMarker("For games that hand over their frame before applying their own exposure: on by default"
                           "\nfor those known to (RDR2), off for every other game. Automatic learns how its own metering"
                           "\nrelates to the game's exposure in the first seconds of play, then follows the game's exposure,"
                           "\nso brightness moves exactly with the game: cutscenes, menus, fades. The brightness slider"
                           "\nkeeps its meaning. Leave it off for games that expose their frame themselves (most games):"
                           "\nit would apply their exposure twice. On Vulkan it follows a few frames behind the game.");

                const auto followStatus =
                    followVk ? DlssNr::FollowGameExposureStatusVk() : DlssNr::FollowGameExposureStatus();
                const auto& calibration = DlssNrFollowGame::Instance();

                if (!follow)
                    ImGui::TextDisabled("Off");
                else if (!followStatus.gameExposureSeen)
                    ImGui::TextDisabled("Not available yet: no exposure from the game");
                else if (!calibration.Locked())
                    ImGui::TextDisabled("Learning the calibration... (%u/%u)", calibration.Readings(),
                                        DlssNrFollowGame::kWindow);
                else
                    ImGui::TextDisabled("Calibration %+.2f EV against the game's exposure%s", calibration.OffsetEv(),
                                        followStatus.following ? "; following" : "; not following");

                // The calibration is learned once per session; this learns it again.
                if (ImGui::SmallButton("Re-calibrate##autoexposure"))
                {
                    DlssNrFollowGame::Instance().Reset();
                    LOG_INFO("DLSS-NR automatic exposure: re-calibration requested");
                }

                HelpMarker("Learns the calibration against the game's exposure again, for example when it was"
                           "\nlearned during a cutscene or a loading screen. Plain Automatic is used meanwhile (about 2 s)."
                           "\nRe-calibrate in an ordinary daylight scene, not snow, night or indoors: the brightness"
                           "\nlearned there is kept for the whole game.");
            }

            float protection = config->DlssNrAutoExposureShadowProtection.value_or_default();
            if (ImGui::SliderFloat("Ignore bright highlights", &protection, 0.0f, 100.0f, "%.0f%%"))
                config->DlssNrAutoExposureShadowProtection = std::clamp(protection, 0.0f, 100.0f);

            HelpMarker("Stops the sky, lamps and reflections from darkening the rest of the picture."
                       "\n0% averages the whole frame as it is; 100% counts bright areas the least."
                       "\nBlack bars and black borders are always left out.");
        }
        else
        {
            // Logarithmic, because the useful range is not linear. A quarter to 2000: the low end
            // because a frame the game already tone mapped wants roughly 1, the high end because
            // there is no principled ceiling -- this is a divisor on an open-ended linear buffer, and
            // how far up a given game needs to go is a property of that game's exposure rather than
            // of anything that can be bounded here. One tester was still improving at 100.
            float wpScale = config->DlssNrWhitePointScale.value_or_default();

            if (ImGui::SliderFloat("Paper white", &wpScale, 0.25f, 2000.0f, "%.2fx",
                                   ImGuiSliderFlags_Logarithmic))
                config->DlssNrWhitePointScale = wpScale;

            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##paperwhite"))
                config->DlssNrWhitePointScale = 1.0f;

        HelpMarker("Brightness reference used to prepare HDR colour for NR. Higher values darken the model input; lower values brighten it.\nAdjust if NR loses detail or produces colour shifts.");
        }

        // Highlight guard, directly under the white point / trim -- it bounds the model's edit and
        // belongs with the exposure controls it works alongside.
        float maxRatio = config->DlssNrMaxRatio.value_or_default();
        if (ImGui::SliderFloat("Highlight guard", &maxRatio, 1.0f, MaxHighlightGuard, "%.1fx"))
            config->DlssNrMaxRatio = maxRatio;

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##guard"))
            config->DlssNrMaxRatio = 2.0f;

        HelpMarker("Limit how much NR can brighten a pixel; darkening is not capped. Lower values restrict highlight changes; higher values allow more.\nReplace mode still bounds darkening too -- a different guard, for a different reason.");

        // Directly under the white point, because that is the number it moves and the number the
        // anchor captures. It used to sit under Inspect, a whole section away from the slider it
        // reads, which left "Anchor here" looking like a control for something else entirely.
        {
            // No checkbox here any more.
            //
            // The dropdown above says whether the scan is the white point's source, and that is
            // the only reason anybody using this would want it running. A second control could
            // only agree with the dropdown or contradict it, and both were on offer: it began as
            // a redundant question and became a way to switch off the thing the chosen source
            // depended on.
            //
            // The ini key survives as a developer override for the one case a user has no reason
            // to want -- running the scan in a game that supplies a REAL exposure, so the log can
            // compare the two. That is validation, and validation does not need a widget.
            //
            // Worth keeping written down, since the panel no longer says it: the scan matches
            // buffers by SHAPE, and shape is a weak filter. In GTA V -- a game that supplies a
            // real exposure, so the right answer sat visible beside it -- the best candidate was
            // a 1x1 R32_FLOAT that climbed in a straight line for seventeen minutes while the
            // true exposure held still. Their ratio moved 14x. That is an accumulator, not an
            // eye adaptation.

                // Only where it means something. The lamp reads the scan, so offering it beside a
                // white point that comes from the game's own exposure is offering a control that
                // cannot light up.
                bool meter = config->DlssNrScanMeter.value_or_default();

                if (config->DlssNrWhitePointSource.value_or_default() == 2 &&
                    ImGui::Checkbox("Show exposure meter", &meter))
                    config->DlssNrScanMeter = meter;

                HelpMarker("Show the scanned exposure value and a colour indicator. Display only; does not change the image.");

            // Shown when the scan is actually running, whichever way it got switched on.
            if (DlssNr::ExposureScan::Scanning())
            {
                // Anchoring: one press, then it never needs touching again.
                //
                // The absolute white point cannot come out of a buffer whose units are unknown.
                // Every value AFTER the first can: only the ratio against the anchor is used, so
                // whatever the number means, it cancels. That is why this is a button and not a
                // measurement -- the one thing a person can supply that no amount of cleverness
                // can is "this looks right to me".
                int which = 0;
                float low = 0.0f, high = 0.0f;
                const float live = DlssNr::ExposureScan::BestValue(&which, &low, &high);

                const bool isSource = config->DlssNrWhitePointSource.value_or_default() == 2;

                // Anchor captures (currentScan, currentPaperWhite) and ADDS a row -- it does not
                // replace. One row is the old single-anchor ratio law; add a second in different
                // light and the white point is interpolated between the points, so it holds across
                // the whole range instead of only near one anchor. Greyed unless the scan is the
                // chosen source and it currently has a value to capture.
                ImGui::BeginDisabled(live <= 0.0f || !isSource);

                if (ImGui::Button("Anchor here"))
                {
                    // What to capture. Before the first point, the paper white above (an absolute value
                    // with the wide range a fresh game needs). After that, the EFFECTIVE white point the
                    // picture is showing right now -- the interpolated value times the Trim the user just
                    // dialed in -- so a second point in different light captures the trimmed look, not a
                    // frozen paper white (which would make two equal whites and a flat, non-tracking
                    // curve). The trim is reset afterwards: the new point, which the picture now passes
                    // through exactly, must not be multiplied by it a second time.
                    const float captureWhite =
                        anchors.empty()
                            ? std::max(0.01f, config->DlssNrWhitePointScale.value_or_default())
                            : std::max(0.01f, DlssNr::ExposureScan::AnchoredWhitePoint(
                                                  live, config->DlssNrScanInverted.value_or_default(),
                                                  config->DlssNrScanTrim.value_or_default()));

                    if (DlssNr::ExposureScan::AnchorAdd(live, captureWhite))
                    {
                        config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                        config->DlssNrScanTrim = 1.0f;
                        selectedAnchor = -1;
                    }
                }

                ImGui::EndDisabled();

                HelpMarker("Save the current exposure and white point as a calibration point.\nAdjust Paper white for the first point, then Trim for additional lighting conditions. Up to 8 points.");

                if (!isSource)
                    ImGui::TextDisabled("Scanned exposure is not the selected white point source.");

                if (!anchors.empty())
                {
                    // The row nearest the live scan value (in log space) is the one driving the
                    // picture right now; mark it so the user can see which calibration is in effect.
                    int active = 0;
                    float bestDist = 1e30f;
                    const float liveLog = std::log(std::max(live, 1e-6f));

                    for (size_t i = 0; i < anchors.size(); ++i)
                    {
                        const float d =
                            std::fabs(std::log(std::max(anchors[i].scan, 1e-6f)) - liveLog);
                        if (d < bestDist)
                        {
                            bestDist = d;
                            active = (int) i;
                        }
                    }

                    for (size_t i = 0; i < anchors.size(); ++i)
                    {
                        ImGui::PushID((int) i);

                        // Delete first, so its click is never swallowed by the row-wide Selectable.
                        if (ImGui::SmallButton("x"))
                        {
                            DlssNr::ExposureScan::AnchorRemove((int) i);
                            config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                            if (selectedAnchor == (int) i)
                                selectedAnchor = -1;
                            else if (selectedAnchor > (int) i)
                                --selectedAnchor;
                            ImGui::PopID();
                            continue;
                        }

                        ImGui::SameLine();

                        const bool sel = (int) i == selectedAnchor;
                        char row[96];
                        snprintf(row, sizeof(row), "%s scan %.4f  ->  white %.2f%s",
                                 ((int) i == active && isSource) ? ">" : "  ", anchors[i].scan,
                                 anchors[i].white, sel ? "   [editing]" : "");

                        // Click selects the row (slider edits it); click again deselects (slider
                        // returns to the live unanchored point).
                        if (ImGui::Selectable(row, sel))
                            selectedAnchor = sel ? -1 : (int) i;

                        ImGui::PopID();
                    }

                    ImGui::TextDisabled("Select a row to edit it; select it again"
                                        " to deselect. > marks the active point.");
                }

                // The direction flag only means anything with a single point; with two or more the
                // direction the white point moves is already fixed by the data.
                if (anchors.size() == 1)
                {
                    bool inverted = config->DlssNrScanInverted.value_or_default();
                    if (ImGui::Checkbox("Invert exposure tracking", &inverted))
                        config->DlssNrScanInverted = inverted;

                    HelpMarker("Reverse how scanned exposure changes the white point. Only needed with one calibration point.");
                }

                // The scan -> white point readout is shown above the sliders now, not here.

                // Everything below is read-out rather than control: what the scan is looking at and
                // how to tell whether it found the right thing. Folded away because the two decisions
                // that matter -- anchor, and which way the number runs -- are above it.
                if (ImGui::TreeNode("Advanced"))
                {

                    const auto found = DlssNr::ExposureScan::Report();
                    const char* why = DlssNr::ExposureScan::Status();

                    if (found.empty())
                    {
                        ImGui::TextDisabled("%s", why != nullptr && why[0] != 0
                                                      ? why
                                                      : "No exposure candidates found.");
                    }
                    else
                    {
                        for (size_t i = 0; i < found.size(); ++i)
                        {
                            const auto& c = found[i];

                            if (c.reads == 0)
                            {
                                ImGui::TextDisabled("%zu. %s -- not read yet", i + 1, c.shape.c_str());
                                continue;
                            }

                            // Moving is the whole signal, so it is the thing that is coloured.
                            ImGui::TextColored(c.moves ? ImVec4(0.45f, 0.8f, 0.45f, 1.0f)
                                                       : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                                               "%zu. %s = %.5f  (seen %.5f..%.5f) %s", i + 1,
                                               c.shape.c_str(), c.latest, c.lowest, c.highest,
                                               c.moves ? "MOVES" : "flat so far");
                        }

                        ImGui::TextDisabled("Move between bright and dark areas to check exposure tracking.");
                        ImGui::TextDisabled("A value that only increases may be a counter.");
                    }

                    ImGui::TreePop();
                }
            }
        }


        }

        ImGui::SeparatorText("NR Options");

        bool beforeSr = config->DlssNrRunBeforeSr.value_or_default() ||
                        (finishedPicture && config->DlssNrDeferredDlss.value_or_default());
        const auto activeFeature = State::Instance().currentFeature;
        const bool rayReconstruction = activeFeature && activeFeature->GetUpscalerType() == Upscaler::DLSSD;
        const bool deferredActive = !finishedPicture && config->DlssNrDeferredDlss.value_or_default() && !rayReconstruction;

        // A single exclusive choice, not two independent checkboxes: "before SR" used to be one
        // checkbox whose own label AND meaning silently changed depending on finishedPicture's
        // state, and "neither checked" was an unlabeled third placement (after SR, not on the
        // finished picture) a user had to infer rather than see. All three are named options here.
        //
        // A Combo, not inline RadioButtons: this panel's width isn't user-resizable, and three
        // radios with these labels ran off the visible edge with no way to reach the third one.
        // Every other 3+-option control in this file (Model precision right below, Upscale Mode,
        // Upscale Method, Final Image Composition) is already a Combo for the same reason.
        static const char* placementNames[] = { "After Super Resolution", "Before Super Resolution",
                                                 "Finished Picture" };
        int placement = finishedPicture ? 2 : (beforeSr ? 1 : 0);

        if (deferredActive)
            ImGui::BeginDisabled();
        if (ImGui::Combo("NR Pass at:", &placement, placementNames, IM_ARRAYSIZE(placementNames)))
        {
            if (placement == 0)
            {
                if (finishedPicture)
                    DlssNr::RetryAfterFailure();
                finishedPicture = false;
                beforeSr = false;
                config->DlssNrFinishedPicture = false;
                config->DlssNrRunBeforeSr = false;
            }
            else if (placement == 1)
            {
                if (finishedPicture)
                    DlssNr::RetryAfterFailure();
                finishedPicture = false;
                beforeSr = true;
                config->DlssNrFinishedPicture = false;
                config->DlssNrRunBeforeSr = true;
            }
            else
            {
                if (!finishedPicture)
                    DlssNr::RetryAfterFailure();
                finishedPicture = true;
                config->DlssNrFinishedPicture = true;
            }
        }
        if (deferredActive)
            ImGui::EndDisabled();

        HelpMarker("Choose where in the pipeline NR runs.\nAfter Super Resolution (default): apply NR once SR has upscaled the frame.\nBefore Super Resolution: apply NR to the smaller pre-upscale image instead. No effect when the game's Ray Reconstruction is active -- RR always runs NR after RR+SR, and unsupported input layouts fall back to after SR.\nFinished Picture: apply NR after the game has finished its lighting and effects, which may help with green noise. Works with frame generation on or off in native DirectX 12 games (SDR, HDR10, scRGB), and can also change the HUD and menus.");

        if (finishedPicture && enabled)
        {
            const auto feature = State::Instance().currentFeature;
            if (feature && (feature->Api() != API::DX12 || feature->IsWithDx12()))
                ImGui::TextWrapped("This option needs a native DirectX 12 game.");
            else
                ImGui::TextWrapped("%s", DlssNr::FinishedPictureStatus().c_str());
        }

        // Nested under Finished Picture: a second, independent axis (generate the changes at the
        // smaller pre-SR size vs. at the finished picture's own size), not a fourth top-level
        // placement -- progressive disclosure, same as every other mode-gated control in this file.
        if (finishedPicture)
        {
            if (ImGui::Checkbox("Run the model before Super Resolution", &beforeSr))
            {
                config->DlssNrRunBeforeSr = beforeSr;
                config->DlssNrDeferredDlss = false;
            }
            HelpMarker("Run the model at the smaller input size, upscale its changes with DLSS, then apply them to the finished picture.\nExperimental: the colour transfer is approximate and may look different. Requires DLSS SR; does not support RR.");
        }

        bool deferredDlss = config->DlssNrDeferredDlss.value_or_default();
        int precisionChoice = config->DlssNrPrecision.value_or_default() == 4 ? 1 : 0;
        const char* precisions[] = { "NVIDIA (FP8)", "Experimental (FP8+NVFP4 hybrid)" };
        if (ImGui::Combo("Model precision", &precisionChoice, precisions, IM_ARRAYSIZE(precisions)))
            config->DlssNrPrecision = precisionChoice == 1 ? 4u : 0u;
        HelpMarker("NVIDIA: original FP8 model (default), with some sensitive operations kept at higher precision.\nExperimental: this fork's FP8+NVFP4 hybrid for RTX 50 GPUs; output may differ slightly.");
        // One setting per kernel set: the fp8 kernels (NVIDIA's DLL and fp8-based builds) and the plain FP16 kernels (used by some modified DLSS-NR DLLs).
        // Only the one for the kernels actually running is used.
        const char* kernelSet = DlssNrNative::VitKernelSet();
        bool vitReuse = config->DlssNrVitEvery.value_or_default() > 1;
        if (ImGui::Checkbox("Reuse bottleneck: FP8 kernels", &vitReuse))
            config->DlssNrVitEvery = vitReuse ? 2u : 1u;
        HelpMarker("Recomputes the model's coarsest stage (its 32x18 bottleneck) only every other frame and reuses the last result in between, "
                   "which saves roughly a tenth of the model's GPU time.\nThat stage changes slowly, so the picture usually barely differs, "
                   "but fast camera motion can look slightly softer. Scene cuts always recompute. With several passes, all passes compute on the same frame "
                   "and all reuse on the next.\nOn by default. Applies immediately, NVIDIA's own model only.\n"
                   "Used when the model runs NVIDIA's FP8 kernels (NVIDIA's DLL and FP8-based builds).");
        bool vitReusePlain = config->DlssNrVitEveryPlain.value_or_default() > 1;
        if (ImGui::Checkbox("Reuse bottleneck: plain FP16 kernels", &vitReusePlain))
            config->DlssNrVitEveryPlain = vitReusePlain ? 2u : 1u;
        HelpMarker("The same as above, used when the model runs the plain FP16 kernels (used by some modified DLSS-NR DLLs).\nOn by default.");
        ImGui::Text("Kernel set in use: %s", kernelSet);
        if (DlssNrNative::VitPlainKernels() ? vitReusePlain : vitReuse)
            ImGui::TextUnformatted(("Bottleneck reuse: " + DlssNrNative::VitStatus()).c_str());
        if (precisionChoice > 0)
        {
            ImGui::TextUnformatted(enabled && DlssNrNative::IsActive() ? "Hybrid: active" : "Hybrid: inactive");
            ImGui::TextWrapped("Loading may pause the game and look like a freeze. Please wait.");
        }
        // Keep failure details in the log without displaying changing kernel counters in the menu.
        auto hybridStatus = DlssNrNative::Status();
        hybridStatus = hybridStatus.substr(0, hybridStatus.find(" |"));
        static std::string lastHybridWarning;
        if (hybridStatus.rfind("Restart required:", 0) == 0 || hybridStatus.find("fallback") != std::string::npos)
        {
            if (hybridStatus != lastHybridWarning)
                LOG_WARN("Hybrid: {}", hybridStatus);
            lastHybridWarning = hybridStatus;
        }
        else
            lastHybridWarning.clear();
        if (!finishedPicture)
        {
            if (ImGui::Checkbox("Generate before SR, apply after SR (DLSS)", &deferredDlss))
                config->DlssNrDeferredDlss = deferredDlss;
            HelpMarker("Compute NR at input resolution, upscale its changes with DLSS, then apply them after SR.\nExperimental: may flicker and adds GPU cost. Requires DLSS on DX12 or its bridges; does not support RR.\nOverrides Apply before SR. Disable Hold frame, Compare and Debug view.");
            if (deferredDlss && rayReconstruction)
                ImGui::TextWrapped("Generate before / apply after is unavailable with RR. Apply before SR "
                                   "controls NR placement.");
            else if (deferredDlss)
                ImGui::TextWrapped("Residual DLSS: %s", DlssNr::DeferredDlssStatus().c_str());
            ImGui::BeginDisabled(finishedPicture || !deferredDlss || rayReconstruction);
            bool residualFg = config->DlssNrResidualFg.value_or_default();
            if (ImGui::Checkbox("NR every second frame (NVIDIA Frame Generation, experimental)", &residualFg))
                config->DlssNrResidualFg = residualFg;
            HelpMarker("Run NR every other rendered frame and use NVIDIA Frame Generation (FG) to interpolate its changes.\nRequires the option above. Adds one rendered frame of latency and may misalign effects or UI.\nIf motion vectors are unavailable, each NR result is reused for two frames.");
            bool approxCamera = config->DlssNrResidualFgApproxCamera.value_or_default();
            if (ImGui::Checkbox("Allow approximate FG camera guides (experimental)", &approxCamera))
                config->DlssNrResidualFgApproxCamera = approxCamera;
            HelpMarker("Use estimated camera data when the game does not provide it. May cause artifacts during camera movement.");
            ImGui::EndDisabled();

        }
        else if (beforeSr)
            ImGui::TextWrapped("Pre-SR changes: %s", DlssNr::DeferredDlssStatus().c_str());

        // The toggle can be bound to a key, and nobody would think to look for it under Keybinds
        // unless told. Dimmed, because it is a note rather than a setting.
        ImGui::TextDisabled("Set the NR toggle shortcut under Keybinds.");

        bool applyModel = config->DlssNrApplyModel.value_or_default();
        if (ImGui::Checkbox("Apply the model", &applyModel))
            config->DlssNrApplyModel = applyModel;

        HelpMarker("Show or hide the NR effect. The model still runs when hidden.\nDisable Enable Neural Rendering to stop its GPU cost.");

        if (ImGui::Checkbox("Lift model pass limit (up to 30; expensive)", &unlockPasses))
            config->DlssNrUnlockPasses = unlockPasses;
        HelpMarker("Allow up to 30 passes instead of 3. More passes use more GPU time and VRAM; high values may crash the game.");

        ImGui::Spacing();
        ImGui::PushItemWidth(220.0f * menuResScale);

        ImGui::SeparatorText("NR Input Options");
        ImGui::Text("Size");

        // Any percentage, rather than a handful of steps somebody chose in advance. The lower bound
        // is 25%: below that the model is working on so little of the picture that its answer no
        // longer survives being enlarged onto it.
        // Applied when the handle is let go, not while it is moving.
        //
        // Every distinct value here is a different working size, and a different working size tears
        // down the scratch textures and rebuilds the model. Writing it on each pixel of a drag meant
        // dozens of rebuilds in a second, which is felt as the whole frame hitching. The slider still
        // reads live; only the commit waits.

        bool resolutionAuto = config->DlssNrModelResolutionAuto.value_or_default();
        const bool autoActive = resolutionAuto && (!beforeSr || rayReconstruction);

        int scalePercent = autoActive ? DlssNr::CurrentModelResolutionPercent()
                          : pendingScale >= 0
                               ? pendingScale
                               : (int) lroundf(config->DlssNrWorkingScale.value_or_default() * 100.0f);

        ImGui::BeginDisabled(autoActive);
        if (ImGui::SliderInt("Model resolution", &scalePercent, 25, 200, "%d%%"))
            pendingScale = scalePercent;

        // Captured right here, before the Reset button below becomes the new "last item" --
        // IsItemDeactivatedAfterEdit() only ever reports on whatever was most recently
        // submitted, so checking it after the button would report the button's state, not
        // the slider's release, and the commit below would never fire.
        const bool sliderReleased = ImGui::IsItemDeactivatedAfterEdit();

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##modelresolution"))
        {
            config->DlssNrWorkingScale = 1.0f;
            pendingScale = -1;
        }
        ImGui::EndDisabled();

        if (!autoActive && sliderReleased && pendingScale >= 0)
        {
            config->DlssNrWorkingScale = std::clamp(pendingScale, 25, 200) / 100.0f;
            pendingScale = -1;
        }

        HelpMarker("NR resolution relative to the image it processes. 50% halves width and height; 100% uses the full size.\nLower values reduce cost and fine detail. Above 100% increases cost. Game output resolution is unchanged.\nThe model averages its input 2x2 before its main network runs, so that network always works at half of this size: cost follows the halved size, and so does the finest detail it can add.");

        {
            unsigned int modelWidth = 0;
            unsigned int modelHeight = 0;
            DlssNr::CurrentModelSize(modelWidth, modelHeight);

            if (modelWidth != 0 && modelHeight != 0)
                ImGui::TextDisabled("Model input %ux%u; its main network runs at %ux%u.", modelWidth, modelHeight,
                                    (modelWidth + 1) / 2, (modelHeight + 1) / 2);
        }

        if (ImGui::Checkbox("Auto (post-SR only)", &resolutionAuto))
            config->DlssNrModelResolutionAuto = resolutionAuto;
        HelpMarker("When NR runs after SR -- Apply before SR off, or Ray Reconstruction, which always runs it after -- derive the working scale from the render:output ratio the upscaler itself already reconstructed detail at, instead of the slider above.\nThat output already reconstructed detail at that ratio, so NR running at the same reduced scale costs nothing extra to tune for. No effect while NR runs before SR -- the slider applies as usual.");

        if (autoActive)
            ImGui::TextDisabled("NR scale: %.2fx, derived from the upscaler's render:output ratio.",
                                scalePercent / 100.0f);
        else if (scalePercent > 100)
            ImGui::TextDisabled("NR scale: %.2fx. Higher resolution increases GPU cost.",
                                scalePercent / 100.0f);

        if (scalePercent > 100)
        {
            static const char* dsNames[] = { "FSR1", "Bicubic", "Catmull-Rom", "Lanczos2",
                                             "Lanczos3", "Kaiser2", "Kaiser3", "MAGIC" };
            int ds = (int) config->DlssNrScalingDownscaler.value_or_default();
            if (ds < 0 || ds >= IM_ARRAYSIZE(dsNames))
                ds = (int) Scaler::Lanczos3;

            if (ImGui::Combo("Downscaler (NR)", &ds, dsNames, IM_ARRAYSIZE(dsNames)))
                config->DlssNrScalingDownscaler = (Scaler) ds;

            HelpMarker("Filter used to reduce NR output when Model resolution exceeds 100%.\nSharper filters may introduce ringing around edges.");
        }

        ImGui::SeparatorText("NR Output Options");

        // Meaningful only when the model runs BELOW the frame's size. At 100% -- and above, where
        // supersampling composites its down-legged answer at native -- the residual collapses to the
        // model's own picture and the two modes are identical, so the control says so by going grey.
        {
            const bool reduced = config->DlssNrWorkingScale.value_or_default() < 0.999f;

            if (!reduced)
                ImGui::BeginDisabled();

            static const char* enlargeNames[] = { "Classic", "Matched residual", "NVIDIA residual" };
            int enlarge = (int) std::min(config->DlssNrTransfer.value_or_default(), 2u);

            if (ImGui::Combo("Upscale Mode", &enlarge, enlargeNames, IM_ARRAYSIZE(enlargeNames)))
                config->DlssNrTransfer = (uint32_t) enlarge;

            if (!reduced)
                ImGui::EndDisabled();

            HelpMarker("Below 100% model resolution: Classic enlarges the model output; Matched residual enlarges only its changes.\nMatched residual can reduce blur and colour shifts. NVIDIA residual enlarges the changes in OkLab (luminance as a ratio, chroma as a difference).\nNo effect at 100% or above.");

            if (!reduced)
                ImGui::BeginDisabled();

            static const char* upscaleMethodNames[] = { "Bilinear (fast)", "SGSR1" };
            int upscaleMethod = (int) std::min(config->DlssNrReducedUpscaleMethod.value_or_default(), 1u);

            if (ImGui::Combo("Upscale Method", &upscaleMethod, upscaleMethodNames, IM_ARRAYSIZE(upscaleMethodNames)))
                config->DlssNrReducedUpscaleMethod = (uint32_t) upscaleMethod;

            if (!reduced)
                ImGui::EndDisabled();

            HelpMarker("Below 100% model resolution: filter used to enlarge the model's answer back to native before it's applied.\nBilinear is the cheapest, softest, pre-SGSR1 default. SGSR1 does an edge-directed upscale of the answer instead. No effect at 100% or above.");
        }

        // Experimental. 0 off (soft knee), 1 Reversible curve + our composition, 2 Reversible curve +
        // pure-inverse replace, 3 Balanced+composed, 4 Balanced+replace (identity midtones + unclipped
        // highlights). Always shown.
        static const char* reversibleNames[] = { "Off (soft knee)", "Reversible curve + composed",
                                                 "Reversible curve + replace", "Balanced curve + composed",
                                                 "Balanced curve + replace" };
        int reversible = (int) config->DlssNrReversibleMode.value_or_default();
        if (reversible < 0 || reversible > 4)
            reversible = 0;
        if (ImGui::Combo("Final Image Composition (experimental)", &reversible, reversibleNames,
                         IM_ARRAYSIZE(reversibleNames)))
            config->DlssNrReversibleMode = (uint32_t) reversible;

        HelpMarker("Choose how HDR brightness is mapped for NR.\nSoft knee compresses highlights. Reversible curve uses a reversible mapping. Balanced preserves midtones and compresses highlights.\nComposed uses the strength control and the Highlight guard below (brightening only; darkening is not capped in Composed). Replace bypasses the strength control (the model's answer applies directly, uncomposited) but the same Highlight guard number still bounds it in both directions -- lower it if Replace flickers or shows banding near bright highlights.");

        if (reversible == 2 || reversible == 4)
        {
            float replaceDetail = config->DlssNrReplaceDetailStrength.value_or_default();
            if (ImGui::SliderFloat("Restore Sharpness", &replaceDetail, 0.0f, 2.0f, "%.2f"))
                config->DlssNrReplaceDetailStrength = replaceDetail;

            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##replacedetail"))
                config->DlssNrReplaceDetailStrength = 0.5f;

            HelpMarker("Sharpens fine edges and textures using brightness from the original frame. Helps when Final Image Composition uses a Replace mode and NR runs below 100% resolution, where the image can otherwise look soft.\nNo effect at 100% resolution or above, or when set to 0.");
        }

        ImGui::SeparatorText("Effect strength");

        float transfer = config->DlssNrTransferStrength.value_or_default();
        if (ImGui::SliderFloat("Detail strength", &transfer, 0.0f, 2.0f, "%.2f"))
            config->DlssNrTransferStrength = transfer;

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##detail"))
            config->DlssNrTransferStrength = 1.0f;

        HelpMarker("Overall NR detail strength: 0 = no effect, 1 = normal, above 1 = exaggerated.");

        float colour = config->DlssNrColourStrength.value_or_default();
        if (ImGui::SliderFloat("Colour strength", &colour, 0.0f, 4.0f, "%.2f"))
            config->DlssNrColourStrength = colour;

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##colour"))
            config->DlssNrColourStrength = 1.0f;

        HelpMarker("NR colour strength: 0 = preserve game colours, 1 = model colours, above 1 = stronger saturation.");

        ImGui::SeparatorText("Model passes");
        ImGui::TextWrapped("Settings apply when you release a slider.");
        static const char* styles[] = { "Standard", "Natural", "Cinematic" };
        static const char* inheritedStyles[] = { "Auto (inherit pass 1)", "Standard", "Natural", "Cinematic" };

        if (ImGui::TreeNodeEx("Pass 1", ImGuiTreeNodeFlags_DefaultOpen))
        {
            int style = (int) std::min(config->DlssNrStyle.value_or_default(), 2u);
            if (ImGui::Combo("Style", &style, styles, IM_ARRAYSIZE(styles)))
                config->DlssNrStyle = (uint32_t) style;
            HelpMarker("Select the appearance profile: Standard, Natural or Cinematic. Intensity controls its strength.");
            DeferredSlider("Intensity", &config->DlssNrIntensity, 0.0f, 2.0f, 1.0f);
            DeferredSlider("Local structure", &config->DlssNrLocalStructure, 0.0f, 2.0f, 1.0f);
            DeferredSlider("Local tone", &config->DlssNrLocalTone, 0.0f, 2.0f, 1.0f);
            DeferredSlider("Skin structure", &config->DlssNrSkinStructure, -1.0f, 2.0f, -1.0f);
            bool mask = config->DlssNrAutoMask.value_or_default();
            if (ImGui::Checkbox("Auto skin mask", &mask))
                config->DlssNrAutoMask = mask;
            HelpMarker("Use the model's learned skin selection to apply Skin structure without an authored mask.\nAccuracy varies. This is separate from the colour-based mask below.");
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Pass 2", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextWrapped("Defaults: inherit Pass 1; Local tone = 0. Reset restores these defaults.");
            InheritedProfileCombo("Style", &config->DlssNrPass2Style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
            DeferredSlider("Intensity", &config->DlssNrPass2Intensity, 0.0f, 2.0f, config->DlssNrIntensity.value_or_default(), "%.2f", true);
            DeferredSlider("Local structure", &config->DlssNrPass2LocalStructure, 0.0f, 2.0f, config->DlssNrLocalStructure.value_or_default(), "%.2f", true);
            DeferredSlider("Local tone", &config->DlssNrPass2LocalTone, 0.0f, 2.0f, 0.0f, "%.2f", true);
            DeferredSlider("Skin structure", &config->DlssNrPass2SkinStructure, -1.0f, 2.0f, config->DlssNrSkinStructure.value_or_default(), "%.2f", true);
            bool mask = config->DlssNrPass2AutoMask.has_value() ? config->DlssNrPass2AutoMask.value() : config->DlssNrAutoMask.value_or_default();
            if (ImGui::Checkbox("Auto skin mask", &mask))
                config->DlssNrPass2AutoMask = mask;
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##mask"))
                config->DlssNrPass2AutoMask = std::optional<bool> {};
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Pass 3", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextWrapped("Defaults: inherit Pass 1; Local tone = 0. Reset restores these defaults.");
            InheritedProfileCombo("Style", &config->DlssNrPass3Style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
            DeferredSlider("Intensity", &config->DlssNrPass3Intensity, 0.0f, 2.0f, config->DlssNrIntensity.value_or_default(), "%.2f", true);
            DeferredSlider("Local structure", &config->DlssNrPass3LocalStructure, 0.0f, 2.0f, config->DlssNrLocalStructure.value_or_default(), "%.2f", true);
            DeferredSlider("Local tone", &config->DlssNrPass3LocalTone, 0.0f, 2.0f, 0.0f, "%.2f", true);
            DeferredSlider("Skin structure", &config->DlssNrPass3SkinStructure, -1.0f, 2.0f, config->DlssNrSkinStructure.value_or_default(), "%.2f", true);
            bool mask = config->DlssNrPass3AutoMask.has_value() ? config->DlssNrPass3AutoMask.value() : config->DlssNrAutoMask.value_or_default();
            if (ImGui::Checkbox("Auto skin mask", &mask))
                config->DlssNrPass3AutoMask = mask;
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##mask"))
                config->DlssNrPass3AutoMask = std::optional<bool> {};
            ImGui::TreePop();
        }

        const unsigned int visiblePasses = std::clamp(config->DlssNrPasses.value_or_default(), 1u, passLimit);
        for (unsigned int pass = 3; pass < visiblePasses; ++pass)
        {
            auto& settings = config->DlssNrExtraPasses[pass - 3];
            if (!ImGui::TreeNode(std::format("Pass {}", pass + 1).c_str()))
                continue;
            ImGui::TextWrapped("Defaults: inherit Pass 1; Local tone = 0.");
            InheritedProfileCombo("Style", &settings.style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
            DeferredSlider("Intensity", &settings.intensity, 0.0f, 2.0f, config->DlssNrIntensity.value_or_default(), "%.2f", true);
            DeferredSlider("Local structure", &settings.structure, 0.0f, 2.0f, config->DlssNrLocalStructure.value_or_default(), "%.2f", true);
            DeferredSlider("Local tone", &settings.tone, 0.0f, 2.0f, 0.0f, "%.2f", true);
            DeferredSlider("Skin structure", &settings.skin, -1.0f, 2.0f, config->DlssNrSkinStructure.value_or_default(), "%.2f", true);
            bool mask = settings.autoMask.value_or(config->DlssNrAutoMask.value_or_default());
            if (ImGui::Checkbox("Auto skin mask", &mask))
                settings.autoMask = mask;
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##mask"))
                settings.autoMask = std::optional<bool> {};
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Advanced preset hints (effect unverified)"))
        {
            ImGui::TextWrapped("Experimental model hints; visual effect unverified. Use Style to select a profile.");
            static const char* presets[] = { "Default", "Preset 1", "Preset 2", "Preset 3" };
            static const char* inheritedPresets[] = { "Auto (inherit pass 1)", "Default", "Preset 1", "Preset 2", "Preset 3" };
            int preset = (int) std::min(config->DlssNrPreset.value_or_default(), 3u);
            if (ImGui::Combo("Pass 1 preset hint", &preset, presets, IM_ARRAYSIZE(presets)))
                config->DlssNrPreset = (uint32_t) preset;
            InheritedProfileCombo("Pass 2 preset hint", &config->DlssNrPass2Preset, inheritedPresets, IM_ARRAYSIZE(inheritedPresets));
            InheritedProfileCombo("Pass 3 preset hint", &config->DlssNrPass3Preset, inheritedPresets, IM_ARRAYSIZE(inheritedPresets));
            ImGui::TreePop();
        }
        ImGui::TextWrapped("Pass settings apply to SR and RR on DX12 and native Vulkan. The driver-proxy backend supports one pass.");

        ImGui::SeparatorText("Colour");

        if (ImGui::TreeNode("Skin and environment (final edit)"))
        {
            ImGui::TextWrapped("Select skin by colour and adjust the final NR effect separately for skin and scenery. Selection can be inaccurate; check Preview.");
            bool filter = config->DlssNrSkinProtection.value_or_default();
            if (ImGui::Checkbox("Separate skin / environment controls", &filter))
                config->DlssNrSkinProtection = filter;
            ImGui::BeginDisabled(!filter);
            bool tone = config->DlssNrSkinToneEnabled.value_or_default();
            if (ImGui::Checkbox("Allow skin tone / colour changes", &tone))
                config->DlssNrSkinToneEnabled = tone;
            HelpMarker("Allow NR colour changes in the selected skin region. Turn off to preserve its colour; detail can still change.");
            const auto slider = [](const char* label, auto& option) {
                float v = option.value_or_default();
                if (ImGui::SliderFloat(label, &v, 0.0f, 1.0f, "%.2f"))
                    option = v;
                ImGui::SameLine();
                const std::string resetId = std::string("Reset##") + label;
                if (ImGui::SmallButton(resetId.c_str()))
                    option = 1.0f;
                HelpMarker("NR strength in this region: 0 = no change, 1 = full effect.");
            };
            slider("Skin detail / lighting", config->DlssNrSkinDetail);
            ImGui::BeginDisabled(!tone);
            slider("Skin colour", config->DlssNrSkinColour);
            ImGui::EndDisabled();
            slider("Environment detail / lighting", config->DlssNrEnvironmentDetail);
            slider("Environment colour", config->DlssNrEnvironmentColour);
            bool preview = config->DlssNrShowSkinMask.value_or_default();
            if (ImGui::Checkbox("Preview colour-based mask", &preview))
                config->DlssNrShowSkinMask = preview;
            ImGui::EndDisabled();
            ImGui::TreePop();
        }

        ImGui::SeparatorText("Compare");

        // Freeze the frame the model works on, so a setting change re-renders it in place -- the only
        // clean way to A/B our own settings (a moving scene confounds every other comparison). See
        // design/frame-hold.md.
        bool held = config->DlssNrHoldFrame.value_or_default();
        if (ImGui::Checkbox("Hold frame", &held))
            config->DlssNrHoldFrame = held;

        HelpMarker("Freeze NR's input to compare its settings. The game's HUD and later effects may keep updating.\nDoes not re-run SR/RR or show changes to their settings. Turn off to resume.");

        bool frameStats = config->DlssNrFrameStats.value_or_default();
        if (ImGui::Checkbox("Log frame brightness stats", &frameStats))
            config->DlssNrFrameStats = frameStats;

        HelpMarker("Diagnostic. Every 2 seconds or so, writes a line to OptiScaler.log describing the frame NR is given: format, luminance percentiles, the game's exposure value and the white point in use.");

        bool kernelProfile = config->DlssNrKernelProfile.value_or_default();
        if (ImGui::Checkbox("Log NR kernel profile", &kernelProfile))
            config->DlssNrKernelProfile = kernelProfile;

        HelpMarker("Diagnostic. Every 4 seconds or so, writes a line to OptiScaler.log with the NVIDIA kernels one NR evaluation launched (fp8-named or plain fp16) and where its GPU time went, by kernel group. Approximate: chained kernels overlap.");

        static const char* compareNames[] = { "Off", "Side by side", "Wipe" };
        int compare = (int) config->DlssNrCompare.value_or_default();
        if (ImGui::Combo("Compare", &compare, compareNames, IM_ARRAYSIZE(compareNames)))
            config->DlssNrCompare = (uint32_t) compare;

        HelpMarker("Compare the original and NR result. Side by side fits both images; Wipe divides one full-size image.");

        if (compare != 0)
        {
            bool swap = config->DlssNrCompareSwap.value_or_default();
            if (ImGui::Checkbox("Swap sides", &swap))
                config->DlssNrCompareSwap = swap;
            HelpMarker("Swap the original and NR sides.");

            bool tags = config->DlssNrCompareTags.value_or_default();
            if (ImGui::Checkbox("Label the sides", &tags))
                config->DlssNrCompareTags = tags;

            HelpMarker("Display labels identifying the original and NR sides.");

            if (tags)
            {
                float tagScale = config->DlssNrTagScale.value_or_default();
                if (ImGui::SliderFloat("Label size", &tagScale, 0.5f, 5.0f, "%.1fx"))
                    config->DlssNrTagScale = std::clamp(tagScale, 0.5f, 5.0f);
            }

        }

        if (compare == 1)
        {
            float zoom = config->DlssNrCompareZoom.value_or_default();
            if (ImGui::SliderFloat("Zoom", &zoom, 1.0f, 2.0f, "%.2f"))
                config->DlssNrCompareZoom = std::clamp(zoom, 1.0f, 2.0f);

            HelpMarker("Side-by-side zoom: 1 = fit the whole image, 2 = fill each half by cropping the sides.");
        }

        if (compare == 2)
        {
            float split = config->DlssNrCompareSplit.value_or_default();
            if (ImGui::SliderFloat("Split", &split, 0.0f, 1.0f, "%.2f"))
                config->DlssNrCompareSplit = std::clamp(split, 0.0f, 1.0f);

            HelpMarker("Position of the comparison boundary. Swap sides reverses which image appears on each side.");
        }

        static const char* debugNames[] = { "Off", "Proxy (what the model sees)", "Model output (raw)",
                                            "Difference (amplified)" };
        int debugView = (int) config->DlssNrDebugView.value_or_default();
        if (ImGui::Combo("Debug view", &debugView, debugNames, IM_ARRAYSIZE(debugNames)))
            config->DlssNrDebugView = (uint32_t) debugView;

        HelpMarker("Show the model input, raw output, or a 20x amplified difference. Grey in Difference means no change.\nShown at the game's own brightness, so Model input brightness makes the view brighter or darker by the amount it changes what the model is given.");

        ImGui::PopItemWidth();
    }
}

} // namespace DlssNr

