/*
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "renderjournal.h"

#include <algorithm>
#include <cmath>
#include <iterator>

using namespace std::chrono_literals;

namespace KWin
{

static constexpr auto s_windowDuration = 1s;
static constexpr size_t s_maxSamples = 512;
static constexpr size_t s_renderTimePercentile = 95;
static constexpr size_t s_wakeLatencyPercentile = 95;
// Target rate for late flips caused by insufficient wake reservation.
static constexpr double s_wakeMissBudget = 0.005;
static constexpr auto s_missRateTimeConstant = 2s;
static constexpr auto s_adjustInterval = 250ms;
static constexpr double s_minimumFrameWeight = 50;
// Feedback may reserve more than the measured estimate alone.
static constexpr std::chrono::nanoseconds s_estimateReservationCap = 1ms;

static std::chrono::nanoseconds minimumReservation(std::chrono::nanoseconds vblankInterval)
{
    return std::min(std::chrono::nanoseconds(1ms), vblankInterval / 32);
}

RenderJournal::RenderJournal()
{
    for (auto &history : m_histories) {
        history.renderTime.scratch.reserve(s_maxSamples);
        history.wakeLatency.scratch.reserve(s_maxSamples);
    }
}

size_t RenderJournal::modeIndex(Mode mode)
{
    return static_cast<size_t>(mode);
}

void RenderJournal::updateEstimate(History &history, size_t percentile)
{
    if (history.samples.empty()) {
        return;
    }

    history.scratch.clear();
    std::ranges::transform(history.samples, std::back_inserter(history.scratch), [](const Sample &sample) {
        return sample.duration;
    });
    const size_t rank = (history.scratch.size() * percentile + 99) / 100 - 1;
    std::nth_element(history.scratch.begin(), history.scratch.begin() + rank, history.scratch.end());
    history.estimate = history.scratch[rank];

    if (history.samples.size() >= 3) {
        const auto first = history.samples.end() - 3;
        const auto attack = std::min_element(first, history.samples.end(), [](const Sample &left, const Sample &right) {
            return left.duration < right.duration;
        })->duration;
        history.estimate = std::max(history.estimate, attack);
    }
}

bool RenderJournal::expireSamples(History &history, std::chrono::nanoseconds timestamp)
{
    bool changed = false;
    while (!history.samples.empty() && timestamp - history.samples.front().timestamp >= s_windowDuration) {
        history.samples.pop_front();
        changed = true;
    }
    return changed;
}

void RenderJournal::addSample(History &history, std::chrono::nanoseconds duration, std::chrono::nanoseconds timestamp)
{
    history.samples.push_back(Sample{
        .duration = std::max(duration, 0ns),
        .timestamp = timestamp,
    });
    while (history.samples.size() > s_maxSamples) {
        history.samples.pop_front();
    }
}

void RenderJournal::add(std::chrono::nanoseconds renderTime, std::chrono::nanoseconds wakeLatency,
                        std::chrono::nanoseconds presentationTimestamp, Mode mode)
{
    for (auto &history : m_histories) {
        if (expireSamples(history.renderTime, presentationTimestamp)) {
            updateEstimate(history.renderTime, s_renderTimePercentile);
        }
        if (expireSamples(history.wakeLatency, presentationTimestamp)) {
            updateEstimate(history.wakeLatency, s_wakeLatencyPercentile);
        }
    }

    auto &history = m_histories[modeIndex(mode)];
    addSample(history.renderTime, renderTime, presentationTimestamp);
    addSample(history.wakeLatency, wakeLatency, presentationTimestamp);
    updateEstimate(history.renderTime, s_renderTimePercentile);
    updateEstimate(history.wakeLatency, s_wakeLatencyPercentile);
}

void RenderJournal::notifyFrameOutcome(Mode mode, std::chrono::nanoseconds vblankInterval,
                                       std::chrono::nanoseconds timestamp, bool lateFlip,
                                       std::chrono::nanoseconds wakeLatency,
                                       std::chrono::nanoseconds wakeReservation)
{
    auto &controller = m_histories[modeIndex(mode)].wakeController;

    if (controller.lastUpdate) {
        const double decay = std::exp(-double((timestamp - *controller.lastUpdate).count())
                                      / double(std::chrono::nanoseconds(s_missRateTimeConstant).count()));
        controller.missWeight *= decay;
        controller.frameWeight *= decay;
    }
    controller.lastUpdate = timestamp;
    controller.frameWeight += 1;
    if (lateFlip && wakeLatency > wakeReservation && wakeLatency <= vblankInterval / 2) {
        controller.missWeight += 1;
        controller.missSinceAdjust = true;
    }
    controller.reserve = std::min(controller.reserve, vblankInterval / 2);

    if (controller.frameWeight < s_minimumFrameWeight
        || timestamp - controller.lastAdjust < s_adjustInterval) {
        return;
    }
    controller.lastAdjust = timestamp;

    const double missRate = controller.missWeight / controller.frameWeight;
    if (missRate > s_wakeMissBudget && controller.missSinceAdjust) {
        // Do not raise the reserve from decaying miss history alone.
        controller.reserve += vblankInterval / 16;
    } else if (missRate < s_wakeMissBudget / 2) {
        controller.reserve -= vblankInterval / 64;
    }
    controller.missSinceAdjust = false;
    controller.reserve = std::clamp(controller.reserve, std::chrono::nanoseconds::zero(), vblankInterval / 2);
}

RenderJournal::Prediction RenderJournal::result(Mode mode, std::chrono::nanoseconds vblankInterval) const
{
    const auto minimum = minimumReservation(vblankInterval);
    const auto &history = m_histories[modeIndex(mode)];
    const auto measuredWake = std::min(history.wakeLatency.estimate, s_estimateReservationCap);
    return Prediction{
        .renderTime = std::clamp(history.renderTime.estimate, minimum, 2 * vblankInterval),
        // Feedback can raise the capped estimate to half a refresh interval.
        .wakeLatency = std::clamp(std::max(measuredWake, history.wakeController.reserve),
                                  minimum, vblankInterval / 2),
    };
}

} // namespace KWin
