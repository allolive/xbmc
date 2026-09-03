/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>
#include <cstdint>

class CDVDClock;

//! \brief What the video pipeline costs, measured once rather than assumed.
//!
//! CGraphicContext::GetDisplayLatency() falls back to (buffers + 1) / fps when
//! the window system does not answer, and nothing checks it. Measured on g12b
//! that overstates the path by about two and a half frames, so every frame
//! reaches the screen that far before its timestamp. Audio has no matching
//! error, being written ahead by a measured CAudioSinkAE::GetDelay(), so the
//! two part company by whatever the video guess is wrong by.
class CAMLLatencyStore
{
public:
  static CAMLLatencyStore& GetInstance()
  {
    static CAMLLatencyStore store;
    return store;
  }

  //! \brief Milliseconds, or negative while unknown, which is what
  //! CWinSystemBase returns to ask for the buffer-count estimate instead.
  float Get() const { return m_latencyMs.load(std::memory_order_relaxed); }
  void Set(float ms) { m_latencyMs.store(ms, std::memory_order_relaxed); }
  void Forget()
  {
    m_latencyMs.store(-1.0f, std::memory_order_relaxed);
    m_alignmentMs.store(0.0f, std::memory_order_relaxed);
  }

  //! \brief How far the picture on screen is from where its timestamp says it
  //! should be, in milliseconds. Reported, never acted on.
  float GetAlignment() const { return m_alignmentMs.load(std::memory_order_relaxed); }
  void SetAlignment(float ms) { m_alignmentMs.store(ms, std::memory_order_relaxed); }

private:
  CAMLLatencyStore() = default;
  std::atomic<float> m_latencyMs{-1.0f};
  std::atomic<float> m_alignmentMs{0.0f};
};

//! \brief Counts the refreshes between handing a frame over and it appearing.
//!
//! The frame going out is stamped with the display's vblank counter. When the
//! driver reports that same timestamp on screen, the counter has advanced by
//! however many refreshes the path costs. That difference is a whole number by
//! construction, so there is no timebase to mix and nothing that steps the DVD
//! clock can reach it. The only statistic is an agreement count, which exists to
//! stop a single low reading latching, not to average anything.
//!
//! Published as whole refresh periods on purpose. The renderer can only put a
//! frame on a vblank, so the sub-period part of this belongs to
//! GetFrameLatencyAdjustment() and would only be counted twice here.
class CAMLLatency
{
public:
  //! \brief One instance: the matcher is polled from the renderer, which has no
  //! codec to reach through on a pass that presents no new frame.
  static CAMLLatency& GetInstance()
  {
    static CAMLLatency latency;
    return latency;
  }

  //! \brief Called at least once per render refresh; extra calls within one
  //! refresh are absorbed. Counts the refreshes between a frame
  //! being handed over and the driver reporting it on screen. Needs no clock -
  //! both ends are counters - so nothing that steps the clock can reach it.
  void Poll();

  //! \brief Called once per presented frame, with the timestamp being handed
  //! over. Arms the matcher and reports where the picture landed.
  void Update(CDVDClock& clock, uint64_t ptsUs, double fps);

  //! \brief Re-arm the matcher. A seek moves which frame is on screen, not the
  //! path it travels, so the measured answer still stands.
  void Restart();

  //! \brief Something moved the clock or the picture by more than any drift
  //! will. Ends the drift baseline and stands the frame check down until the
  //! picture runs again; the refresh count needs neither and is left be.
  void NoteClockDisturbed()
  {
    m_disturbed.store(true, std::memory_order_relaxed);
    m_classReset.store(true, std::memory_order_relaxed);
  }

  //! \brief Drop the published answer too. For a refresh-period change, and for
  //! the decoder that measured it going away.
  void Forget();

private:
  void Rearm();

  //! \brief Says so at once, outside the video component so it is visible with
  //! debug logging off, and stops talking after a few so a broken stream cannot
  //! bury the rest of the log.
  void WarnFrame(const char* what, unsigned int frames, uint32_t ticks);

  //! \brief Judge one refresh: did it put a new frame up, hold the last one, or
  //! jump past some. Keeps its own reference sample so the refresh count, which
  //! has different reset rules, is not disturbed.
  void ClassifyRefresh(uint64_t seq, uint32_t ticks);

  //! \brief Wait for the picture to run again, without ending the report
  //! window. For a pause, a seek, or anything that leaves pts_video standing.
  void StandDown();

  std::atomic<bool> m_rearm{false};   //!< raised by the decode thread
  std::atomic<bool> m_forget{false};  //!< raised by the decode thread

  double m_period{0.0};   //!< a change to this is what voids the answer
  bool m_armed{false};
  uint32_t m_key{0};        //!< the armed frame's timestamp, in 90kHz ticks
  uint64_t m_seqArmed{0};   //!< the refresh it was handed over on

  bool m_haveShown{false};
  uint32_t m_lastTicks{0};  //!< to catch the change rather than the steady state
  uint64_t m_lastN{0};      //!< the answer standing
  uint64_t m_candidate{0};  //!< a lower one, not yet believed
  unsigned int m_agreed{0}; //!< how many times running it has turned up

  //! \brief What the last poll read, so the report does not read it again.
  bool m_pollValid{false};
  double m_pollAge{0.0};
  uint32_t m_pollTicks{0};
  uint64_t m_pollSeq{0};
  unsigned int m_polls{0};
  unsigned int m_cadenceRefreshes{0};
  uint64_t m_cadenceSeq{0};
  double m_pollHostUs{0.0};

  //! \brief Anchors for the drift rates, over a baseline that keeps growing.
  //!
  //! The walk is the one measured directly, and the only one worth acting on:
  //! it is the alignment's own change with the whole refreshes the pipeline
  //! swallowed taken back out, so it depends on nothing but a per-frame reading
  //! and a count. The panel rate is measured too, by the vblank counter, which
  //! shares nothing with the alignment except the answer it should agree with.
  //!
  //! The audio rate is neither. The clock only moves when the loop's error
  //! passes its 50ms dead band (111ms on TrueHD passthrough), so it resolves no
  //! better than that over the baseline - a couple of dozen ppm across a whole
  //! title - and rests on however few corrections happened to fire. The step
  //! count is printed beside it so the figure can be discounted properly.
  std::atomic<bool> m_disturbed{false};  //!< raised by whatever moved the clock
  bool m_driftAnchored{false};
  double m_driftAbsolute{0.0};  //!< the instant this ran, for the clock
  double m_driftVblank{0.0};    //!< the vblank edge, for the refresh count
  double m_driftClock{0.0};
  uint64_t m_driftSeq{0};
  double m_driftPrevAbsolute{0.0};  //!< the previous report, to catch a step
  double m_driftPrevClock{0.0};
  double m_lastAlign{0.0};   //!< the newest sample, not the window mean
  double m_driftAlign{0.0};  //!< the same at the previous report
  double m_driftWalkUs{0.0};
  int m_driftFrames{0};  //!< refreshes swallowed, the walk's own check
  unsigned int m_driftSteps{0};

  //! \brief Conservation of frames, checked one refresh at a time.
  //!
  //! Where the display shows each frame once, every refresh must put a new
  //! frame on screen and every frame must appear exactly once. pts_video names
  //! the frame on screen and the vblank counter counts the refreshes, so
  //! comparing the two across a single refresh says which rule broke. Kodi
  //! counts a frame its own renderer discards, but nothing counts one that goes
  //! missing below it, which is what this is for.
  //!
  //! The span checked is reported with the faults, because a count of zero says
  //! nothing unless it is known that something was looked at.
  std::atomic<bool> m_classReset{false};  //!< raised by whatever disturbed it

  bool m_ratioIsOne{false};      //!< one frame per refresh, so the rules apply
  bool m_classArmed{false};      //!< past the drops a start makes on purpose
  bool m_classHaveRef{false};    //!< holding a sample to compare against
  bool m_classWaitMove{true};    //!< pts stale or standing; wait for it to move
  uint32_t m_lastClassTicks{0};  //!< the check's own, not the matcher's
  uint64_t m_lastClassSeq{0};
  unsigned int m_cleanRun{0};  //!< consecutive good refreshes, for arming
  unsigned int m_seenRun{0};   //!< refreshes since the re-arm, to arm anyway
  unsigned int m_heldRun{0};   //!< consecutive held refreshes, to spot a stop

  double m_lastDisturbUs{0.0};    //!< when something said the picture would stop
  unsigned int m_heldPending{0};  //!< a run not yet known to be a hold or a stop
  bool m_ratioWas{false};         //!< to say so when the check turns on or off

  unsigned int m_refreshes{0};  //!< every refresh the window spanned
  unsigned int m_shown{0};      //!< refreshes that put a new frame up
  unsigned int m_held{0};       //!< refreshes that showed the same one again
  unsigned int m_lost{0};       //!< frames that never reached the screen
  unsigned int m_lostOn{0};     //!< refreshes those were noticed on
  unsigned int m_unwatched{0};  //!< refreshes no usable poll landed on
  unsigned int m_resyncs{0};    //!< a stop that was announced: not a fault
  unsigned int m_stalls{0};     //!< a stop that was not: the picture froze
  double m_lastWarnUs{0.0};
  unsigned int m_warned{0};
  unsigned int m_hidden{0};

  //! \brief Where the picture landed, gathered for the periodic report.
  unsigned int m_reports{0};
  double m_since{0.0};
  double m_alignSum{0.0};
  double m_alignMin{0.0};
  double m_alignMax{0.0};
};

