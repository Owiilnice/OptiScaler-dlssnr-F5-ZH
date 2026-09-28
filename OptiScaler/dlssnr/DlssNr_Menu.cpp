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
    const std::string sliderLabel = std::string("模型输入亮度##") + idSuffix;
    if (ImGui::SliderFloat(sliderLabel.c_str(), &ev, minEv, maxEv, "%+.1f EV"))
        trim = EvToTrim(ev, neutral);

    ImGui::SameLine();

    // Deliberately always present rather than greyed at 0 EV: the safe value is one click away.
    const std::string resetLabel = std::string("重置##") + idSuffix;
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
        ImGui::TextDisabled("ini 中有 %u 个亮度锚点正在使用；在它们存在期间该滑块无效。",
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

    const std::string resetId = std::string("重置##") + label;
    if (ImGui::SmallButton(resetId.c_str()))
    {
        if (inheritReset)
            *opt = std::optional<float> {};
        else
            *opt = def;
        pending.erase(id);
        changed = true;
    }

    if (std::strcmp(label, "强度") == 0)
        HelpMarker("本通道的整体增强强度。1 = 默认；效果取决于配置文件。\n高于 1 的数值为实验性；运行时可能将其限制或忽略。");
    else if (std::strcmp(label, "局部结构") == 0)
        HelpMarker("向模型请求的精细细节与局部对比度（高频结构）。\n1 = 默认；高于 1 的数值为实验性。");
    else if (std::strcmp(label, "局部色调") == 0)
        HelpMarker("向模型请求的整体亮度与光照变化（低频色调）。\n后续通道默认为 0。高于 1 的数值为实验性。");
    else if (std::strcmp(label, "皮肤结构") == 0)
        HelpMarker("模型判定为皮肤的像素的精细细节。-1 跟随局部结构；0 减少皮肤细节。\n皮肤颜色单独控制。高于 1 的数值为实验性。");
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
    config->DlssNrApplyModel = true;
    config->DlssNrUnlockPasses = false;
    config->DlssNrPasses = passes;

    // Upscale Method, Upscale Mode and Final Image Composition are deliberately not set here: the Pre-SR/Post-SR
    // tiers set them, and writing them from a pass preset would clear that tier's highlight.
    // Reuse bottleneck is not set here either: it is a speed/quality choice per kernel set, not part of a look, so a
    // preset leaves both checkboxes as the user had them.
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
    { "高",   1u, 1.00f, 1u, -1.0f, 1.0f, 1.0f },
    { "中", 1u, 0.80f, 2u,  1.50f, -1.0f, -1.0f },
    { "低",    0u, 0.65f, 2u,  1.50f, -1.0f, -1.0f },
    { "土豆", 0u, 0.50f, 2u,  1.70f, -1.0f, -1.0f },
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
    if (auto ch = ScopedCollapsingHeader("DLSS 神经渲染"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        // Moved up here (out of its original spot just above the Model-resolution slider) so
        // the "Optimized Defaults" preset button, which sits earlier in the panel, can clear
        // an in-flight drag when it overwrites the setting. Same static-local lifetime either
        // way.
        static int pendingScale = -1;

        bool enabled = config->DlssNrEnabled.value_or_default();
        if (ImGui::Checkbox("启用神经渲染", &enabled))
            config->DlssNrEnabled = enabled;

        HelpMarker("使用 NR 模型增强光照与材质表现。「放置位置」决定在超分之前还是之后。\n需要 nvngx_dlssnr.dll 以及随附的 nvngx.dll_dlssnr.dll 辅助文件。");

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
            ImGui::TextDisabled("NR 关闭。");
        }
        else if (!DlssNr::IsRunning() && !vulkan)
        {
            const auto feature = State::Instance().currentFeature;
            const bool nativeVk = feature && feature->Api() == API::Vulkan && !feature->IsWithDx12();
            const char* reason = nativeVk ? DlssNr::FailureReasonVk() : DlssNr::FailureReason();

            if (reason[0] != 0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "本次会话已关闭：%s。", reason);
                ImGui::SameLine();

                if (nativeVk)
                    ImGui::TextUnformatted("重启游戏以重试原生 Vulkan NR。");
                else if (ImGui::SmallButton("重试"))
                    DlssNr::RetryAfterFailure();
            }
            else if (feature && feature->Api() == API::DX11 && !feature->IsWithDx12())
            {
                ImGui::TextWrapped("在 D3D11 下 NR 需要 D3D12 桥接。请选择标记为 w/Dx12 的超分器并重启。");
            }
            else if (nativeVk && config->DlssNrDeferredDlss.value_or_default())
            {
                ImGui::TextWrapped("禁用「超分前生成，超分后应用 (DLSS)」即可使用原生 Vulkan NR。");
            }
            else if (enabled)
                ImGui::TextUnformatted("等待超分器运行。");
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
                !config->DlssNrApplyModel.value_or_default() ? "  （模型运行中，效果已隐藏）" : "";

            if (ms.has_value())
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "运行中%s - 已耗时 %.2f ms%s",
                                   vulkan ? " 原生 Vulkan" : "", ms.value(), runSuffix);
            else if (vulkan)
                // Measured but not yet read: the first few frames are still in the query ring.
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "原生运行于 Vulkan - %llu 帧%s",
                                   DlssNr::FramesVk(), runSuffix);
            else
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "运行中.%s", runSuffix);

            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("NR 在 GPU 上从开始到结束的时间，包含其他工作运行时的等待。\n对比 FPS 以了解对游戏性能的影响。");
            if (finishedPicture)
                ImGui::TextDisabled("包含与其他 GPU 工作共用的时间。");

            if (!vulkan && DlssNr::BackendName()[0] != 0)
                ImGui::TextDisabled("模型后端: %s", DlssNr::BackendName());
        }

        ImGui::SeparatorText("多遍预设");
        if (PresetButton("1 遍", PassPresetActive(config, 1u)))
            ApplyPassPreset(config, 1u);
        ImGui::SameLine();
        if (PresetButton("2 遍", PassPresetActive(config, 2u)))
            ApplyPassPreset(config, 2u);
        ImGui::SameLine();
        if (PresetButton("3 遍", PassPresetActive(config, 3u)))
            ApplyPassPreset(config, 3u);
        HelpMarker("为所选遍数套用本分支推荐的起点：FP8 精度与游戏曝光白点。「放大方式」「放大模式」和「最终图像合成」保持不变；\n这些请使用超分前或超分后预设。2 遍还会设置第 2 遍的覆盖项；3 遍会设置第 2 遍和第 3 遍的覆盖项。\n会覆盖下方的设置；此处未列出的项目，包括「NR 遍位于:」，一律保持你的当前设置。\n绿色按钮表示当前生效的遍数；更改遍数，或更改某一遍的「风格」「强度」「局部结构」「局部色调」或「皮肤结构」，都会清除它。");

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

            if (ImGui::SliderInt("模型遍数", &passes, 1, (int) passLimit,
                                 passes == 1 ? "%d（普通）" : "%dx 模型开销"))
                config->DlssNrPasses = (uint32_t) std::clamp(passes, 1, (int) passLimit);

            ImGui::PopStyleColor(2);

            // Reset's own label stays plain text -- placed after PopStyleColor so the
            // passes-based warning colour above doesn't tint it too.
            ImGui::SameLine();
            if (ImGui::SmallButton("重置##modelpasses"))
                config->DlssNrPasses = 1u;

            HelpMarker("对图像重复处理。遍数越多，效果越强，GPU 开销也越大。\n每一遍都有各自的设置和历史。建议从 1 开始。");
        }

        {
            // Disabled rather than hidden at Passes == 1: the control exists, it just has nothing to
            // do yet (there is no boundary between passes to damp), which is a clearer statement
            // than making it vanish and reappear as Passes changes.
            const bool noBoundary = config->DlssNrPasses.value_or_default() <= 1;
            ImGui::BeginDisabled(noBoundary);
            float feedback = config->DlssNrPassFeedback.value_or_default();
            if (ImGui::SliderFloat("通道反馈", &feedback, 0.0f, 1.0f,
                                   feedback >= 1.0f ? "%.2f（完整，当前行为）" : "%.2f"))
                config->DlssNrPassFeedback = std::clamp(feedback, 0.0f, 1.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton("重置##passfeedback"))
                config->DlssNrPassFeedback = 1.0f;
            ImGui::EndDisabled();

            HelpMarker("下一遍实际接收到上一遍原始结果的比例。\n" "\n" "1.0 是所有配置一直以来的行为：下一遍拿到完整结果。第一遍之后的每一遍，\n" "看到的都是模型从未训练过的输入——它自己上一次的输出，而不是原始帧——\n" "因此数值越低，各遍就越贴近该训练分布，而不会随着每增加一遍越漂越远，代价是累积编辑量更小。\n" "\n" "遍数 = 1 时无效：没有边界需要抑制。");
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

        ImGui::SeparatorText("超分前预设");
        tierRow("pre", true);
        HelpMarker("用于在超分辨率之前以较低模型分辨率运行 NR 的画质档位，从 High（100%，最佳画质）到 Potato（50%，最省性能）。\n每档都会设置「放大模式」「放大方式」「模型分辨率」和「最终图像合成」（High 还会把「细节」和「色彩强度」设为 1；Medium、Low 和 Potato 还会设置「恢复锐度」），\n并关闭「自动模型分辨率」以使分辨率生效。\n它还会把「NR 遍位于:」设为「超分辨率之前」。此处未列出的项目一律保持你的当前设置。\n绿色按钮表示当前生效的档位；更改「NR 遍位于:」「放大模式」「放大方式」「模型分辨率」或「最终图像合成」都会清除它。");

        ImGui::SeparatorText("超分后预设");
        tierRow("post", false);
        HelpMarker("用于在超分辨率之后以较低模型分辨率运行 NR 的画质档位，从 High（100%，最佳画质）到 Potato（50%，最省性能）。\n每档都会设置「放大模式」「放大方式」「模型分辨率」和「最终图像合成」（High 还会把「细节」和「色彩强度」设为 1；Medium、Low 和 Potato 还会设置「恢复锐度」），\n并关闭「自动模型分辨率」以使分辨率生效。\n它还会把「NR 遍位于:」设为「超分辨率之后」。此处未列出的项目一律保持你的当前设置。\n绿色按钮表示当前生效的档位；更改「NR 遍位于:」「放大模式」「放大方式」「模型分辨率」或「最终图像合成」都会清除它。");

        ImGui::SeparatorText("HDR 输入");
        ImGui::TextDisabled("HDR 输入设置。调整呈现给 NR 的亮度范围。");

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

            static const char* sourceNames[] = { "手动纸白", "游戏曝光",
                                                 "扫描曝光（实验性）",
                                                 "来自 HDR 帧的自动曝光" };

            int source = (int) config->DlssNrWhitePointSource.value_or_default();

            if (source < 0 || source > 3)
                source = 0;

            if (ImGui::Combo("白点来源", &source, sourceNames, IM_ARRAYSIZE(sourceNames)))
            {
                config->DlssNrWhitePointSource = (uint32_t) source;

                // Nothing else to set. The scan asks the source whether it is wanted, so choosing
                // it here is the whole of switching it on -- there is no second flag to keep in
                // step, and so no way for the two to disagree.
            }

            HelpMarker("手动：使用「纸白」。游戏曝光：使用游戏提供的曝光值。\n扫描曝光：从游戏缓冲区估算；需要校准，且可能选错缓冲区。\n自动曝光：OptiScaler 自行测量线性 HDR 帧，因此无需游戏提供任何信息。");

            // Availability, in colour, for the option currently chosen.
            if (source == 1)
            {
                if (!vk && ex.seenFrames == 0)
                    ImGui::TextDisabled("等待帧…");
                else if (!haveExposure)
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                       "无可用游戏曝光。使用手动纸白。");
                else if (ex.exposure > 1e-6f)
                {
                    const float baseWhitePoint = ex.preExposure / ex.exposure;
                    const auto trimAnchors =
                        DlssNrTrim::Parse(config->DlssNrGameExposureTrimAnchors.value_or_default());
                    const float trim = DlssNrTrim::TrimForKey(
                        baseWhitePoint, config->DlssNrWhitePointTrim.value_or_default(), trimAnchors, false);
                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "游戏曝光 %.4f  ->  模型白点 %.2f%s", ex.exposure,
                                       baseWhitePoint * trim,
                                       ex.offeredNow ? "" : "  （保持：本帧缺失）");
                }
                else
                    ImGui::TextDisabled("正在读取曝光...");
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
                                       "场景亮度 %.3f  ->  模型白点 %.2f",
                                       0.18f / (0.82f * autoEx.exposure), baseWhitePoint * trim);
                    HelpMarker("在 NR 运行前从线性 HDR 帧测得（原始曝光值见下方）。\n" "模型白点是把画面缩放到的亮度水平：达到或超过它的都算作纯白。");
                    ImGui::TextDisabled("自动曝光 %.4f", autoEx.exposure);
                }
                else
                    ImGui::TextDisabled("正在计算自动曝光...");
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
                                           "未找到曝光候选。");
                    else
                        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                           "%u 个候选；在明暗区域之间移动以测试它们。",
                                           watching);
                }
                else if (!haveAnchor)
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                       "已找到曝光候选。调整「纸白」，然后选择「在此锚定」。");
                // Once anchored, the scan -> white point readout sits above the sliders below; it is
                // not repeated up here.
            }
            else if (haveExposure)
            {
                ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                   "游戏曝光可用。");
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
                                       "扫描 %.5f  ->  白点 %.2f   （%u 个点%s）", liveScan, w,
                                       (unsigned) anchors.size(), anchors.size() == 1 ? "" : "");
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
                    snprintf(lbl, sizeof(lbl), "纸白（正在编辑点 %d）", selectedAnchor + 1);
                else
                    snprintf(lbl, sizeof(lbl), "纸白");

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

                HelpMarker("调整所选的校准点，或为下一个点设定数值。\n使用「在此锚定」保存当前光照条件。");
            }

            // The trim multiplies the interpolated result, and in the steady state it is the control
            // that stands in for paper white: adjust it until the picture looks right in the current
            // light, then Anchor bakes that trimmed value into a new point and resets the trim to 1.
            if (!anchors.empty())
            {
                float trim = config->DlssNrScanTrim.value_or_default();

                if (ImGui::SliderFloat("微调 (× 扫描值)", &trim, 0.25f, 4.0f, "%.2fx",
                                       ImGuiSliderFlags_Logarithmic))
                    config->DlssNrScanTrim = std::clamp(trim, 0.25f, 4.0f);

                ImGui::SameLine();

                if (ImGui::SmallButton("重置##scantrim"))
                    config->DlssNrScanTrim = 1.0f;

                HelpMarker("对已校准的白点进行倍乘。「在此锚定」会保存调整后的数值，并将此倍率重置为 1。");
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
                               "交给 NR 的画面亮度，相对于游戏报告的曝光值。\n" "+ 更亮，- 更暗；0 EV 直接沿用游戏的曝光值。\n" "过亮会裁切高光；过暗会掩盖阴影细节。");
        }
        else if (wpSource == 3)
        {
            // The scale stays centred on a 5x Trim (0 EV), the old default from the PR this came from. The default is
            // +1.5 EV for every game; see DlssNr_AutoTrimDefault.h for the measurements. It is independent of the Game exposure Trim.
            RenderTrimEvSlider(config->DlssNrAutoExposureTrim, 5.0f,
                               DlssNrTrim::Parse(config->DlssNrAutoExposureTrimAnchors.value_or_default()).size(),
                               "autoexposure",
                               "交给 NR 的画面亮度。+ 更亮，- 更暗。\n" "在你手动调整之前，它在所有游戏中都是 +1.5 EV。\n" "重置会恢复该默认值。\n" "过亮会裁切高光或使阴影偏色；过暗会掩盖阴影细节。\n" "OptiScaler 在 NR 运行前自行测量线性 HDR 帧。\n" "自动曝光在 D3D12 和 Vulkan 上可用。",
                               DlssNrAutoTrim::kDefaultTrim);

            // Following the game's own exposure (DlssNr_FollowGame.h): on by default for a known unexposed game
            // (DlssNr_GameDefaults.h). Vulkan follows from the host value, a few frames behind the game.
            {
                const bool followVk = DlssNr::IsRunningVk();
                bool follow = DlssNr::FollowGameOn(*config);

                if (ImGui::Checkbox("跟随游戏的曝光", &follow))
                    config->DlssNrAutoExposureFollowGame = follow;

                HelpMarker("适用于在应用自身曝光之前就把画面交出来的游戏：已知会这样做的游戏（如 RDR2）默认开启，\n其余游戏默认关闭。自动模式会在游玩最初几秒内学习自身测光与游戏曝光的关系，\n随后跟随游戏的曝光，因此亮度会与游戏完全同步：过场、菜单、淡入淡出。亮度滑块\n保持原有含义。对于自行处理曝光的游戏（大多数游戏）请保持关闭：\n否则会把它们的曝光叠加两次。在 Vulkan 上它会比游戏滞后几帧。");

                const auto followStatus =
                    followVk ? DlssNr::FollowGameExposureStatusVk() : DlssNr::FollowGameExposureStatus();
                const auto& calibration = DlssNrFollowGame::Instance();

                if (!follow)
                    ImGui::TextDisabled("关闭");
                else if (!followStatus.gameExposureSeen)
                    ImGui::TextDisabled("暂不可用: 游戏未提供曝光");
                else if (!calibration.Locked())
                    ImGui::TextDisabled("正在学习校准…（%u/%u）", calibration.Readings(),
                                        DlssNrFollowGame::kWindow);
                else
                    ImGui::TextDisabled("相对游戏曝光值的校准 %+.2f EV%s", calibration.OffsetEv(),
                                        followStatus.following ? "；跟随中" : "；未跟随");

                // The calibration is learned once per session; this learns it again.
                if (ImGui::SmallButton("重新校准##autoexposure"))
                {
                    DlssNrFollowGame::Instance().Reset();
                    LOG_INFO("DLSS-NR automatic exposure: re-calibration requested");
                }

                HelpMarker("重新学习与游戏曝光之间的校准关系，例如当校准是在过场动画或加载画面中\n" "学到的时候。此期间会暂时使用普通的自动模式（约 2 秒）。\n" "请在普通白天场景中重新校准，而不是雪地、夜晚或室内：在那里学到的亮度\n" "会在整个游戏过程中沿用。");
            }

            float protection = config->DlssNrAutoExposureShadowProtection.value_or_default();
            if (ImGui::SliderFloat("忽略明亮高光", &protection, 0.0f, 100.0f, "%.0f%%"))
                config->DlssNrAutoExposureShadowProtection = std::clamp(protection, 0.0f, 100.0f);

            HelpMarker("防止天空、灯光和反射使画面其余部分变暗。\n" "0% 表示按原样对整帧取平均；100% 表示最不把明亮区域计入。\n" "黑边和黑色边框始终被排除。");
        }
        else
        {
            // Logarithmic, because the useful range is not linear. A quarter to 2000: the low end
            // because a frame the game already tone mapped wants roughly 1, the high end because
            // there is no principled ceiling -- this is a divisor on an open-ended linear buffer, and
            // how far up a given game needs to go is a property of that game's exposure rather than
            // of anything that can be bounded here. One tester was still improving at 100.
            float wpScale = config->DlssNrWhitePointScale.value_or_default();

            if (ImGui::SliderFloat("纸白", &wpScale, 0.25f, 2000.0f, "%.2fx",
                                   ImGuiSliderFlags_Logarithmic))
                config->DlssNrWhitePointScale = wpScale;

            ImGui::SameLine();
            if (ImGui::SmallButton("重置##paperwhite"))
                config->DlssNrWhitePointScale = 1.0f;

        HelpMarker("用于为 NR 准备 HDR 色彩的亮度参考。数值越高，模型输入越暗；越低越亮。\n若 NR 丢失细节或产生偏色，请调整此项。");
        }

        // Highlight guard, directly under the white point / trim -- it bounds the model's edit and
        // belongs with the exposure controls it works alongside.
        float maxRatio = config->DlssNrMaxRatio.value_or_default();
        if (ImGui::SliderFloat("高光保护", &maxRatio, 1.0f, MaxHighlightGuard, "%.1fx"))
            config->DlssNrMaxRatio = maxRatio;

        ImGui::SameLine();
        if (ImGui::SmallButton("重置##guard"))
            config->DlssNrMaxRatio = 2.0f;

        HelpMarker("限制 NR 最多能把像素提亮多少；变暗不受此限制。数值越低，高光变化越受限；越高则允许越多。\n替换模式同样会限制变暗——这是另一道出于不同原因的防线。");

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
                    ImGui::Checkbox("显示曝光计", &meter))
                    config->DlssNrScanMeter = meter;

                HelpMarker("显示扫描到的曝光值和一个色彩指示器。仅用于显示；不改变图像。");

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

                if (ImGui::Button("在此锚定"))
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

                HelpMarker("将当前曝光和白点保存为一个校准点。\n第一个点调整「纸白」，后续光照条件用「微调」。最多 8 个点。");

                if (!isSource)
                    ImGui::TextDisabled("扫描曝光不是当前选定的白点来源。");

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
                        snprintf(row, sizeof(row), "%s 扫描 %.4f  ->  白点 %.2f%s",
                                 ((int) i == active && isSource) ? ">" : "  ", anchors[i].scan,
                                 anchors[i].white, sel ? "   [编辑中]" : "");

                        // Click selects the row (slider edits it); click again deselects (slider
                        // returns to the live unanchored point).
                        if (ImGui::Selectable(row, sel))
                            selectedAnchor = sel ? -1 : (int) i;

                        ImGui::PopID();
                    }

                    ImGui::TextDisabled("选择一行进行编辑；再次选择则取消。> 标记当前活动的点。");
                }

                // The direction flag only means anything with a single point; with two or more the
                // direction the white point moves is already fixed by the data.
                if (anchors.size() == 1)
                {
                    bool inverted = config->DlssNrScanInverted.value_or_default();
                    if (ImGui::Checkbox("反转曝光跟踪", &inverted))
                        config->DlssNrScanInverted = inverted;

                    HelpMarker("反转扫描曝光改变白点的方式。仅在只有一个校准点时需要。");
                }

                // The scan -> white point readout is shown above the sliders now, not here.

                // Everything below is read-out rather than control: what the scan is looking at and
                // how to tell whether it found the right thing. Folded away because the two decisions
                // that matter -- anchor, and which way the number runs -- are above it.
                if (ImGui::TreeNode("高级"))
                {

                    const auto found = DlssNr::ExposureScan::Report();
                    const char* why = DlssNr::ExposureScan::Status();

                    if (found.empty())
                    {
                        ImGui::TextDisabled("%s", why != nullptr && why[0] != 0
                                                      ? why
                                                      : "未找到曝光候选。");
                    }
                    else
                    {
                        for (size_t i = 0; i < found.size(); ++i)
                        {
                            const auto& c = found[i];

                            if (c.reads == 0)
                            {
                                ImGui::TextDisabled("%zu. %s —— 尚未读取", i + 1, c.shape.c_str());
                                continue;
                            }

                            // Moving is the whole signal, so it is the thing that is coloured.
                            ImGui::TextColored(c.moves ? ImVec4(0.45f, 0.8f, 0.45f, 1.0f)
                                                       : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                                               "%zu. %s = %.5f  （实测 %.5f..%.5f） %s", i + 1,
                                               c.shape.c_str(), c.latest, c.lowest, c.highest,
                                               c.moves ? "移动中" : "暂时平稳");
                        }

                        ImGui::TextDisabled("在明暗区域之间移动以检查曝光跟踪。");
                        ImGui::TextDisabled("只增不减的数值可能是个计数器。");
                    }

                    ImGui::TreePop();
                }
            }
        }


        }

        ImGui::SeparatorText("NR 选项");

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
        static const char* placementNames[] = { "超分辨率之后", "超分辨率之前",
                                                 "成品画面" };
        int placement = finishedPicture ? 2 : (beforeSr ? 1 : 0);

        if (deferredActive)
            ImGui::BeginDisabled();
        if (ImGui::Combo("NR 遍位于:", &placement, placementNames, IM_ARRAYSIZE(placementNames)))
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

        HelpMarker("选择 NR 在管线中的运行位置。\n超分辨率之后（默认）：待 SR 完成画面放大后再应用 NR。\n超分辨率之前：改为对放大前的较小图像应用 NR。当游戏的光线重建处于活动状态时无效——RR 总是在 RR+SR 之后运行 NR，不受支持的输入布局也会回退到超分之后。\n成品画面：在游戏完成光照与特效之后应用 NR，可能有助于改善绿色噪点。在原生 DirectX 12 游戏中无论帧生成开关都可用（SDR、HDR10、scRGB），也可能改变 HUD 和菜单。");

        if (finishedPicture && enabled)
        {
            const auto feature = State::Instance().currentFeature;
            if (feature && (feature->Api() != API::DX12 || feature->IsWithDx12()))
                ImGui::TextWrapped("此选项需要原生 DirectX 12 游戏。");
            else
                ImGui::TextWrapped("%s", DlssNr::FinishedPictureStatus().c_str());
        }

        // Nested under Finished Picture: a second, independent axis (generate the changes at the
        // smaller pre-SR size vs. at the finished picture's own size), not a fourth top-level
        // placement -- progressive disclosure, same as every other mode-gated control in this file.
        if (finishedPicture)
        {
            if (ImGui::Checkbox("在超分辨率之前运行模型", &beforeSr))
            {
                config->DlssNrRunBeforeSr = beforeSr;
                config->DlssNrDeferredDlss = false;
            }
            HelpMarker("以较小的输入尺寸运行模型，用 DLSS 放大其变化量，再应用到成品画面。\n实验性：色彩传递是近似的，观感可能不同。需要 DLSS SR；不支持 RR。");
        }

        bool deferredDlss = config->DlssNrDeferredDlss.value_or_default();
        int precisionChoice = config->DlssNrPrecision.value_or_default() == 4 ? 1 : 0;
        const char* precisions[] = { "NVIDIA（FP8）", "实验性（FP8+NVFP4 混合）" };
        if (ImGui::Combo("模型精度", &precisionChoice, precisions, IM_ARRAYSIZE(precisions)))
            config->DlssNrPrecision = precisionChoice == 1 ? 4u : 0u;
        HelpMarker("NVIDIA：原始 FP8 模型（默认），部分敏感运算保持更高精度。\n实验性：此分支面向 RTX 50 显卡的 FP8+NVFP4 混合方案；输出可能略有差异。");
        // One setting per kernel set: the fp8 kernels (NVIDIA's DLL and fp8-based builds) and the plain FP16 kernels (used by some modified DLSS-NR DLLs).
        // Only the one for the kernels actually running is used.
        const char* kernelSet = DlssNrNative::VitKernelSet();
        bool vitReuse = config->DlssNrVitEvery.value_or_default() > 1;
        if (ImGui::Checkbox("复用瓶颈: FP8 内核", &vitReuse))
            config->DlssNrVitEvery = vitReuse ? 2u : 1u;
        HelpMarker("每隔一帧才重新计算模型最粗的层级（其 32x18 瓶颈），中间复用上一次的结果，\n" "大约可节省模型 GPU 耗时的十分之一。\n" "该层级变化缓慢，因此画面通常几乎没有差别，但快速镜头移动时可能略显柔和。场景切换时始终重新计算。多遍时，所有遍都在同一帧上计算，并在下一帧统一复用。\n" "默认开启。立即生效，仅适用于 NVIDIA 自家模型。\n" "当模型运行 NVIDIA 的 FP8 内核（NVIDIA 的 DLL 及基于 FP8 的构建）时使用。");
        bool vitReusePlain = config->DlssNrVitEveryPlain.value_or_default() > 1;
        if (ImGui::Checkbox("复用瓶颈: 纯 FP16 内核", &vitReusePlain))
            config->DlssNrVitEveryPlain = vitReusePlain ? 2u : 1u;
        HelpMarker("与上述相同，用于模型运行纯 FP16 内核时（某些修改版 DLSS-NR DLL 使用）。\n默认开启。");
        ImGui::Text("正在使用的内核集: %s", kernelSet);
        if (DlssNrNative::VitPlainKernels() ? vitReusePlain : vitReuse)
            ImGui::TextUnformatted(("瓶颈复用: " + DlssNrNative::VitStatus()).c_str());
        if (precisionChoice > 0)
        {
            ImGui::TextUnformatted(enabled && DlssNrNative::IsActive() ? "混合: 活动" : "混合: 不活动");
            ImGui::TextWrapped("加载可能使游戏暂停，看起来像卡死。请稍候。");
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
            if (ImGui::Checkbox("超分前生成，超分后应用 (DLSS)", &deferredDlss))
                config->DlssNrDeferredDlss = deferredDlss;
            HelpMarker("以输入分辨率计算 NR，用 DLSS 放大其变化量，再在 SR 之后应用。\n实验性：可能闪烁并增加 GPU 开销。需要 DX12 上的 DLSS 或其桥接；不支持 RR。\n会覆盖「超分前应用」。请禁用「保持帧」「对比」和「调试视图」。");
            if (deferredDlss && rayReconstruction)
                ImGui::TextWrapped("使用 RR 时无法「超分前生成 / 超分后应用」。NR 的位置由「超分前应用」控制。");
            else if (deferredDlss)
                ImGui::TextWrapped("残差 DLSS: %s", DlssNr::DeferredDlssStatus().c_str());
            ImGui::BeginDisabled(finishedPicture || !deferredDlss || rayReconstruction);
            bool residualFg = config->DlssNrResidualFg.value_or_default();
            if (ImGui::Checkbox("每隔一帧运行 NR（NVIDIA 帧生成，实验性）", &residualFg))
                config->DlssNrResidualFg = residualFg;
            HelpMarker("每隔一个渲染帧运行 NR，并使用 NVIDIA 帧生成 (FG) 插值其变化。\n需要上面的选项。会引入一个渲染帧的延迟，并可能使特效或 UI 错位。\n若运动矢量不可用，每个 NR 结果会被复用于两帧。");
            bool approxCamera = config->DlssNrResidualFgApproxCamera.value_or_default();
            if (ImGui::Checkbox("允许近似的 FG 相机引导（实验性）", &approxCamera))
                config->DlssNrResidualFgApproxCamera = approxCamera;
            HelpMarker("当游戏未提供相机数据时使用估算值。相机移动时可能产生伪影。");
            ImGui::EndDisabled();

        }
        else if (beforeSr)
            ImGui::TextWrapped("超分前更改: %s", DlssNr::DeferredDlssStatus().c_str());

        // The toggle can be bound to a key, and nobody would think to look for it under Keybinds
        // unless told. Dimmed, because it is a note rather than a setting.
        ImGui::TextDisabled("在「按键绑定」中设置 NR 切换快捷键。");

        bool applyModel = config->DlssNrApplyModel.value_or_default();
        if (ImGui::Checkbox("应用模型效果", &applyModel))
            config->DlssNrApplyModel = applyModel;

        HelpMarker("显示或隐藏 NR 效果。隐藏时模型仍会运行。\n要停止其 GPU 开销，请禁用「启用神经渲染」。");

        if (ImGui::Checkbox("解除模型遍数上限（最高 30；开销很大）", &unlockPasses))
            config->DlssNrUnlockPasses = unlockPasses;
        HelpMarker("允许最多 30 遍而非 3 遍。遍数越多，占用的 GPU 时间和显存越多；数值过高可能导致游戏崩溃。");

        ImGui::Spacing();
        ImGui::PushItemWidth(220.0f * menuResScale);

        ImGui::SeparatorText("NR 输入选项");
        ImGui::Text("尺寸");

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
        if (ImGui::SliderInt("模型分辨率", &scalePercent, 25, 200, "%d%%"))
            pendingScale = scalePercent;

        // Captured right here, before the Reset button below becomes the new "last item" --
        // IsItemDeactivatedAfterEdit() only ever reports on whatever was most recently
        // submitted, so checking it after the button would report the button's state, not
        // the slider's release, and the commit below would never fire.
        const bool sliderReleased = ImGui::IsItemDeactivatedAfterEdit();

        ImGui::SameLine();
        if (ImGui::SmallButton("重置##modelresolution"))
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

        HelpMarker("NR 分辨率相对于它所处理的图像。50% 表示宽高各减半；100% 表示使用完整尺寸。\n数值越低，开销和精细细节都越少。高于 100% 会增加开销。游戏输出分辨率不变。\n模型在主干网络运行前会对输入做 2x2 平均，因此该网络始终在此尺寸的一半上工作：\n开销随减半后的尺寸变化，它能添加的最精细细节也一样。");

        {
            unsigned int modelWidth = 0;
            unsigned int modelHeight = 0;
            DlssNr::CurrentModelSize(modelWidth, modelHeight);

            if (modelWidth != 0 && modelHeight != 0)
                ImGui::TextDisabled("模型输入 %ux%u；其主网络以 %ux%u 运行。", modelWidth, modelHeight,
                                    (modelWidth + 1) / 2, (modelHeight + 1) / 2);
        }

        if (ImGui::Checkbox("自动（仅超分后）", &resolutionAuto))
            config->DlssNrModelResolutionAuto = resolutionAuto;
        HelpMarker("当 NR 在 SR 之后运行时——「超分前应用」关闭，或使用光线重建（RR 总是在其后运行 NR）——从超分器自身已用于重建细节的渲染:输出比例推得工作缩放，\n而不是使用上面的滑块。该输出已按此比例重建了细节，因此 NR 以同样的缩减比例运行无需额外调校。\n当 NR 在 SR 之前运行时无效——此时滑块照常生效。");

        if (autoActive)
            ImGui::TextDisabled("NR 缩放: %.2fx，由超分器自身的渲染:输出比例推得。",
                                scalePercent / 100.0f);
        else if (scalePercent > 100)
            ImGui::TextDisabled("NR 缩放: %.2fx。分辨率越高，GPU 开销越大。",
                                scalePercent / 100.0f);

        if (scalePercent > 100)
        {
            static const char* dsNames[] = { "FSR1", "Bicubic", "Catmull-Rom", "Lanczos2",
                                             "Lanczos3", "Kaiser2", "Kaiser3", "MAGIC" };
            int ds = (int) config->DlssNrScalingDownscaler.value_or_default();
            if (ds < 0 || ds >= IM_ARRAYSIZE(dsNames))
                ds = (int) Scaler::Lanczos3;

            if (ImGui::Combo("降采样器 (NR)", &ds, dsNames, IM_ARRAYSIZE(dsNames)))
                config->DlssNrScalingDownscaler = (Scaler) ds;

            HelpMarker("当「模型分辨率」超过 100% 时用于缩减 NR 输出的滤镜。\n更锐利的滤镜可能在边缘产生振铃。");
        }

        ImGui::SeparatorText("NR 输出选项");

        // Meaningful only when the model runs BELOW the frame's size. At 100% -- and above, where
        // supersampling composites its down-legged answer at native -- the residual collapses to the
        // model's own picture and the two modes are identical, so the control says so by going grey.
        {
            const bool reduced = config->DlssNrWorkingScale.value_or_default() < 0.999f;

            if (!reduced)
                ImGui::BeginDisabled();

            static const char* enlargeNames[] = { "经典", "匹配残差", "NVIDIA 残差" };
            int enlarge = (int) std::min(config->DlssNrTransfer.value_or_default(), 2u);

            if (ImGui::Combo("放大模式", &enlarge, enlargeNames, IM_ARRAYSIZE(enlargeNames)))
                config->DlssNrTransfer = (uint32_t) enlarge;

            if (!reduced)
                ImGui::EndDisabled();

            HelpMarker("模型分辨率低于 100% 时：经典方式放大模型输出；匹配残差只放大其变化量。\n匹配残差可减轻模糊和偏色。NVIDIA 残差在 OkLab 空间放大变化（亮度按比例，色度按差值）。\n在 100% 及以上无效。");

            if (!reduced)
                ImGui::BeginDisabled();

            static const char* upscaleMethodNames[] = { "双线性（快速）", "SGSR1" };
            int upscaleMethod = (int) std::min(config->DlssNrReducedUpscaleMethod.value_or_default(), 1u);

            if (ImGui::Combo("放大方式", &upscaleMethod, upscaleMethodNames, IM_ARRAYSIZE(upscaleMethodNames)))
                config->DlssNrReducedUpscaleMethod = (uint32_t) upscaleMethod;

            if (!reduced)
                ImGui::EndDisabled();

            HelpMarker("模型分辨率低于 100% 时：在应用前把模型结果放大回原生分辨率所用的滤镜。\n双线性是最便宜、最柔和的 SGSR1 之前默认值。SGSR1 则对结果做边缘导向放大。在 100% 及以上无效。");
        }

        // Experimental. 0 off (soft knee), 1 Reversible curve + our composition, 2 Reversible curve +
        // pure-inverse replace, 3 Balanced+composed, 4 Balanced+replace (identity midtones + unclipped
        // highlights). Always shown.
        static const char* reversibleNames[] = { "关闭（软拐点）", "可逆曲线 + 合成",
                                                 "可逆曲线 + 替换", "均衡曲线 + 合成",
                                                 "均衡曲线 + 替换" };
        int reversible = (int) config->DlssNrReversibleMode.value_or_default();
        if (reversible < 0 || reversible > 4)
            reversible = 0;
        if (ImGui::Combo("最终图像合成（实验性）", &reversible, reversibleNames,
                         IM_ARRAYSIZE(reversibleNames)))
            config->DlssNrReversibleMode = (uint32_t) reversible;

        HelpMarker("选择 NR 的 HDR 亮度映射方式。\n软拐点会压缩高光。可逆曲线使用可逆映射。均衡模式保留中间调并压缩高光。\n合成模式使用强度控制和下方的「高光保护」（仅限制提亮；合成模式下变暗不受限）。替换模式绕过强度控制（模型结果直接应用，不做合成），但同一个「高光保护」数值仍会在两个方向上限制它——若替换模式在明亮高光附近闪烁或出现色带，请调低该值。");

        if (reversible == 2 || reversible == 4)
        {
            float replaceDetail = config->DlssNrReplaceDetailStrength.value_or_default();
            if (ImGui::SliderFloat("恢复锐度", &replaceDetail, 0.0f, 2.0f, "%.2f"))
                config->DlssNrReplaceDetailStrength = replaceDetail;

            ImGui::SameLine();
            if (ImGui::SmallButton("重置##replacedetail"))
                config->DlssNrReplaceDetailStrength = 0.5f;

            HelpMarker("使用原始帧的亮度锐化细边缘与纹理。当「最终图像合成」使用替换模式、且 NR 以低于 100% 的分辨率运行时很有帮助，否则画面可能显得发软。\n分辨率为 100% 或更高时无效，设为 0 时也无效。");
        }

        ImGui::SeparatorText("效果强度");

        float transfer = config->DlssNrTransferStrength.value_or_default();
        if (ImGui::SliderFloat("细节强度", &transfer, 0.0f, 2.0f, "%.2f"))
            config->DlssNrTransferStrength = transfer;

        ImGui::SameLine();
        if (ImGui::SmallButton("重置##detail"))
            config->DlssNrTransferStrength = 1.0f;

        HelpMarker("NR 总体细节强度: 0 = 无效果，1 = 正常，大于 1 = 夸张。");

        float colour = config->DlssNrColourStrength.value_or_default();
        if (ImGui::SliderFloat("色彩强度", &colour, 0.0f, 4.0f, "%.2f"))
            config->DlssNrColourStrength = colour;

        ImGui::SameLine();
        if (ImGui::SmallButton("重置##colour"))
            config->DlssNrColourStrength = 1.0f;

        HelpMarker("NR 色彩强度: 0 = 保留游戏色彩，1 = 模型色彩，大于 1 = 更强饱和度。");

        ImGui::SeparatorText("模型遍数");
        ImGui::TextWrapped("松开滑块时设置才会生效。");
        static const char* styles[] = { "标准", "自然", "电影感" };
        static const char* inheritedStyles[] = { "自动（继承第 1 遍）", "标准", "自然", "电影感" };

        if (ImGui::TreeNodeEx("第 1 遍", ImGuiTreeNodeFlags_DefaultOpen))
        {
            int style = (int) std::min(config->DlssNrStyle.value_or_default(), 2u);
            if (ImGui::Combo("风格", &style, styles, IM_ARRAYSIZE(styles)))
                config->DlssNrStyle = (uint32_t) style;
            HelpMarker("选择外观配置档: 标准、自然或电影感。强度控制其作用力度。");
            DeferredSlider("强度", &config->DlssNrIntensity, 0.0f, 2.0f, 1.0f);
            DeferredSlider("局部结构", &config->DlssNrLocalStructure, 0.0f, 2.0f, 1.0f);
            DeferredSlider("局部色调", &config->DlssNrLocalTone, 0.0f, 2.0f, 1.0f);
            DeferredSlider("皮肤结构", &config->DlssNrSkinStructure, -1.0f, 2.0f, -1.0f);
            bool mask = config->DlssNrAutoMask.value_or_default();
            if (ImGui::Checkbox("自动皮肤遮罩", &mask))
                config->DlssNrAutoMask = mask;
            HelpMarker("使用模型学习到的皮肤选择来应用「皮肤结构」，无需手工遮罩。\n准确度不一。这与下方基于色彩的遮罩是分开的。");
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("第 2 遍", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextWrapped("默认值：继承第 1 遍；局部色调 = 0。重置可恢复这些默认值。");
            InheritedProfileCombo("风格", &config->DlssNrPass2Style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
            DeferredSlider("强度", &config->DlssNrPass2Intensity, 0.0f, 2.0f, config->DlssNrIntensity.value_or_default(), "%.2f", true);
            DeferredSlider("局部结构", &config->DlssNrPass2LocalStructure, 0.0f, 2.0f, config->DlssNrLocalStructure.value_or_default(), "%.2f", true);
            DeferredSlider("局部色调", &config->DlssNrPass2LocalTone, 0.0f, 2.0f, 0.0f, "%.2f", true);
            DeferredSlider("皮肤结构", &config->DlssNrPass2SkinStructure, -1.0f, 2.0f, config->DlssNrSkinStructure.value_or_default(), "%.2f", true);
            bool mask = config->DlssNrPass2AutoMask.has_value() ? config->DlssNrPass2AutoMask.value() : config->DlssNrAutoMask.value_or_default();
            if (ImGui::Checkbox("自动皮肤遮罩", &mask))
                config->DlssNrPass2AutoMask = mask;
            ImGui::SameLine();
            if (ImGui::SmallButton("重置##mask"))
                config->DlssNrPass2AutoMask = std::optional<bool> {};
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("第 3 遍", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextWrapped("默认值：继承第 1 遍；局部色调 = 0。重置可恢复这些默认值。");
            InheritedProfileCombo("风格", &config->DlssNrPass3Style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
            DeferredSlider("强度", &config->DlssNrPass3Intensity, 0.0f, 2.0f, config->DlssNrIntensity.value_or_default(), "%.2f", true);
            DeferredSlider("局部结构", &config->DlssNrPass3LocalStructure, 0.0f, 2.0f, config->DlssNrLocalStructure.value_or_default(), "%.2f", true);
            DeferredSlider("局部色调", &config->DlssNrPass3LocalTone, 0.0f, 2.0f, 0.0f, "%.2f", true);
            DeferredSlider("皮肤结构", &config->DlssNrPass3SkinStructure, -1.0f, 2.0f, config->DlssNrSkinStructure.value_or_default(), "%.2f", true);
            bool mask = config->DlssNrPass3AutoMask.has_value() ? config->DlssNrPass3AutoMask.value() : config->DlssNrAutoMask.value_or_default();
            if (ImGui::Checkbox("自动皮肤遮罩", &mask))
                config->DlssNrPass3AutoMask = mask;
            ImGui::SameLine();
            if (ImGui::SmallButton("重置##mask"))
                config->DlssNrPass3AutoMask = std::optional<bool> {};
            ImGui::TreePop();
        }

        const unsigned int visiblePasses = std::clamp(config->DlssNrPasses.value_or_default(), 1u, passLimit);
        for (unsigned int pass = 3; pass < visiblePasses; ++pass)
        {
            auto& settings = config->DlssNrExtraPasses[pass - 3];
            if (!ImGui::TreeNode(std::format("第 {} 遍", pass + 1).c_str()))
                continue;
            ImGui::TextWrapped("默认值：继承第 1 遍；局部色调 = 0。");
            InheritedProfileCombo("风格", &settings.style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
            DeferredSlider("强度", &settings.intensity, 0.0f, 2.0f, config->DlssNrIntensity.value_or_default(), "%.2f", true);
            DeferredSlider("局部结构", &settings.structure, 0.0f, 2.0f, config->DlssNrLocalStructure.value_or_default(), "%.2f", true);
            DeferredSlider("局部色调", &settings.tone, 0.0f, 2.0f, 0.0f, "%.2f", true);
            DeferredSlider("皮肤结构", &settings.skin, -1.0f, 2.0f, config->DlssNrSkinStructure.value_or_default(), "%.2f", true);
            bool mask = settings.autoMask.value_or(config->DlssNrAutoMask.value_or_default());
            if (ImGui::Checkbox("自动皮肤遮罩", &mask))
                settings.autoMask = mask;
            ImGui::SameLine();
            if (ImGui::SmallButton("重置##mask"))
                settings.autoMask = std::optional<bool> {};
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("高级预设提示（效果未经验证）"))
        {
            ImGui::TextWrapped("实验性模型提示；视觉效果未经验证。请使用「风格」选择配置档。");
            static const char* presets[] = { "默认", "预设 1", "预设 2", "预设 3" };
            static const char* inheritedPresets[] = { "自动（继承第 1 遍）", "默认", "预设 1", "预设 2", "预设 3" };
            int preset = (int) std::min(config->DlssNrPreset.value_or_default(), 3u);
            if (ImGui::Combo("第 1 遍预设提示", &preset, presets, IM_ARRAYSIZE(presets)))
                config->DlssNrPreset = (uint32_t) preset;
            InheritedProfileCombo("第 2 遍预设提示", &config->DlssNrPass2Preset, inheritedPresets, IM_ARRAYSIZE(inheritedPresets));
            InheritedProfileCombo("第 3 遍预设提示", &config->DlssNrPass3Preset, inheritedPresets, IM_ARRAYSIZE(inheritedPresets));
            ImGui::TreePop();
        }
        ImGui::TextWrapped("遍设置适用于 DX12 和原生 Vulkan 上的 SR 与 RR。驱动代理后端仅支持一遍。");

        ImGui::SeparatorText("色彩");

        if (ImGui::TreeNode("皮肤与环境（最终编辑）"))
        {
            ImGui::TextWrapped("按色彩选择皮肤，并分别调整皮肤与景物的最终 NR 效果。选择可能不准确；请查看预览。");
            bool filter = config->DlssNrSkinProtection.value_or_default();
            if (ImGui::Checkbox("皮肤 / 环境分开控制", &filter))
                config->DlssNrSkinProtection = filter;
            ImGui::BeginDisabled(!filter);
            bool tone = config->DlssNrSkinToneEnabled.value_or_default();
            if (ImGui::Checkbox("允许改变肤色 / 色彩", &tone))
                config->DlssNrSkinToneEnabled = tone;
            HelpMarker("允许 NR 在所选皮肤区域内改变色彩。关闭则保留原有色彩；细节仍可变化。");
            const auto slider = [](const char* label, auto& option) {
                float v = option.value_or_default();
                if (ImGui::SliderFloat(label, &v, 0.0f, 1.0f, "%.2f"))
                    option = v;
                ImGui::SameLine();
                const std::string resetId = std::string("重置##") + label;
                if (ImGui::SmallButton(resetId.c_str()))
                    option = 1.0f;
                HelpMarker("该区域内的 NR 强度: 0 = 无变化，1 = 完全生效。");
            };
            slider("Skin detail / lighting", config->DlssNrSkinDetail);
            ImGui::BeginDisabled(!tone);
            slider("皮肤色彩", config->DlssNrSkinColour);
            ImGui::EndDisabled();
            slider("Environment detail / lighting", config->DlssNrEnvironmentDetail);
            slider("环境色彩", config->DlssNrEnvironmentColour);
            bool preview = config->DlssNrShowSkinMask.value_or_default();
            if (ImGui::Checkbox("预览基于色彩的遮罩", &preview))
                config->DlssNrShowSkinMask = preview;
            ImGui::EndDisabled();
            ImGui::TreePop();
        }

        ImGui::SeparatorText("对比");

        // Freeze the frame the model works on, so a setting change re-renders it in place -- the only
        // clean way to A/B our own settings (a moving scene confounds every other comparison). See
        // design/frame-hold.md.
        bool held = config->DlssNrHoldFrame.value_or_default();
        if (ImGui::Checkbox("冻结帧", &held))
            config->DlssNrHoldFrame = held;

        HelpMarker("冻结 NR 的输入以对比其设置。游戏的 HUD 和后续特效可能仍在更新。\n不会重新运行 SR/RR，也不显示其设置的变化。关闭即可恢复。");

        bool frameStats = config->DlssNrFrameStats.value_or_default();
        if (ImGui::Checkbox("记录帧亮度统计", &frameStats))
            config->DlssNrFrameStats = frameStats;

        HelpMarker("诊断。约每 2 秒向 OptiScaler.log 写入一行，描述交给 NR 的帧：格式、亮度百分位、游戏的曝光值以及当前使用的白点。");

        bool kernelProfile = config->DlssNrKernelProfile.value_or_default();
        if (ImGui::Checkbox("记录 NR 内核分析", &kernelProfile))
            config->DlssNrKernelProfile = kernelProfile;

        HelpMarker("诊断。约每 4 秒向 OptiScaler.log 写入一行，记录一次 NR 求值所启动的 NVIDIA 内核（以 fp8 命名或纯 fp16）及其 GPU 耗时去向（按内核分组）。为近似值：串联内核会重叠。");

        static const char* compareNames[] = { "关闭", "并排", "划擦" };
        int compare = (int) config->DlssNrCompare.value_or_default();
        if (ImGui::Combo("对比", &compare, compareNames, IM_ARRAYSIZE(compareNames)))
            config->DlssNrCompare = (uint32_t) compare;

        HelpMarker("对比原始画面与 NR 结果。并排模式同时容纳两张图像；划像模式则分割一张全尺寸图像。");

        if (compare != 0)
        {
            bool swap = config->DlssNrCompareSwap.value_or_default();
            if (ImGui::Checkbox("交换左右", &swap))
                config->DlssNrCompareSwap = swap;
            HelpMarker("交换原始与 NR 两侧。");

            bool tags = config->DlssNrCompareTags.value_or_default();
            if (ImGui::Checkbox("标注两侧", &tags))
                config->DlssNrCompareTags = tags;

            HelpMarker("显示标识原始与 NR 两侧的标签。");

            if (tags)
            {
                float tagScale = config->DlssNrTagScale.value_or_default();
                if (ImGui::SliderFloat("标签大小", &tagScale, 0.5f, 5.0f, "%.1fx"))
                    config->DlssNrTagScale = std::clamp(tagScale, 0.5f, 5.0f);
            }

        }

        if (compare == 1)
        {
            float zoom = config->DlssNrCompareZoom.value_or_default();
            if (ImGui::SliderFloat("缩放", &zoom, 1.0f, 2.0f, "%.2f"))
                config->DlssNrCompareZoom = std::clamp(zoom, 1.0f, 2.0f);

            HelpMarker("并排缩放: 1 = 容纳整幅图像，2 = 裁切两侧以填满每一半。");
        }

        if (compare == 2)
        {
            float split = config->DlssNrCompareSplit.value_or_default();
            if (ImGui::SliderFloat("分屏", &split, 0.0f, 1.0f, "%.2f"))
                config->DlssNrCompareSplit = std::clamp(split, 0.0f, 1.0f);

            HelpMarker("对比分界线的位置。「交换左右」会反转每一侧显示的图像。");
        }

        static const char* debugNames[] = { "关闭", "代理（模型所见）", "模型输出（原始）",
                                            "差异（放大）" };
        int debugView = (int) config->DlssNrDebugView.value_or_default();
        if (ImGui::Combo("Debug view", &debugView, debugNames, IM_ARRAYSIZE(debugNames)))
            config->DlssNrDebugView = (uint32_t) debugView;

        HelpMarker("显示模型输入、原始输出，或放大 20 倍的差异图。差异图中的灰色表示无变化。\n以游戏自身的亮度显示，因此「模型输入亮度」会让视图按其改变模型输入的幅度相应变亮或变暗。");

        ImGui::PopItemWidth();
    }
}

} // namespace DlssNr

