#pragma once

// Automatic exposure following the game's own exposure, on frames the game has not exposed yet.
//
// On an unexposed frame the game hands DLSS the exposure it is about to apply in its tone-mapping pass. That value is
// the game's own adaptation, frame by frame: it cannot be fooled by letterboxing, menus, fades or a flash, and it moves
// exactly when the picture on screen moves. Automatic's meter is not the game's, though -- it maps the scene's
// log-average to middle grey (0.18) and RDR2 maps it to about 1.7, a steady 3.2 EV apart in gameplay (measured
// 2026-09-25, -3.14..-3.27 over 70 s). So the game's exposure is not used as it is: the offset between the two is learned
// here, once per session, and the base white point Automatic uses becomes the game's times that offset. The brightness
// slider keeps its meaning (the same EV is the same picture), and only the frame-to-frame movement is the game's.
//
// The offset is the median of log2(Automatic base white point / the game's) over kWindow readings, taken only while the
// frame reads unexposed (DlssNr_AutoTrimDefault.h), and locked from then on. The meter ignores black tiles, so a
// letterboxed cutscene reads the same offset as gameplay; a few loading-screen or menu readings are outvoted by the
// median, and a reading more than kMaxEv from zero is not taken at all.
//
// Header-only and free of D3D types so it can be exercised on the host (tests/nr_follow_game_smoke.cpp).

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace DlssNrFollowGame
{
constexpr unsigned kWindow = 120; // readings before the offset locks (about 2 s)
constexpr float kMaxEv = 10.0f;   // |offset| beyond this is not a calibration, it is a broken reading

class Calibration
{
  public:
    // One pair of base white points from the same frame. True when this reading locked the offset.
    bool Feed(float autoBaseWhitePoint, float gameBaseWhitePoint)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (locked_ || !(autoBaseWhitePoint > 0.0f) || !(gameBaseWhitePoint > 0.0f) ||
            !std::isfinite(autoBaseWhitePoint) || !std::isfinite(gameBaseWhitePoint))
            return false;

        const float ev = std::log2(autoBaseWhitePoint / gameBaseWhitePoint);

        if (!std::isfinite(ev) || std::fabs(ev) > kMaxEv)
            return false;

        window_[filled_++] = ev;

        if (filled_ < kWindow)
            return false;

        std::array<float, kWindow> sorted = window_;
        std::nth_element(sorted.begin(), sorted.begin() + kWindow / 2, sorted.end());
        offsetEv_ = sorted[kWindow / 2];
        locked_ = true;
        return true;
    }

    bool Locked() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return locked_;
    }

    // log2(Automatic / game), valid once Locked().
    float OffsetEv() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return offsetEv_;
    }

    // What the game's base white point is multiplied by. 1 until locked.
    float Scale() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return locked_ ? std::exp2(offsetEv_) : 1.0f;
    }

    unsigned Readings() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return filled_;
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        window_ = {};
        filled_ = 0;
        locked_ = false;
        offsetEv_ = 0.0f;
    }

  private:
    mutable std::mutex mutex_;
    std::array<float, kWindow> window_ {};
    unsigned filled_ = 0;
    bool locked_ = false;
    float offsetEv_ = 0.0f;
};

inline Calibration& Instance()
{
    static Calibration calibration;
    return calibration;
}
} // namespace DlssNrFollowGame
