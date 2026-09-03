/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AMLGenlock.h"

#include "AMLLatency.h"

#include "ServiceBroker.h"
#include "cores/VideoPlayer/DVDClock.h"
#include "cores/VideoPlayer/Process/ProcessInfo.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/AMLUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <cmath>

namespace
{
// Below this a reading is measurement noise.
constexpr double PHASE_NOISE = DVD_MSEC_TO_TIME(0.5);

// Between corrections the clock advances at exactly the rate of the reference it
// is derived from, so any difference at all is a step somebody made. Compared
// against a threshold only to leave room for arithmetic, not for drift: the
// smallest dead band the audio loop can be set to is 20ms.
constexpr double CLOCK_STEPPED = DVD_MSEC_TO_TIME(10.0);

} // unnamed namespace

void CAMLGenlock::Update(CDVDClock& clock,
                         CProcessInfo& processInfo,
                         StreamHdrType hdrType,
                         double framePts)
{
  // A mode change voids any earlier alignment.
  const double rate = static_cast<double>(CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS());
  const double period = rate > 0.0 ? DVD_TIME_BASE / rate : 0.0;
  if (period != m_period)
  {
    m_period = period;
    Restart();
  }

  // A step in the clock voids it too. Ours is not one: the read below is retaken
  // after a correction, so the alignment does not see its own work and ask for
  // itself again.
  double absolute;
  const double clockNow = clock.GetClock(absolute);
  if (m_haveClock && std::abs((clockNow - m_lastClock) - (absolute - m_lastAbsolute)) >
                         CLOCK_STEPPED)
    Restart();
  m_lastClock = clockNow;
  m_lastAbsolute = absolute;
  m_haveClock = true;

  // Read after the triggers above, so a request they raised is counted here,
  // and one raised later is left for the next frame.
  const uint32_t requested = m_alignRequests.load();
  const bool needsAlign = requested != m_alignDone;

  // Nothing to aim with until the pipeline has been measured: until then
  // GetDisplayLatency() is CGraphicContext's buffer-count estimate, which is a
  // whole frame or more out, and aiming with it would step the clock by that
  // error. Only a refresh-period change clears it, which is the one event that
  // makes the measured path wrong.
  if (CAMLLatencyStore::GetInstance().Get() < 0.0f)
    return;

  // Stand down while CRenderManager centres the frame itself. Its own flag
  // rather than a second rate comparison: it decides from CRenderManager::m_fps
  // and this would decide from CProcessInfo, and the two can disagree.
  if (processInfo.IsRenderClockSync())
    return;

  // Whole-number ratios only, so pulldown, where there is no fixed phase, is
  // left alone. Compared with a tolerance: the two rates come from different
  // sources and exact equality on a ratio of floats does not hold.
  const double refreshes = aml_refreshes_per_frame(rate, processInfo.GetVideoFps());
  if (period <= 0.0 || refreshes <= 0.0 || std::abs(std::round(refreshes) - refreshes) > 0.0005)
    return;

  // Aligned only where playback is already discontinuous - a start, a seek, a
  // speed change - and left alone after that. The display and the clock do not
  // walk apart while playing, and stepping the clock under a picture that is
  // running smoothly is exactly what must not happen.
  if (!needsAlign)
    return;

  // Two periods of slack: the offset is taken modulo the period below, so a
  // whole period of read delay cancels.
  const std::optional<double> sinceFrameStart = aml_since_frame_start_us(2.0 * period);
  if (!sinceFrameStart)
    return;

  // Read the clock again next to the vblank reading: the two are differenced,
  // so whatever ran between the reading at the top and here would bias it.
  const double clockAtRead = clock.GetClock();
  const double latency =
      aml_render_display_latency(hdrType, processInfo.GetVideoSettings().m_AudioDelay);

  // Aim at zero: a frame should be painted when its timestamp says, once the
  // latency the renderer schedules it with is taken into account. Only the
  // position within one period matters, so fold into the half period either
  // side.
  // Aim half a period off when the display shows each frame more than once.
  // There the renderer flips from its early branch, whose threshold is a whole
  // period below the late one, so aiming at zero puts both of them exactly on a
  // vblank; half a period leaves each of them half a period of slack.
  const double offset = std::remainder(clockAtRead - *sinceFrameStart + latency - framePts -
                                           (std::round(refreshes) == 1.0 ? 0.0 : period / 2.0),
                                       period);

  CLog::Log(LOGDEBUG, LOGVIDEO, "CAMLGenlock: phase {:+.2f}ms", offset / 1000.0);

  if (std::abs(offset) > PHASE_NOISE)
  {
    // While the vsync adjust is running ErrorAdjust quantises anything this
    // small to a whole frame or to nothing, so leave the alignment owed.
    if (clock.GetVsyncAdjust() != 0.0)
      return;

    // Not corrected past the audio loop's own dead band. A step that large can
    // by itself push the audio error over the threshold, and the correction
    // that answers it comes back here as another step - which would settle into
    // a standing oscillation rather than an alignment. Refusing leaves it owed,
    // which is the same thing this does for every other reason it cannot act.
    if (std::abs(offset) >= DVD_MSEC_TO_TIME(processInfo.GetMaxPassthroughOffSyncDuration()))
      return;

    // ErrorAdjust holds the clock lock across the read and the write, and
    // returns zero if it declines while the player is slewing the clock.
    const double applied = clock.ErrorAdjust(-offset, "CAMLGenlock");
    if (applied == 0.0)
      return;

    // An alignment that could not be made when it was asked for is still owed,
    // so this can land well into a title rather than only at the start. It is a
    // step in the clock like any other, and reads as drift if left in.
    CAMLLatency::GetInstance().NoteClockDisturbed();

    // The step just made is this class's own, so it must not come back round as
    // a reason to align again. Carried into the baseline rather than dropping
    // it: ErrorAdjust returns exactly what it applied, and dropping the baseline
    // would blind the detector for a frame - which is precisely when the audio
    // loop's own correction tends to land, and it would go unseen.
    m_lastClock += applied;
  }

  m_alignDone = requested;
}
