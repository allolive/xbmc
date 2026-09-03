/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/AudioEngine/Interfaces/AE.h"
#include "cores/AudioEngine/Interfaces/AEStream.h"
#include "cores/AudioEngine/Utils/AEChannelInfo.h"
#include "threads/CriticalSection.h"

#include <atomic>
#include <mutex>

#include "PlatformDefs.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

typedef struct stDVDAudioFrame DVDAudioFrame;

class CDVDClock;

class CAudioSinkAE : IAEClockCallback
{
public:
  explicit CAudioSinkAE(CDVDClock *clock);
  ~CAudioSinkAE() override;

  void SetVolume(float fVolume);
  void SetDynamicRangeCompression(long drc);
  void Pause();
  void Resume();
  bool Create(const DVDAudioFrame &audioframe, AVCodecID codec, bool needresampler);
  bool IsValidFormat(const DVDAudioFrame &audioframe);
  void Destroy(bool finish);
  unsigned int AddPackets(const DVDAudioFrame &audioframe);
  double GetPlayingPts();
  double GetCacheTime();
  double GetCacheTotal(); // returns total time a stream can buffer
  double GetMaxDelay(); // returns total time of audio in AE for the stream
  double GetDelay(); // returns the time it takes to play a packet if we add one at this time
  double GetSyncError();

  //! \brief Whether there is a measurement at all. GetSyncError() reads zero
  //! both when the audio is exactly on the clock and when the engine has not
  //! measured it yet, and those are not the same answer.
  bool HasSyncError() const { return m_syncErrorTime != 0 && m_syncErrorRawValid; }

  //! \brief The same error without the scaling the engine applies to it - what
  //! the pipeline is really doing, for reporting rather than correcting.
  double GetSyncErrorRaw() const { return m_syncErrorRaw; }

  //! \brief Whether the sink is carrying a bitstream rather than samples.
  bool IsPassthrough() const { return m_bPassthrough; }

  //! \brief Whether a correction is still owed from the last resync.
  //!
  //! The engine declares a stream synchronised once its averaged error is inside
  //! 30ms, and in passthrough it can only move audio in whole IEC frames - 20ms
  //! for TrueHD - so it stops with a residue it has no way to remove. That
  //! residue is then reported through the scaling, which puts it under the
  //! threshold that would correct it, and there it stays. Measured after a
  //! resume from stopped: 105.84ms left standing, reported as 47.6ms against a
  //! 50ms threshold.
  bool PeekSyncAcquisition() const { return m_syncErrorAcquire; }

  //! \brief Spend it. Callers must only reach here once the clock has actually
  //! moved: the clock is entitled to decline - it applies nothing at all between
  //! -27ms and +20ms while the renderer is centring frames, and quantises to
  //! whole frames above that - and a chance spent on a refusal is a chance not
  //! taken.
  void TakeSyncAcquisition() { m_syncErrorAcquire = false; }

  //! \brief Whether the reading hit the engine's own ceiling. A saturated value
  //! is a lower bound on the error, not a measurement of it, so nothing should
  //! be corrected by it - a seek can make the true figure seconds wide.
  bool IsSyncErrorSaturated() const { return m_syncErrorSaturated; }

  void SetSyncErrorCorrection(double correction);

  /*!
   * \brief Returns the resample ratio, or 0.0 if unknown/invalid
   */
  double GetResampleRatio();

  void SetResampleMode(int mode);
  void Flush();
  void Drain();
  void AbortAddPackets();

  double GetClock() override;
  double GetClockSpeed() override;

  CAEStreamInfo::DataType GetPassthroughStreamType(AVCodecID codecId, int samplerate, int profile);

protected:
  IAE::StreamPtr m_pAudioStream;
  double m_playingPts;
  double m_timeOfPts;
  double m_syncError;
  double m_syncErrorRaw{0.0};
  bool m_syncErrorRawValid{false};
  double m_syncErrorScale{1.0}; //!< what the engine scaled the reported error by
  bool m_syncErrorSaturated{false}; //!< the reading hit the engine's ceiling
  bool m_syncErrorAcquire{false};   //!< a correction is owed from the last resync
  bool m_syncWasInSync{false};      //!< the engine's state at the previous packet
  unsigned int m_syncErrorTime;
  double m_resampleRatio = 0.0; // invalid
  CCriticalSection m_critSection;

  AEDataFormat m_dataFormat;
  unsigned int m_sampleRate;
  int m_iBitsPerSample;
  bool m_bPassthrough;
  CAEChannelInfo m_channelLayout;
  CAEStreamInfo::DataType m_dataType;
  bool m_bPaused;

  std::atomic_bool m_bAbort;
  CDVDClock *m_pClock;
};
