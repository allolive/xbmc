/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>

//! \brief Holds the audio clock in step with the master clock.
//!
//! Passthrough audio is generated from mpll0 and played at whatever rate that
//! divider happens to give. Measured on g12b at 23.976 it is 20ppm slow, which
//! is 72ms of lip sync an hour, and in passthrough nothing downstream can take
//! it out: a bitstream cannot be resampled, so the audio engine's only move is a
//! whole IEC frame, three orders of magnitude coarser than the error.
//!
//! So the rate is corrected where it is made. The kernel exposes the divider's
//! sigma-delta field a step at a time; a step is far larger than the error, so
//! the level alternates between the two that bracket it and the average lands
//! where neither can. Dithered every quarter second, which leaves a ripple of
//! about 8 microseconds against a frame of 41700.
//!
//! Steps in twos. The hardware ignores the field's low bit and rounds up, so
//! consecutive values are not distinct - moving by two is the smallest change
//! that always lands somewhere new, whatever the base happens to be.
//!
//! Carries no figure for any rate. The step is worth a different amount at every
//! sample rate, since the divisor differs, so the loop integrates rather than
//! calculates: it only has to have the sign right, and it converges from any
//! starting point without being told what the hardware is.
class CAMLAudioTrim
{
public:
  static CAMLAudioTrim& GetInstance()
  {
    static CAMLAudioTrim trim;
    return trim;
  }

  //! \brief Whether this kernel carries the trim knob at all.
  static bool Supported();

  //! \brief Called once per presented frame. Paces itself.
  //! \param error audio ahead of the master clock in DVD time units, or nullopt
  //! when nothing measured it this frame. Must be the unscaled figure: this is a
  //! rate loop, and a reading shrunk by a constant is a gain error.
  //! \param lead how far the audio leads the picture at the box output, in DVD
  //! time units, or nullopt when nothing has measured it. Only acted on where a
  //! cap has been set for it; without one this is a rate loop and nothing else.
  //! \param now the master clock's absolute time, in DVD time units
  void Update(const std::optional<double>& error,
              const std::optional<double>& lead,
              double now);

  //! \brief Give the clock back and forget everything. For a decoder closing:
  //! the level must not outlive the playback that asked for it.
  void Release();

  //! \brief Forget the measurement but keep the level. For a seek or a pause -
  //! they move the error by hand, and an interval spanning that is not a rate,
  //! but the rate the silicon runs at has not changed and the correction for it
  //! is still the right one.
  void Forget();

  ~CAMLAudioTrim();

private:
  CAMLAudioTrim() = default;
  CAMLAudioTrim(const CAMLAudioTrim&) = delete;
  CAMLAudioTrim& operator=(const CAMLAudioTrim&) = delete;

  bool Armed();

  //! \brief The rate error the loop should hold to walk the offset out. Zero
  //! when there is no offset to walk out, or nothing said how fast it may be.
  double Aim() const;

  //! \brief The aim expressed as levels, applied without being integrated
  //! toward. The plant gain is known to better than a percent, so the level a
  //! rate needs is arithmetic, not something to be discovered.
  double Feedforward() const;

  std::optional<double> ReadSlew() const;
  void ForgetLocked();
  std::optional<int> ReadLevel() const;
  bool WriteLevel(int level);
  void Settle(double drift);

  bool m_checked{false};      //!< whether the knob has been looked for
  bool m_present{false};      //!< whether it was found
  bool m_everWrote{false};    //!< whether anything needs putting back

  int m_applied{0};           //!< what the register was last set to
  double m_want{0.0};         //!< the level the loop wants, in steps, fractional
  double m_dither{0.0};       //!< carries the fraction between dither slots

  double m_bucketStart{0.0};
  double m_lastSampleAt{0.0}; //!< when a sample was last taken, to spot a gap  //!< when the current averaging bucket opened
  double m_bucketSum{0.0};
  unsigned int m_bucketCount{0};
  double m_lastMean{0.0};
  bool m_haveLastMean{false};

  double m_lastError{0.0};    //!< to catch a clock step, which is not a rate
  bool m_haveLastError{false};

  //! The last few slopes. Acted on by their middle value, so one corrupted by a
  //! clock step too small for the jump guard is outvoted rather than believed.
  std::array<double, 3> m_slopes{};
  unsigned int m_slopeCount{0};

  unsigned int m_refused{0};  //!< consecutive writes the clock would not take
  unsigned int m_clamped{0};  //!< decisions at the limit with no answer
  double m_driftAtZero{0.0};  //!< the rate before the level moved, to judge that
  bool m_haveDriftAtZero{false};  //!< decisions spent at the limit with no answer
  bool m_stopped{false};

  mutable std::mutex m_mutex; //!< Release() comes from the player thread

  double m_lastDither{0.0};   //!< when the level was last put out

  double m_lead{0.0};         //!< audio ahead of picture at the output
  bool m_haveLead{false};

  double m_slew{0.0};         //!< the largest rate error the offset may ask for
  std::optional<double> m_slewSeen; //!< the previous read, to confirm a change
  bool m_haveSlew{false};     //!< whether the offset loop is switched on at all
  double m_levelMax{2.0};     //!< MAX_LEVEL_RATE, or more once it is
};
