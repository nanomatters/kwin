/*
    SPDX-FileCopyrightText: 2026 Erhan Bilgili <erhan.bilgili@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "core/renderjournal.h"

#include <QTest>

/*
 * Fixed-refresh frame pacer design
 * =================================
 *
 * The pacer chooses how early KWin should begin a frame so that rendering,
 * waking the compositor and queuing the atomic commit all finish before the
 * target vblank. Its prediction is deliberately split by render mode:
 *
 *     +---------------------+        +---------------------+
 *     | Direct scanout      |        | Composited          |
 *     | render / wake       |        | render / wake       |
 *     | history             |        | history             |
 *     +----------+----------+        +----------+----------+
 *                |                              |
 *                +--------------+---------------+
 *                               |
 *                         current render mode
 *                               |
 *                               v
 *     target vblank - (render estimate + safety margin + wake reservation)
 *                               |
 *                               v
 *                         compositor timer
 *
 * Keeping the histories separate is important: direct scanout has almost no
 * compositor GPU work, while composition necessarily includes the complete
 * scene. A brief transition must not make the fast scanout path reserve a
 * composited-frame budget forever, nor make the first composited frame inherit
 * an unrealistically small scanout prediction. The inactive mode retains its
 * most recent estimate for exactly that reason.
 *
 * Estimation
 * ----------
 * Each mode keeps up to one second (and at most 512) timestamped samples for
 * render duration and timer wake latency. The normal estimate is p95. This
 * gives good protection against ordinary jitter without letting one isolated
 * hitch poison every subsequent frame. A three-sample attack path raises the
 * estimate to the smallest of the last three samples, so a sustained increase
 * is recognised immediately instead of waiting for p95 to accumulate.
 *
 *                         one-second history
 *     samples:  .12 .11 .13 .12 .10 .12 .11 .78 .12 .11
 *                                               ^ one spike: p95 resists it
 *
 *     last 3:                                      .78 .12 .11
 *                                                  ^ smallest: .11
 *
 *     last 3 after a real change:                  .62 .66 .64
 *                                                  ^ smallest: .62
 *
 * The final render estimate is clamped to [vblank / 32, 2 * vblank]. The
 * lower bound scales with refresh rate, unlike a fixed millisecond floor:
 *
 *                 240 Hz: 4.17 ms / 32 = 0.130 ms
 *                 144 Hz: 6.94 ms / 32 = 0.217 ms
 *
 * Wake latency uses the same history but its percentile contribution is capped
 * at 1 ms. Larger, persistent wake needs are handled by a separate feedback
 * controller below; this prevents a rare multi-millisecond scheduler stall
 * from becoming permanent latency for every frame.
 *
 * Wake-reservation feedback
 * -------------------------
 * For every completed fixed-refresh VSync frame, RenderLoop records the wake
 * reservation that was actually used to schedule it. The outcome is then
 * classified as follows:
 *
 *                              late flip?
 *                                  |
 *                       +----------+----------+
 *                       |                     |
 *                      no                    yes
 *                       |                     |
 *                    no miss       wake > reservation AND
 *                                   wake <= vblank / 2 ?
 *                                              |
 *                                  +-----------+-----------+
 *                                  |                       |
 *                                 yes                     no
 *                                  |                       |
 *                        attributable wake miss     do not learn
 *
 * A late flip without a wake overrun has another cause (for example rendering
 * or client timing). A wake later than half a refresh interval cannot be made
 * safe by any allowed reservation, so it is treated as an unschedulable stall
 * rather than a reason to delay all later frames.
 *
 * The controller maintains exponentially decayed miss and frame weights with a
 * two-second time constant. Once it has at least 50 effective frames, it
 * adjusts at most every 250 ms:
 *
 *     attributable miss rate > 0.5%  -> reserve += vblank / 16
 *     attributable miss rate < 0.25% -> reserve -= vblank / 64
 *     otherwise                       -> keep the current reserve
 *
 * The reserve is bounded to [0, vblank / 2], and the wake prediction used by
 * the scheduler is:
 *
 *     clamp(max(min(p95 wake, 1 ms), feedback reserve),
 *           vblank / 32, vblank / 2)
 *
 * Raising the reserve quickly limits repeated wake-caused misses; decaying it
 * more slowly returns latency to the minimum after the condition disappears.
 * The feedback state is also isolated per render mode and is clamped
 * immediately when the refresh rate changes.
 *
 * Glossary
 * --------
 * vblank
 *     The display's periodic vertical-blank interval. At fixed refresh it is
 *     the frame budget: 4.17 ms at 240 Hz and 6.94 ms at 144 Hz.
 *
 * target vblank / target pageflip
 *     The display refresh at which KWin intends the newly prepared buffer to
 *     become visible.
 *
 * pageflip
 *     The DRM commit that replaces the displayed framebuffer. A late flip is
 *     a pageflip reported more than half a refresh interval after its target.
 *
 * wake latency
 *     Time from the timer's scheduled render timestamp to the actual start of
 *     rendering. It measures timer and event-loop lateness, not GPU render
 *     duration.
 *
 * reservation (or lead time)
 *     Time deliberately held before the target vblank for predicted rendering,
 *     safety margin and wake latency. A smaller safe reservation means the
 *     client can submit its newest frame later, reducing latency.
 *
 * p95
 *     The 95th percentile: 95% of retained samples are at or below this value.
 *     It protects against ordinary tail jitter without budgeting for the single
 *     worst sample.
 *
 * direct scanout
 *     The game's buffer is placed directly on a display plane. KWin does not
 *     render the full desktop scene for that frame.
 *
 * composited frame
 *     KWin renders the desktop scene into its own output buffer before that
 *     buffer is presented. This has a separate timing history from scanout.
 *
 * attributable wake miss
 *     A late flip where wake latency exceeded the reservation but was no more
 *     than half a refresh interval. Only these misses train the wake controller.
 *
 * Test map
 * --------
 * The tests below specify these guarantees: mode isolation and retention,
 * percentile resistance to isolated spikes, fast response to sustained work,
 * expiry of old evidence, refresh-relative bounds, bounded feedback growth,
 * decay after recovery, and rejection of late flips that the wake reservation
 * could not have prevented.
 */

using namespace std::chrono_literals;

namespace KWin
{

static void addFrameOutcome(RenderJournal &journal, RenderJournal::Mode mode,
                            std::chrono::nanoseconds vblankInterval,
                            std::chrono::nanoseconds timestamp,
                            std::chrono::nanoseconds renderTime,
                            std::chrono::nanoseconds wakeLatency, bool lateFlip)
{
    const auto wakeReservation = journal.result(mode, vblankInterval).wakeLatency;
    journal.add(renderTime, wakeLatency, timestamp, mode);
    journal.notifyFrameOutcome(mode, vblankInterval, timestamp, lateFlip,
                               wakeLatency, wakeReservation);
}

class RenderJournalTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void isolatesPresentationModes();
    void ignoresSingleSpikes();
    void reactsToSustainedChanges();
    void expiresOldSamples();
    void retainsInactiveModeEstimate();
    void clampsPredictions();
    void ignoresSparseWakeLatencySpikes();
    void settlesReservationAtSustainedWakeCost();
    void raisesReservationForTailMissesBelowEstimate();
    void decaysReservationWhenMissesStop();
    void lateFlipsWithoutWakeOverrunDoNotRaiseReservation();
    void catastrophicWakeStallsDoNotRaiseReservation();
    void clampsReservationAcrossRefreshChanges();
};

void RenderJournalTest::isolatesPresentationModes()
{
    RenderJournal journal;
    for (int i = 0; i < 20; ++i) {
        const auto timestamp = i * 10ms;
        journal.add(200us, 150us, timestamp, RenderJournal::Mode::DirectScanout);
        journal.add(600us, 800us, timestamp, RenderJournal::Mode::Composited);
    }

    const auto directScanout = journal.result(RenderJournal::Mode::DirectScanout, 1ms);
    QCOMPARE(directScanout.renderTime, 200us);
    QCOMPARE(directScanout.wakeLatency, 150us);

    const auto composited = journal.result(RenderJournal::Mode::Composited, 1ms);
    QCOMPARE(composited.renderTime, 600us);
    // Wake reservation is limited to half a refresh interval.
    QCOMPARE(composited.wakeLatency, 500us);
}

void RenderJournalTest::ignoresSingleSpikes()
{
    RenderJournal journal;
    for (int i = 0; i < 200; ++i) {
        journal.add(3ms, 600us, i * 1ms, RenderJournal::Mode::Composited);
    }
    journal.add(12ms, 8ms, 200ms, RenderJournal::Mode::Composited);

    const auto result = journal.result(RenderJournal::Mode::Composited, 4ms);
    QCOMPARE(result.renderTime, 3ms);
    QCOMPARE(result.wakeLatency, 600us);
}

void RenderJournalTest::reactsToSustainedChanges()
{
    RenderJournal journal;
    for (int i = 0; i < 100; ++i) {
        journal.add(3ms, 200us, i * 1ms, RenderJournal::Mode::Composited);
    }
    journal.add(7ms, 700us, 100ms, RenderJournal::Mode::Composited);
    journal.add(6ms, 600us, 101ms, RenderJournal::Mode::Composited);
    journal.add(8ms, 800us, 102ms, RenderJournal::Mode::Composited);

    const auto result = journal.result(RenderJournal::Mode::Composited, 4ms);
    QCOMPARE(result.renderTime, 6ms);
    QCOMPARE(result.wakeLatency, 600us);
}

void RenderJournalTest::expiresOldSamples()
{
    RenderJournal journal;
    journal.add(8ms, 4ms, 0ms, RenderJournal::Mode::Composited);
    journal.add(2ms, 800us, 1100ms, RenderJournal::Mode::Composited);

    const auto result = journal.result(RenderJournal::Mode::Composited, 4ms);
    QCOMPARE(result.renderTime, 2ms);
    QCOMPARE(result.wakeLatency, 800us);
}

void RenderJournalTest::retainsInactiveModeEstimate()
{
    RenderJournal journal;
    journal.add(3ms, 800us, 0ms, RenderJournal::Mode::Composited);

    journal.add(200us, 100us, 1100ms, RenderJournal::Mode::DirectScanout);

    const auto composited = journal.result(RenderJournal::Mode::Composited, 4ms);
    QCOMPARE(composited.renderTime, 3ms);
    QCOMPARE(composited.wakeLatency, 800us);

    const auto directScanout = journal.result(RenderJournal::Mode::DirectScanout, 4ms);
    QCOMPARE(directScanout.renderTime, 200us);
    QCOMPARE(directScanout.wakeLatency, 125us);
}

void RenderJournalTest::clampsPredictions()
{
    RenderJournal journal;

    auto result = journal.result(RenderJournal::Mode::Composited, 8ms);
    QCOMPARE(result.renderTime, 250us);
    QCOMPARE(result.wakeLatency, 250us);

    journal.add(30ms, 24ms, 0ms, RenderJournal::Mode::Composited);
    result = journal.result(RenderJournal::Mode::Composited, 8ms);
    QCOMPARE(result.renderTime, 16ms);
    QCOMPARE(result.wakeLatency, 1ms);
}

void RenderJournalTest::ignoresSparseWakeLatencySpikes()
{
    RenderJournal journal;
    for (int i = 0; i < 200; ++i) {
        journal.add(3ms, 200us, i * 1ms, RenderJournal::Mode::Composited);
    }
    // Three non-consecutive tail samples remain outside p95.
    journal.add(12ms, 800us, 200ms, RenderJournal::Mode::Composited);
    journal.add(3ms, 200us, 201ms, RenderJournal::Mode::Composited);
    journal.add(12ms, 800us, 202ms, RenderJournal::Mode::Composited);
    journal.add(3ms, 200us, 203ms, RenderJournal::Mode::Composited);
    journal.add(12ms, 800us, 204ms, RenderJournal::Mode::Composited);
    journal.add(3ms, 200us, 205ms, RenderJournal::Mode::Composited);

    const auto result = journal.result(RenderJournal::Mode::Composited, 4ms);
    QCOMPARE(result.renderTime, 3ms);
    QCOMPARE(result.wakeLatency, 200us);
}

void RenderJournalTest::settlesReservationAtSustainedWakeCost()
{
    RenderJournal journal;
    for (int i = 0; i < 600; ++i) {
        const auto t = i * 4ms;
        addFrameOutcome(journal, RenderJournal::Mode::Composited, 4ms, t,
                        100us, 1500us, true);
    }

    // The reserve grows past the estimate cap to cover sustained wake cost.
    QCOMPARE(journal.result(RenderJournal::Mode::Composited, 4ms).wakeLatency, 1500us);
}

void RenderJournalTest::raisesReservationForTailMissesBelowEstimate()
{
    RenderJournal journal;
    for (int i = 0; i < 600; ++i) {
        const auto t = i * 4ms;
        // The sparse 1% tail remains outside p95.
        const bool stall = i % 100 == 0;
        addFrameOutcome(journal, RenderJournal::Mode::Composited, 4ms, t,
                        100us, stall ? 1200us : 4us, stall);
    }

    // Miss feedback raises the reservation even when p95 stays low.
    QVERIFY(journal.result(RenderJournal::Mode::Composited, 4ms).wakeLatency > 1ms);
}

void RenderJournalTest::decaysReservationWhenMissesStop()
{
    RenderJournal journal;
    for (int i = 0; i < 300; ++i) {
        const auto t = i * 4ms;
        addFrameOutcome(journal, RenderJournal::Mode::Composited, 4ms, t,
                        100us, 1500us, true);
    }
    for (int i = 300; i < 4300; ++i) {
        const auto t = i * 4ms;
        addFrameOutcome(journal, RenderJournal::Mode::Composited, 4ms, t,
                        100us, 4us, false);
    }

    // The reserve decays to the minimum after misses stop.
    QCOMPARE(journal.result(RenderJournal::Mode::Composited, 4ms).wakeLatency, 125us);
}

void RenderJournalTest::lateFlipsWithoutWakeOverrunDoNotRaiseReservation()
{
    RenderJournal journal;
    for (int i = 0; i < 600; ++i) {
        const auto t = i * 4ms;
        addFrameOutcome(journal, RenderJournal::Mode::Composited, 4ms, t,
                        100us, 4us, true);
    }

    // Late flips without a wake overrun do not affect the reserve.
    QCOMPARE(journal.result(RenderJournal::Mode::Composited, 4ms).wakeLatency, 125us);
}

void RenderJournalTest::catastrophicWakeStallsDoNotRaiseReservation()
{
    RenderJournal journal;
    for (int i = 0; i < 600; ++i) {
        const auto t = i * 4ms;
        addFrameOutcome(journal, RenderJournal::Mode::Composited, 4ms, t,
                        100us, 200ms, true);
    }

    // Stalls beyond the maximum reservation do not affect it.
    QCOMPARE(journal.result(RenderJournal::Mode::Composited, 4ms).wakeLatency, 1ms);
}

void RenderJournalTest::clampsReservationAcrossRefreshChanges()
{
    RenderJournal journal;
    for (int i = 0; i < 600; ++i) {
        const auto t = i * 4ms;
        addFrameOutcome(journal, RenderJournal::Mode::Composited, 4ms, t,
                        100us, 2ms, true);
    }

    QCOMPARE(journal.result(RenderJournal::Mode::Composited, 4ms).wakeLatency, 2ms);
    // A refresh change immediately applies the new upper bound.
    QCOMPARE(journal.result(RenderJournal::Mode::Composited, 2ms).wakeLatency, 1ms);
}

} // namespace KWin

QTEST_GUILESS_MAIN(KWin::RenderJournalTest)
#include "test_render_journal.moc"
