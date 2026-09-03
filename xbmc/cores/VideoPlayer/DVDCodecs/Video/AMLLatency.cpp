/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AMLLatency.h"

#include "cores/VideoPlayer/DVDClock.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "ServiceBroker.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/AMLUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

namespace
{
// The driver publishes pts_video from the vsync interrupt with a plain write,
// but the register writes that put the frame on screen go through RDMA
// (VSYNC_WR_MPEG_REG -> rdma_write_reg) and commit at the following vsync. So
// the timestamp leads the picture by one refresh, and the path from handover to
// photons is one refresh longer than the count of it.
constexpr uint64_t RDMA_COMMIT_VBLANKS = 1;

// A pipeline outside this is a dropped frame or a mode change caught half way.
constexpr uint64_t MAX_VBLANKS = 4;

// A lower answer than the one standing has to be seen this many times running.
constexpr unsigned int AGREE_BEFORE_PUBLISHING = 2;

//! Readings that must agree before the answer is settled for the mode. Two was
//! enough to reject a single stray count, but not to choose between two counts
//! that both keep turning up - and which of those wins decides the schedule by a
//! whole refresh, differently on each run.
constexpr unsigned int AGREE_BEFORE_SETTLING = 8;

// How often to report where the picture actually landed.
constexpr double TIME_BETWEEN_REPORTS = 60.0 * DVD_TIME_BASE;

// Opening a file drops frames on purpose to catch up with the clock, so the
// check stands down until the picture has run clean for about this long. Given
// in seconds and turned into refreshes, so it means the same at 24Hz and at 60.
constexpr double SETTLE_SECONDS = 2.0;

// Armed regardless after this much again, so a picture that is never clean gets
// counted rather than quietly examined for ever.
constexpr unsigned int SETTLE_GIVE_UP = 4;

// A frame held this many refreshes running is a stop of some kind rather than a
// cadence fault. Which kind is decided by whether anything announced it.
constexpr unsigned int HELD_IS_A_STOP = 4;

// A stop within this of an announcement is that announcement - a pause, a seek,
// trick play. Beyond it nobody said the picture would stop, so it froze.
constexpr double ANNOUNCED_WITHIN_US = 1.0e6;

// A reading this close to a vblank may have caught pts_video and the vblank
// counter on opposite sides of it: the two are bumped by different interrupt
// handlers with no ordering between them, and a mismatched pair reads exactly
// like a dropped frame. What has to be cleared is the spread between those two
// handlers, which is set by interrupt latency and does not shrink when the
// refresh period does - so an absolute figure, about ten times the spread to
// expect, capped so that it stays a guard band rather than most of the frame.
constexpr double EDGE_GUARD_US = 500.0;
constexpr double EDGE_GUARD_MAX = 0.10;

// A jump beyond this is a seek or a stream change, not frames falling out.
constexpr double LOST_AT_MOST = 8.0;

// Enough to see what is happening; the report carries the totals either way.
constexpr unsigned int WARN_LIMIT = 10;

// And no faster than this: emitting one blocks the frame loop on a log
// flush, and the diagnostic must not cause the fault it exists to report.
constexpr double WARN_INTERVAL_US = 1.0e6;
} // namespace

void CAMLLatency::Restart()
{
  // Called from the decode thread. Raised as a request because everything it
  // resets belongs to the thread that polls.
  m_rearm.store(true, std::memory_order_relaxed);
}

void CAMLLatency::Forget()
{
  // The published value goes now: the caller may be closing the decoder, and no
  // further poll is guaranteed. The rest is the polling thread's.
  CAMLLatencyStore::GetInstance().Forget();
  m_forget.store(true, std::memory_order_relaxed);
}

void CAMLLatency::Rearm()
{
  m_candidate = 0;
  m_agreed = 0;
  m_settled = false;
  m_armed = false;
  m_haveShown = false;
  m_pollValid = false;
  m_reports = 0;
  m_alignSum = 0.0;
  m_audioReports = 0;
  m_audioSum = 0.0;
  m_leadSum = 0.0;
  m_cadenceSeq = 0;
  m_ratioIsOne = false;
  m_classArmed = false;
  m_classHaveRef = false;
  m_classWaitMove = true;
  // A re-arm follows a seek, a close or a mode change, all of which announce
  // themselves - so the stop that comes next is one of those, not a freeze.
  m_lastDisturbUs = aml_monotonic_us();
  m_cleanRun = m_seenRun = m_heldRun = m_heldPending = 0;
  m_refreshes = m_shown = m_held = m_lost = m_lostOn = 0;
  m_unwatched = m_resyncs = m_stalls = 0;
  m_warned = m_hidden = 0;
  m_lastWarnUs = 0.0;
  m_driftAnchored = false;
  m_driftWalkUs = 0.0;
  m_driftFrames = 0;
}

void CAMLLatency::WarnFrame(const char* what, unsigned int frames, uint32_t ticks)
{
  if (m_warned >= WARN_LIMIT)
  {
    ++m_hidden;
    return;
  }

  // Spaced in time, not merely capped in number: a cap alone lets the whole
  // allowance go out in consecutive passes, and each one is a synchronous log
  // flush on the frame loop.
  const double now = aml_monotonic_us();
  if (m_lastWarnUs != 0.0 && now - m_lastWarnUs < WARN_INTERVAL_US)
  {
    ++m_hidden;
    return;
  }
  m_lastWarnUs = now;
  ++m_warned;

  // Deliberately not carrying the video component: something is wrong with the
  // picture and that should reach the log whatever the debug settings say.
  CLog::Log(LOGWARNING, "CAMLLatency: {} frame{} {} at pts {:.3f}s", frames,
            frames == 1 ? "" : "s", what, static_cast<double>(ticks) / 90000.0);
}

void CAMLLatency::StandDown()
{
  // Keeps the counts and the arming: this means "wait for the picture to come
  // back", not "start the report window again".
  m_classHaveRef = false;
  m_classWaitMove = true;
  m_heldRun = 0;
  m_heldPending = 0;
  m_cleanRun = 0;
}

void CAMLLatency::ClassifyRefresh(uint64_t seq, uint32_t ticks)
{
  // Under pulldown a frame is meant to persist across refreshes, so none of the
  // rules below mean anything there.
  if (!m_ratioIsOne)
  {
    StandDown();
    return;
  }

  if (!m_classHaveRef)
  {
    m_lastClassSeq = seq;
    m_lastClassTicks = ticks;
    m_classHaveRef = true;
    return;
  }

  // A sequence that went backwards is a counter that restarted, not a span.
  // Unsigned subtraction would turn it into billions of unwatched refreshes.
  if (seq < m_lastClassSeq)
  {
    ++m_resyncs;
    StandDown();
    return;
  }

  const uint64_t gap = seq - m_lastClassSeq;
  m_lastClassSeq = seq;
  if (gap == 0)
    return;  // a second poll inside one refresh says nothing new

  // Every refresh the window spanned, judged or not, so the report can say how
  // much of it was actually examined.
  m_refreshes += static_cast<unsigned int>(gap);

  // Folded first, because a negative age is routine rather than wrong: inside
  // the blanking interval the kernel extrapolates to the frame about to start.
  // An age past a whole period is a different thing and is refused rather than
  // folded - folding it without also stepping the sequence back would leave the
  // timestamp and the sequence naming different vblanks, which is the very
  // mismatch this guards against.
  double age = m_pollAge;
  if (age < 0.0)
    age += m_period;

  // Ahead of the unwatched branch below, because that one installs a reference
  // too: left unchecked there, an untrustworthy reading would be compared
  // against on the next refresh and produce the artefact one refresh later.
  // Counted rather than dropped quietly, so that if the guard ever takes a real
  // bite out of the coverage the report says so.
  const double guard = std::min(EDGE_GUARD_US, EDGE_GUARD_MAX * m_period);
  if (age < guard || age > m_period - guard)
  {
    m_unwatched += static_cast<unsigned int>(gap);
    m_classHaveRef = false;
    return;
  }

  if (gap > 1)
  {
    // Nobody looked at the refreshes in between, and this one only tells us
    // where the picture got to, not how it got there. All of them counted, so
    // a run that stopped watching cannot read as a run with nothing to report.
    m_unwatched += static_cast<unsigned int>(gap);
    m_lastClassTicks = ticks;
    return;
  }

  const uint32_t was = m_lastClassTicks;
  m_lastClassTicks = ticks;

  // Signed, so the 90kHz counter wrapping reads as a small step rather than as
  // four billion ticks of loss.
  const double perRefresh = m_period * (90000.0 / DVD_TIME_BASE);
  const double frames = static_cast<double>(static_cast<int32_t>(ticks - was)) / perRefresh;

  // The driver keeps publishing the old timestamp across a flush, so after a
  // stand-down the count resumes only once a genuinely new frame appears - and
  // the jump to it is the seek, not a loss.
  if (m_classWaitMove)
  {
    if (frames > 0.5)
      m_classWaitMove = false;
    return;
  }

  if (frames > LOST_AT_MOST || frames < -0.5)
  {
    // A seek, a stream change or a pts discontinuity: not frames falling out of
    // a running picture, which is the only thing this set out to count.
    ++m_resyncs;
    StandDown();
    return;
  }

  if (frames < 0.5)
  {
    // Nothing is committed while the picture stands still, because a held
    // refresh and the first refresh of a freeze look identical. Which it was is
    // known only when the picture moves again, or when it fails to.
    if (++m_heldRun < HELD_IS_A_STOP)
      return;

    // It failed to. A pause, a seek or trick play says so first, through
    // NoteClockDisturbed; a decoder running dry or a plane hanging says nothing
    // at all, and that is a fault the viewer can see.
    if (!m_classArmed || aml_monotonic_us() - m_lastDisturbUs < ANNOUNCED_WITHIN_US)
    {
      // Either something announced it, or the picture has not yet run clean
      // once - and a file catching up to the clock at its start holds frames on
      // purpose, which is the whole reason for the settling period.
      ++m_resyncs;
    }
    else
    {
      ++m_stalls;
      WarnFrame("held, the picture stopped for", m_heldRun, ticks);
    }
    StandDown();
    return;
  }

  // The picture moved, so whatever run sat behind it was a hold after all.
  m_heldPending = m_heldRun;
  m_heldRun = 0;

  if (frames < 1.5)
  {
    if (m_classArmed)
      ++m_shown;
    if (m_heldPending == 0)
      ++m_cleanRun;
  }
  else
  {
    const unsigned int lost = static_cast<unsigned int>(std::lround(frames)) - 1;
    m_cleanRun = 0;
    if (m_classArmed)
    {
      m_lost += lost;
      ++m_lostOn;
      // The frame that went missing is the one after the last seen, not the one
      // that turned up in its place. Reported together with the hold it came
      // with, because the pair is the whole signature - split across the
      // warning interval, only half of it would ever be seen.
      WarnFrame(m_heldPending > 0 ? "repeated, then lost" : "never displayed", lost,
                was + static_cast<uint32_t>(perRefresh));
    }
  }

  if (m_heldPending > 0)
  {
    if (m_classArmed)
    {
      m_held += m_heldPending;
      // A hold with no loss behind it is a clock losing ground to the display -
      // the commonest real fault, and the one the drift line predicts. Reported
      // here because the loss branch above has already spoken for the pair.
      if (frames < 1.5)
        WarnFrame("repeated", m_heldPending, was);
    }
    m_cleanRun = 0;
    m_heldPending = 0;
  }

  if (!m_classArmed)
  {
    ++m_seenRun;
    const unsigned int settle =
        static_cast<unsigned int>(SETTLE_SECONDS * DVD_TIME_BASE / m_period);
    if (m_cleanRun >= settle || m_seenRun >= settle * SETTLE_GIVE_UP)
      m_classArmed = true;
  }
}

void CAMLLatency::Poll()
{
  m_pollValid = false;

  // Requests raised on the decode thread, acted on here where the members live.
  // Taken before anything can return: a rate of zero across a mode change is
  // exactly when a request is in flight, and dropping it would strand m_lastN.
  if (m_forget.exchange(false, std::memory_order_relaxed))
  {
    // The synchronous half already cleared the store, but a publish may have
    // overtaken it, so clear it again here rather than assume.
    CAMLLatencyStore::GetInstance().Forget();
    m_lastN = 0;
    Rearm();
  }
  if (m_rearm.exchange(false, std::memory_order_relaxed))
    Rearm();
  if (m_classReset.exchange(false, std::memory_order_relaxed))
  {
    // Kept as a time, not a flag: a stop has to be attributable to it seconds
    // later, and that is what tells a pause from a picture that simply froze.
    m_lastDisturbUs = aml_monotonic_us();
    StandDown();
  }

  const double rate = static_cast<double>(CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS());
  if (rate <= 0.0)
    return;

  // A different refresh period voids the answer: it was published in
  // milliseconds, and the lowest-count rule below cannot raise it again.
  const double period = DVD_TIME_BASE / rate;
  if (period != m_period)
  {
    m_period = period;
    CAMLLatencyStore::GetInstance().Forget();
    m_lastN = 0;
    Rearm();
  }

  // Bracket the timestamp read with the vblank counter. If it moved, the read
  // straddled a refresh and there is no telling which one the frame belongs to.
  const auto seq0 = aml_vblank_seq_and_age(2.0 * period);
  const std::optional<uint32_t> ticks = aml_displayed_pts_ticks();
  const auto seq1 = aml_vblank_seq_and_age(2.0 * period);
  if (!seq0 || !ticks || !seq1 || seq0->first != seq1->first)
    return;

  // Counted for the periodic report rather than once at startup: the first
  // seconds of a title are the one time the GUI is certainly dirty and the loop
  // certainly paced, so a figure taken there would describe the wrong state.
  // Only clean readings are counted, which are the ones that cost anything.
  ++m_polls;
  if (seq1->first != m_cadenceSeq)
  {
    ++m_cadenceRefreshes;
    m_cadenceSeq = seq1->first;
  }

  m_pollValid = true;
  m_pollAge = seq1->second;
  m_pollTicks = *ticks;
  m_pollSeq = seq1->first;
  m_pollHostUs = aml_monotonic_us();

  if (m_armed)
  {
    // Only a reading that catches the change is worth having: seeing the frame
    // already up says nothing about which refresh it arrived on.
    if (m_haveShown && *ticks != m_lastTicks && *ticks == m_key)
    {
      const uint64_t n = seq1->first + RDMA_COMMIT_VBLANKS - m_seqArmed;
      if (n >= 1 && n <= MAX_VBLANKS)
      {
        // The lowest is the answer because both ends are biased the same way:
        // the arm takes the refresh the poll saw, which is never later than the
        // hand-over, and the detection is never earlier than the toggle. Neither
        // can read low, so the floor is the truth. A one-way rule has no way back
        // from a wrong low reading, though, so a lower answer has to turn up
        // twice running - and every reading confirms or replaces the candidate,
        // so a stray cannot wait an hour for a partner.
        if (n != m_candidate)
        {
          m_candidate = n;
          m_agreed = 1;
        }
        else
        {
          ++m_agreed;
        }

        // Settled once, then held until the next seek, which re-opens it. The
        // published figure sets the renderer's schedule, so a count that keeps
        // moving moves the picture by a whole refresh - and because only a lower
        // count was ever accepted, which one a run ended up with depended on where
        // the polling happened to fall when the title started. That is a whole
        // frame between one playback and the next, for no reason the viewer sees.
        if (!m_settled && (m_lastN == 0 || n < m_lastN) && m_agreed >= AGREE_BEFORE_PUBLISHING)
        {
          CAMLLatencyStore::GetInstance().Set(static_cast<float>(n * 1000.0 / rate));
          CLog::Log(LOGDEBUG, LOGVIDEO, "CAMLLatency: pipeline {} refreshes, {:.1f}ms", n,
                    n * 1000.0 / rate);
          m_lastN = n;
        }

        if (!m_settled && m_lastN != 0 && n == m_lastN && m_agreed >= AGREE_BEFORE_SETTLING)
        {
          m_settled = true;
          // Said out loud, and at INFO, because which regime is in force decides
          // whether the clock is stepped by an error or by whole frames - and a
          // run in the other regime is indistinguishable in the log from a run
          // that simply did not repeat.
          CLog::Log(LOGINFO,
                    "CAMLLatency: pipeline settled at {} refreshes ({:.1f}ms), clock sync {}",
                    n, n * 1000.0 / rate, m_clockSync ? "on" : "off");
        }
      }
      m_armed = false;
    }
    else if (seq1->first - m_seqArmed >= MAX_VBLANKS)
    {
      // Bounded by the same span a reading is accepted within, so the wait has
      // no region that could only produce an answer that would be thrown away.
      // Unsigned, so a vblank counter that resets trips this and re-arms rather
      // than counting on.
      m_armed = false;
    }
  }

  ClassifyRefresh(seq1->first, *ticks);

  m_lastTicks = *ticks;
  m_haveShown = true;
}

void CAMLLatency::Update(
    CDVDClock& clock, uint64_t ptsUs, double fps, double audioSyncError, bool clockSync)
{
  if (!m_pollValid || m_period <= 0.0)
  {
    // Kept current even here, so that a regime change across a seek or a mode
    // change is not read from a value taken before it.
    m_clockSync = clockSync;
    return;
  }

  m_clockSync = clockSync;

  // Decided on every presented frame. Settled once per report instead, it would
  // spend the first minute of every file false - so the check would never run,
  // and the report would then print a clean sheet it had not earned.
  const double refreshes = aml_refreshes_per_frame(DVD_TIME_BASE / m_period, fps);
  const bool steady = refreshes > 0.0 && std::abs(std::round(refreshes) - refreshes) <= 0.0005;
  m_ratioIsOne = steady && std::round(refreshes) == 1.0;
  if (m_ratioIsOne != m_ratioWas)
  {
    // Otherwise the check going quiet has no cause anywhere in the log.
    // Component-less, like the warnings: the line this explains can be a
    // warning, and a reader with debug logging off would otherwise see a report
    // that judged nothing and find no reason for it anywhere.
    CLog::Log(LOGINFO, "CAMLLatency: frame check {} ({:.3f} refreshes per frame)",
              m_ratioIsOne ? "on" : "off", refreshes);
    m_ratioWas = m_ratioIsOne;
  }

  // Scoped so that a sample this guard turns away does not stop the frame
  // being armed, which needs none of it. The poll happened earlier in this
  // pass, with a V4L2 ioctl and a run of sysfs writes in between, so its age
  // is carried forward to the clock read rather than differencing two moments
  // hundreds of microseconds apart - hence the clock read below and not above.
  // Passing the clock into the poll would undo the one thing keeping the count
  // safe from anything that steps it.
  const double carried = aml_monotonic_us() - m_pollHostUs;
  if (carried >= 0.0 && carried <= m_period)
  {
    // pts_video names the frame that appears at the next vblank, so the one on
    // screen is a refresh older and its timestamp fell due a period sooner.
    double absolute;
    const double clockNow = clock.GetClock(absolute);

    // Inside the blanking interval the kernel extrapolates to the frame about
    // to start, so the age comes back negative and the sequence names the next
    // vblank rather than the last. pts_video still names the last one, so put
    // both back on it - the same fold CWinSystemAmlogic::SampleFrameLatency
    // does for the phase. Left out, a reading lands a whole period wrong.
    double age = m_pollAge;
    uint64_t seq = m_pollSeq;
    if (age < 0.0)
    {
      age += m_period;
      --seq;
    }
    age += carried;

    const double shownUs = static_cast<double>(m_pollTicks) * (DVD_TIME_BASE / 90000.0);
    const double alignment = clockNow - age - shownUs +
                             static_cast<double>(RDMA_COMMIT_VBLANKS) * m_period;
    CAMLLatencyStore::GetInstance().SetAlignment(static_cast<float>(alignment / 1000.0));
    m_lastAlign = alignment;

    if (m_reports == 0)
    {
      m_alignMin = m_alignMax = alignment;
      m_since = absolute;

      // Started here rather than at the first poll: everything before the clock
      // runs is the paced regime, and folding it in would drag the figure toward
      // the answer this exists to test for.
      m_polls = 0;
      m_cadenceRefreshes = 0;
    }
    else
    {
      m_alignMin = std::min(m_alignMin, alignment);
      m_alignMax = std::max(m_alignMax, alignment);
    }
    m_alignSum += alignment;
    ++m_reports;

    // Sampled here rather than on every frame so the two summaries cover the
    // same refreshes and can be subtracted. The reading behind it only changes
    // about once a second - ActiveAE averages over that interval - so this is a
    // time weighting of those, and the spread is theirs, not this loop's.
    if (audioSyncError != DVD_NOPTS_VALUE)
    {
      if (m_audioReports == 0)
        m_audioMin = m_audioMax = audioSyncError;
      else
      {
        m_audioMin = std::min(m_audioMin, audioSyncError);
        m_audioMax = std::max(m_audioMax, audioSyncError);
      }
      m_audioSum += audioSyncError;
      m_leadSum += audioSyncError + alignment;
      ++m_audioReports;
    }

    if (absolute - m_since >= TIME_BETWEEN_REPORTS)
    {
      const double mean = m_alignSum / m_reports;
      CLog::Log(LOGDEBUG, LOGVIDEO,
                "CAMLLatency: alignment {:+.2f}ms [{:+.2f}..{:+.2f}] over {} frames, "
                "{:.1f} clean polls per refresh",
                mean / 1000.0, m_alignMin / 1000.0, m_alignMax / 1000.0, m_reports,
                m_cadenceRefreshes > 0
                    ? static_cast<double>(m_polls) / static_cast<double>(m_cadenceRefreshes)
                    : 0.0);

      // The picture's own figure says nothing about lip sync on its own - the
      // sound is left alone by ActiveAE once it is within 30ms and answered
      // after that by stepping the master clock, not the audio, and only past a
      // 50ms band - so the other half is said beside it, in the same units and
      // over the same window. Signs are opposite at source and reconciled here: the
      // alignment is how late the picture was, the audio error is how early the
      // sound was, so the lead is their sum. Said even when there is nothing to
      // say, because a line that goes missing reads as a clean sheet.
      if (m_audioReports > 0)
      {
        // The picture term is restated over the frames the audio was measured
        // on, not the whole window: the line above averages every frame, and
        // where the two counts differ - any window holding a start, a seek or a
        // track change - the three numbers would not add up and the report
        // would read as broken.
        CLog::Log(LOGDEBUG, LOGVIDEO,
                  "CAMLLatency: audio {:+.2f}ms [{:+.2f}..{:+.2f}] over {} frames, "
                  "picture {:+.2f}ms over those, {:+.2f}ms ahead at the connector",
                  m_audioSum / m_audioReports / 1000.0, m_audioMin / 1000.0, m_audioMax / 1000.0,
                  m_audioReports, (m_leadSum - m_audioSum) / m_audioReports / 1000.0,
                  m_leadSum / m_audioReports / 1000.0);
      }
      else
      {
        CLog::Log(LOGDEBUG, LOGVIDEO, "CAMLLatency: audio not measured");
      }

      // The span comes first because the faults mean nothing without it: a
      // report that checked no refreshes has to read as exactly that, and not
      // as a clean run.
      // Always said, even when it judged nothing: a line that does not appear
      // is indistinguishable from a check that found nothing wrong, which is
      // the one thing this must never be mistaken for. How much of the window
      // was judged is stated too, so a zero can be weighed rather than trusted,
      // and the alignment's own count of swallowed refreshes sits beside it as
      // a second opinion arrived at from entirely different arithmetic.
      // Filled in by the drift block below, which is why this is reported after
      // it rather than before: taken here, the count would be the previous
      // window's, which is a plausible wrong number rather than an obvious one.
      bool haveSwallowed = false;
      int swallowedThisWindow = 0;

      // The vblank edge, not the instant this ran: those sit a whole and
      // differing part of a period later, which on a short baseline swamps the
      // rate entirely. absolute is CLOCK_MONOTONIC_RAW, the 24MHz crystal that
      // nothing disciplines, which is what makes it worth measuring against.
      const double vblankAt = absolute - age;

      if (m_disturbed.exchange(false, std::memory_order_relaxed) && m_driftAnchored)
      {
        // Said out loud, like the guard below: a baseline that quietly restarts
        // every time produces no drift line at all, and an absent line reads
        // exactly like a measurement that found nothing.
        CLog::Log(LOGDEBUG, LOGVIDEO, "CAMLLatency: drift baseline dropped, clock disturbed");
        m_driftAnchored = false;
      }

      if (!steady)
      {
        m_driftAnchored = false;
      }
      else if (!m_driftAnchored)
      {
        // Anchored at a report rather than at the first frame: the genlock
        // steps the clock in the opening seconds, and that is not drift.
        m_driftAnchored = true;
        m_driftAbsolute = m_driftPrevAbsolute = absolute;
        m_driftClock = m_driftPrevClock = clockNow;
        m_driftVblank = vblankAt;
        m_driftSeq = seq;
        m_driftAlign = m_lastAlign;
        m_driftWalkUs = 0.0;
        m_driftFrames = 0;
        m_driftSteps = 0;
      }
      else
      {
        const double elapsed = absolute - m_driftAbsolute;
        const double displayElapsed = vblankAt - m_driftVblank;
        const double clockRun = clockNow - m_driftClock;
        const double displayRun = static_cast<double>(seq - m_driftSeq) * m_period;

        // Between corrections the clock advances at exactly the reference rate,
        // so whatever it gained or lost over this one window is a correction
        // somebody made. Taking that out of the alignment leaves the refreshes
        // the pipeline swallowed to pay for it, which are what the walk has to
        // count rather than measure.
        const double delta = m_lastAlign - m_driftAlign;
        const double step =
            (clockNow - m_driftPrevClock) - (absolute - m_driftPrevAbsolute);
        const double frames = std::round((delta - step) / m_period);
        const double resid = (delta - step) - frames * m_period;
        if (std::abs(step) > DVD_MSEC_TO_TIME(20))
          ++m_driftSteps;
        m_driftPrevAbsolute = absolute;
        m_driftPrevClock = clockNow;
        m_driftAlign = m_lastAlign;

        // The cumulative pair is a backstop, deliberately wide enough to let
        // audio corrections through since those are the signal. The per-window
        // pair is the sharp one, and neither of its tolerances grows with the
        // baseline: a freeze must not get easier to absorb the longer the
        // measurement has been running.
        const double slack = 5.0e-4 * elapsed + DVD_MSEC_TO_TIME(250);
        const char* dropped = nullptr;
        if (elapsed <= 0.0 || displayElapsed <= 0.0)
          dropped = "no baseline";
        else if (std::abs(step) > DVD_MSEC_TO_TIME(350))
          // The dead band tops out at 80ms, so a step this size in one window is
          // a freeze or a seek that no hook caught, not a correction.
          dropped = "clock stepped";
        else if (std::abs(resid) > 0.35 * m_period)
          // Half a period from either neighbour there is no nearest refresh to
          // round to, so noise decides, and the wrong way buys a whole period of
          // invented drift. A 50ms dead band lands exactly there on 50Hz.
          dropped = "fold ambiguous";
        else if (std::abs(clockRun - elapsed) > slack ||
                 std::abs(displayRun - displayElapsed) > slack)
          dropped = "baseline adrift";

        if (dropped)
        {
          CLog::Log(LOGDEBUG, LOGVIDEO,
                    "CAMLLatency: drift baseline dropped after {:.0f}s, {}: step {:+.1f}ms "
                    "fold {:+.1f}ms clock {:+.1f}ms display {:+.1f}ms",
                    elapsed / DVD_TIME_BASE, dropped, step / 1000.0, resid / 1000.0,
                    (clockRun - elapsed) / 1000.0, (displayRun - displayElapsed) / 1000.0);
          m_driftAnchored = false;
        }
        else
        {
          m_driftWalkUs += delta - frames * m_period;
          m_driftFrames += static_cast<int>(frames);
          swallowedThisWindow = static_cast<int>(frames);
          haveSwallowed = true;

          const double walk = m_driftWalkUs / elapsed * 1e6;
          const double panel = (displayRun / displayElapsed - 1.0) * 1e6;
          const double audio = (clockRun - elapsed) / elapsed * 1e6;

          // Sign is the whole answer: a clock that gains on the display brings
          // frames due earlier every refresh until two fall in one, and the
          // second is dropped. Losing repeats one instead. The swallowed count
          // is the walk checking itself - a rate that predicts one repeat every
          // ten minutes and a count that says four is a fold gone wrong.
          CLog::Log(LOGDEBUG, LOGVIDEO,
                    "CAMLLatency: walk {:+.2f}ppm one {} per {:.0f}min ({:+d} swallowed), "
                    "panel {:+.2f}ppm, audio {:+.1f}ppm from {} steps, over {:.0f}s",
                    walk, walk >= 0.0 ? "drop" : "repeat",
                    std::abs(walk) > 0.01 ? m_period / std::abs(walk) / 60.0 : 0.0,
                    m_driftFrames, panel, audio, m_driftSteps, elapsed / DVD_TIME_BASE);
        }
      }

      // Said last, and always - even when it judged nothing. A line that does
      // not appear is indistinguishable from a check that found nothing wrong,
      // which is the one thing this must never be mistaken for. How much of the
      // window was judged is stated too, so a zero can be weighed rather than
      // trusted, and the alignment's own count of swallowed refreshes sits
      // beside it as a second opinion reached by different arithmetic. The two
      // are not meant to agree exactly: the genlock announces its corrections,
      // which stands this check down across the refreshes the fold counts.
      const unsigned int judged = m_shown + m_held + m_lostOn;
      const bool faulty = m_held > 0 || m_lost > 0 || m_stalls > 0;
      CLog::Log(faulty ? LOGWARNING : LOGDEBUG,
                "CAMLLatency: {} of {} refreshes judged, {} unwatched, {} repeated, "
                "{} lost over {}, {} resync, {} stall; alignment swallowed {}{}",
                judged, m_refreshes, m_unwatched, m_held, m_lost, m_lostOn, m_resyncs,
                m_stalls,
                haveSwallowed ? fmt::format("{}", swallowedThisWindow) : std::string("-"),
                m_hidden > 0 ? fmt::format(", {} warnings held back", m_hidden) : "");
      m_refreshes = m_shown = m_held = m_lost = m_lostOn = 0;
      m_unwatched = m_resyncs = m_stalls = 0;
      m_warned = m_hidden = 0;

      m_reports = 0;
      m_alignSum = 0.0;
      m_audioReports = 0;
      m_audioSum = 0.0;
      m_leadSum = 0.0;
      m_polls = 0;
      m_cadenceRefreshes = 0;
    }
  }

  // Hand-over: the frame going out now, and the refresh it went out on. Both
  // sides of the answer are counters, so there is no clock in the count at all.
  if (!m_armed)
  {
    // Zero is the driver's own "no timestamp" value, so it never appears on
    // screen; arming on it would only burn the wait.
    const uint32_t key = static_cast<uint32_t>((ptsUs * 9ULL) / 100ULL);
    if (key == 0)
      return;

    // The refresh the poll saw, not a fresh read: between the two sit
    // ReleaseFrame and a run of sysfs writes, and a vblank falling in that gap
    // would put the count one low - the one direction the ratchet cannot climb
    // back from.
    m_key = key;
    m_seqArmed = m_pollSeq;
    m_armed = true;
  }
}
