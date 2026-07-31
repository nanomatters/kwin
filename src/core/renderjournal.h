/*
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once
#include "kwin_export.h"

#include <array>
#include <chrono>
#include <deque>
#include <optional>
#include <vector>

namespace KWin
{

/**
 * The RenderJournal class measures how long it takes to wake up and render frames and
 * estimates how much time should be reserved for the next frame.
 */
class KWIN_EXPORT RenderJournal
{
public:
    enum class Mode {
        Composited,
        DirectScanout,
    };

    struct Prediction
    {
        std::chrono::nanoseconds renderTime;
        std::chrono::nanoseconds wakeLatency;
    };

    explicit RenderJournal();

    void add(std::chrono::nanoseconds renderTime, std::chrono::nanoseconds wakeLatency,
             std::chrono::nanoseconds presentationTimestamp, Mode mode);

    /**
     * Updates the wake reservation from a completed fixed-refresh VSync frame.
     * wakeReservation must be the value used to schedule that frame.
     */
    void notifyFrameOutcome(Mode mode, std::chrono::nanoseconds vblankInterval,
                            std::chrono::nanoseconds timestamp, bool lateFlip,
                            std::chrono::nanoseconds wakeLatency,
                            std::chrono::nanoseconds wakeReservation);

    Prediction result(Mode mode, std::chrono::nanoseconds vblankInterval) const;

private:
    struct Sample
    {
        std::chrono::nanoseconds duration;
        std::chrono::nanoseconds timestamp;
    };

    struct History
    {
        std::deque<Sample> samples;
        std::vector<std::chrono::nanoseconds> scratch;
        std::chrono::nanoseconds estimate{0};
    };

    struct WakeController
    {
        double missWeight = 0;
        double frameWeight = 0;
        bool missSinceAdjust = false;
        std::optional<std::chrono::nanoseconds> lastUpdate;
        std::chrono::nanoseconds lastAdjust{0};
        std::chrono::nanoseconds reserve{0};
    };

    struct ModeHistory
    {
        History renderTime;
        History wakeLatency;
        WakeController wakeController;
    };

    static size_t modeIndex(Mode mode);
    static void updateEstimate(History &history, size_t percentile);
    static bool expireSamples(History &history, std::chrono::nanoseconds timestamp);
    static void addSample(History &history, std::chrono::nanoseconds duration,
                          std::chrono::nanoseconds timestamp);

    std::array<ModeHistory, 2> m_histories;
};

} // namespace KWin
