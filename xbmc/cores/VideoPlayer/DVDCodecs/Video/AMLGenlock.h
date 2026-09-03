/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/StreamDetails.h"

#include <atomic>
#include <cstdint>

class CDVDClock;
class CProcessInfo;

//! \brief Aligns the phase of the player's clock to the display's frames.
//!
//! The renderer picks the frame to show at each vsync by comparing the master
//! clock against the frame's timestamp. Where those timestamps fall inside the
//! refresh period is settled by chance when playback starts, and near the
//! decision boundary jitter shows one frame twice and drops the next. Handles
//! the exact rate match, where CheckEnableClockSync() stands down.
//!
//! The phase only, never the rate, once per trigger. The move is at most half a
//! refresh period, which is also the most it shifts video against audio - and
//! under a whole period, so the renderer absorbs it without adding or dropping.
//!
//! Aligning once is not enough on its own. The audio loop steps the master clock
//! whenever its own error passes its dead band, which at a start happens a second
//! or two after the alignment was made, and the picture is then off by whatever
//! that step left over once the pipeline had swallowed whole refreshes. Measured
//! at 20ms on a 61ms correction. So a step nobody here made asks for the
//! alignment again.
class CAMLGenlock
{
public:
  //! \brief Aligns the clock if an alignment is owed. Called once per frame on
  //! the frame loop, as the frame is handed to the display. Reads what it
  //! needs itself, so the frames that owe nothing pay only for the clock.
  //! \param clock the player's master clock, not retained
  //! \param processInfo the player's process info, not retained
  //! \param hdrType the stream's HDR type, for the latency the renderer applies
  //! \param framePts the frame's timestamp, in master clock units
  void Update(CDVDClock& clock,
              CProcessInfo& processInfo,
              StreamHdrType hdrType,
              double framePts);

  //! \brief Asks for an alignment on the next frame. Called from the player
  //! thread on reset and speed change, and from Update() itself. A counter, so
  //! that a request raised while Update() is running is not cleared by it.
  void Restart() { ++m_alignRequests; }

  //! \brief Drop the clock reading held for the step check, without asking for
  //! an alignment. For the frames the caller declines to offer at all: across
  //! one of those the clock and its reference need not have moved together, and
  //! differencing over the gap would read the whole of it as a step.
  void Forget() { m_haveClock = false; }

private:
  std::atomic<uint32_t> m_alignRequests{1};

  //! \brief The clock against the reference it is derived from, so that a step
  //! in it can be told from it simply running. App thread only.
  bool m_haveClock{false};
  double m_lastClock{0.0};
  double m_lastAbsolute{0.0};

  //! \brief The request Update() last satisfied. App thread only, as are the
  //! members below.
  uint32_t m_alignDone{0};

  double m_period{0.0};
};
