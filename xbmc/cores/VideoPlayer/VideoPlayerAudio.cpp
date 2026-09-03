/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayerAudio.h"

#include "DVDCodecs/Audio/DVDAudioCodec.h"
#include "DVDCodecs/DVDFactoryCodec.h"
#include "ServiceBroker.h"
#include "cores/AudioEngine/Interfaces/AE.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/MathUtils.h"
#include "utils/log.h"

#include <mutex>

#ifdef TARGET_RASPBERRY_PI
#include "platform/linux/RBP.h"
#endif

#include <sstream>
#include <iomanip>
#include <math.h>

using namespace std::chrono_literals;

namespace
{
//! Past this a reading is not a resync residue but something that has gone wrong
//! - a stale timestamp, a seek the decoder has not caught up with. It is above
//! the ordinary threshold anyway, so the correction still happens; what this
//! saves is the one chance, for a reading worth spending it on.
constexpr double ACQUIRE_LIMIT = DVD_MSEC_TO_TIME(500.0);

//! Below this a correction is not worth a clock step, and more to the point it is
//! below the rate loop's own jump guard - so it would land inside a drift bucket
//! and be read as a rate rather than discarded as a step. Kept in step with that
//! guard deliberately.
constexpr double ACQUIRE_FLOOR = DVD_MSEC_TO_TIME(5.0);

//! Readings allowed to arrive before an acquisition that has found nothing worth
//! correcting lapses. Held indefinitely it would be spent much later on ordinary
//! jitter, which is a clock step under a running picture - the thing the
//! threshold exists to avoid.
//!
//! Counted as well as timed. The engine widens its own averaging window when a
//! stream's timestamps jump - to six seconds, tripled again when it is resampling
//! hard - so a wall clock alone could lapse the chance before the readings it is
//! waiting for had been published at all. But readings alone are not a bound
//! either: readings come seconds apart, so eight of them is a wait, and a correction
//! that arrives minutes in is a clock step under a picture nobody is expecting
//! one under. Whichever comes first.
constexpr unsigned int ACQUIRE_READINGS = 8;
constexpr double ACQUIRE_EXPIRY = 30.0 * DVD_TIME_BASE;

//! Three readings must agree within this before the one chance is spent. The
//! engine reports a mean over the last second, so a reading taken while the
//! pipeline is still moving is a blend of where the audio was and where it now
//! is - and the step sized from it lands short by the difference, permanently,
//! because what is left is inside every threshold below it.
//!
//! Measured: a display mode change re-opens the audio sink a few hundred
//! milliseconds before the acquisition falls due. The error plateaus at +153ms
//! while the mean still reads +90ms; the clock moves 90 and 63 stands for the
//! rest of the film. With no re-open the same code lands within a tenth of a
//! millisecond, which is what says the sizing is right and only the timing is
//! wrong.
//! Two milliseconds, an order above the settled noise: consecutive windows agree
//! to a fifth of a millisecond once the pipeline is still. Ten was fifty times
//! that, wide enough to pass a ramp still forty milliseconds from where it was
//! going.
//!
//! Or a twentieth of the reading, whichever is larger. A mean over a jittery
//! source - a variable bitrate, a network, the messy timestamps that widened the
//! window in the first place - moves by more than two milliseconds between
//! windows while sitting perfectly still, and a fixed figure would refuse every
//! reading on exactly the content most likely to need correcting. The ramp this
//! has to reject moved sixty-three milliseconds in one window, so a twentieth
//! turns it away with room to spare.
constexpr double STATIONARY = DVD_MSEC_TO_TIME(2.0);
constexpr double STATIONARY_FRACTION = 0.05;
} // unnamed namespace

class CDVDMsgAudioCodecChange : public CDVDMsg
{
public:
  CDVDMsgAudioCodecChange(const CDVDStreamInfo& hints, std::unique_ptr<CDVDAudioCodec> codec)
    : CDVDMsg(GENERAL_STREAMCHANGE), m_codec(std::move(codec)), m_hints(hints)
  {}
  ~CDVDMsgAudioCodecChange() override = default;

  std::unique_ptr<CDVDAudioCodec> m_codec;
  CDVDStreamInfo  m_hints;
};

CVideoPlayerAudio::CVideoPlayerAudio(CDVDClock* pClock,
                                     CDVDMessageQueue& parent,
                                     CProcessInfo& processInfo,
                                     double messageQueueTimeSize)
  : CThread("VideoPlayerAudio"),
    IDVDStreamPlayerAudio(processInfo),
    m_messageQueue("audio"),
    m_messageParent(parent),
    m_audioSink(pClock)
{
  m_pClock = pClock;
  m_audioClock = 0;
  m_speed = DVD_PLAYSPEED_NORMAL;
  m_stalled = true;
  m_paused = false;
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
  m_synctype = SYNC_DISCON;
  m_prevsynctype = -1;
  m_prevskipped = false;
  m_maxspeedadjust = 0.0;

  // queue data size is dynamical changed by stream
  m_messageQueue.SetMaxDataSize(LvLAudioMIN);
  m_messageQueue.SetMaxTimeSize(messageQueueTimeSize);

  m_disconAdjustTimeMs = processInfo.GetMaxPassthroughOffSyncDuration();
}

CVideoPlayerAudio::~CVideoPlayerAudio()
{
  StopThread();

  // close the stream, and don't wait for the audio to be finished
  // CloseStream(true);
}

bool CVideoPlayerAudio::OpenStream(CDVDStreamInfo hints)
{
  CLog::Log(LOGINFO, "Finding audio codec for: {}", hints.codec);
  bool allowpassthrough = !CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK);

  CAEStreamInfo::DataType streamType =
      m_audioSink.GetPassthroughStreamType(hints.codec, hints.samplerate, hints.profile);
  std::unique_ptr<CDVDAudioCodec> codec = CDVDFactoryCodec::CreateAudioCodec(
      hints, m_processInfo, allowpassthrough, m_processInfo.AllowDTSHDDecode(), streamType);
  if(!codec)
  {
    CLog::Log(LOGERROR, "Unsupported audio codec");
    return false;
  }

  if(m_messageQueue.IsInited())
    m_messageQueue.Put(std::make_shared<CDVDMsgAudioCodecChange>(hints, std::move(codec)), 0);
  else
  {
    OpenStream(hints, std::move(codec));
    m_messageQueue.Init();
    CLog::Log(LOGINFO, "Creating audio thread");
    Create();
  }
  return true;
}

void CVideoPlayerAudio::OpenStream(CDVDStreamInfo& hints, std::unique_ptr<CDVDAudioCodec> codec)
{
  m_pAudioCodec = std::move(codec);


  /* store our stream hints */
  m_streaminfo = hints;

  /* update codec information from what codec gave out, if any */
  int channelsFromCodec   = m_pAudioCodec->GetFormat().m_channelLayout.Count();
  int samplerateFromCodec = m_pAudioCodec->GetFormat().m_sampleRate;

  if (channelsFromCodec > 0)
    m_streaminfo.channels = channelsFromCodec;
  if (samplerateFromCodec > 0)
    m_streaminfo.samplerate = samplerateFromCodec;

  /* check if we only just got sample rate, in which case the previous call
   * to CreateAudioCodec() couldn't have started passthrough */
  if (hints.samplerate != m_streaminfo.samplerate)
    SwitchCodecIfNeeded();

  m_audioClock = 0;
  m_stalled = m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET) == 0;

  m_prevsynctype = -1;
  m_synctype = SYNC_DISCON;
  if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK))
    m_synctype = SYNC_RESAMPLE;

  if (m_synctype == SYNC_DISCON)
    CLog::LogF(LOGINFO, "Allowing max Out-Of-Sync Value of {} ms", m_disconAdjustTimeMs);

  m_prevskipped = false;

  m_maxspeedadjust = 5.0;

  m_messageParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_AVCHANGE));
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
}

void CVideoPlayerAudio::CloseStream(bool bWaitForBuffers)
{
  bool bWait = bWaitForBuffers && m_speed > 0 && !CServiceBroker::GetActiveAE()->IsSuspended();

  // wait until buffers are empty
  if (bWait)
    m_messageQueue.WaitUntilEmpty();

  // send abort message to the audio queue
  m_messageQueue.Abort();

  CLog::Log(LOGINFO, "Waiting for audio thread to exit");

  // shut down the adio_decode thread and wait for it
  StopThread(); // will set this->m_bStop to true

  // After the thread that publishes it has been joined, not before: the drain
  // above runs OutputPacket, which would put a live value back.
  m_processInfo.SetAudioSyncError(DVD_NOPTS_VALUE);

  // destroy audio device
  CLog::Log(LOGINFO, "Closing audio device");
  if (bWait)
  {
    m_bStop = false;
    m_audioSink.Drain();
    m_bStop = true;
  }
  else
  {
    m_audioSink.Flush();
  }

  m_audioSink.Destroy(true);

  // uninit queue
  m_messageQueue.End();

  CLog::Log(LOGINFO, "Deleting audio codec");
  if (m_pAudioCodec)
  {
    m_pAudioCodec->Dispose();
    m_pAudioCodec.reset();
  }

  std::ostringstream s;
  SInfo info;
  info.info        = s.str();
  info.pts         = DVD_NOPTS_VALUE;
  info.passthrough = false;

  { std::unique_lock<CCriticalSection> lock(m_info_section);
    m_info = info;
  }
}

void CVideoPlayerAudio::OnStartup()
{
}

void CVideoPlayerAudio::UpdatePlayerInfo()
{
  std::ostringstream s;
  s << "aq:" << std::setw(2) << std::min(99, m_messageQueue.GetLevel()) << "% ("
    << std::setw(2) << std::min(99,m_messageQueue.GetLevel(true)) << "%, "
    << std::fixed << std::setprecision(1) << static_cast<double>(m_messageQueue.GetMaxDataSize()) / SIZE_1M << "MB)";
  s << std::fixed << std::setprecision(3) << m_messageQueue.GetTimeSize();
  s << "s, Kb/s:" << std::fixed << std::setprecision(2) << m_audioStats.GetBitrate() / 1024.0;
  s << ", ac:"   << m_processInfo.GetAudioDecoderName().c_str();
  if (!m_info.passthrough)
    s << ", chan:" << m_processInfo.GetAudioChannels().c_str();
  s << ", " << m_streaminfo.samplerate/1000 << " kHz";

  // print a/v discontinuity adjustments counter when audio is not resampled (passthrough mode)
  if (m_synctype == SYNC_DISCON)
    s << ", a/v corrections (" << m_disconAdjustTimeMs << "ms): " << m_disconAdjustCounter;

  //print the inverse of the resample ratio, since that makes more sense
  //if the resample ratio is 0.5, then we're playing twice as fast
  else if (m_synctype == SYNC_RESAMPLE)
    s << ", rr:" << std::fixed << std::setprecision(5) << 1.0 / m_audioSink.GetResampleRatio();

  SInfo info;
  info.info        = s.str();
  info.pts         = m_audioSink.GetPlayingPts();
  info.passthrough = m_pAudioCodec && m_pAudioCodec->NeedPassthrough();

  {
    std::unique_lock lock(m_info_section);
    m_info = info;
  }

  m_processInfo.SetAudioLiveBitRate(m_audioStats.GetBitrate());
  m_processInfo.SetAudioQueueLevel(std::min(99, m_messageQueue.GetLevel()));
  m_processInfo.SetAudioQueueDataLevel(std::min(99, m_messageQueue.GetLevel(true)));
}

void CVideoPlayerAudio::Process()
{
  CLog::Log(LOGINFO, "running thread: CVideoPlayerAudio::Process()");

  DVDAudioFrame audioframe;
  audioframe.nb_frames = 0;
  audioframe.framesOut = 0;
  m_audioStats.Start();
  m_disconAdjustCounter = 0;

  bool onlyPrioMsgs = false;

  while (!m_bStop)
  {
    std::shared_ptr<CDVDMsg> pMsg;
    auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double, std::ratio<1>>(m_audioSink.GetCacheTime()));

    // read next packet and return -1 on error
    int priority = 1;
    //Do we want a new audio frame?
    if (m_syncState == IDVDStreamPlayer::SYNC_STARTING ||              /* when not started */
        m_processInfo.IsTempoAllowed(static_cast<float>(m_speed)/DVD_PLAYSPEED_NORMAL) ||
        m_speed <  DVD_PLAYSPEED_PAUSE  || /* when rewinding */
        (m_speed >  DVD_PLAYSPEED_NORMAL && m_audioClock < m_pClock->GetClock())) /* when behind clock in ff */
      priority = 0;

    if (m_syncState == IDVDStreamPlayer::SYNC_WAITSYNC)
      priority = 1;

    if (m_paused)
      priority = 1;

    if (onlyPrioMsgs)
    {
      priority = 1;
      timeout = 0ms;
    }

    MsgQueueReturnCode ret = m_messageQueue.Get(pMsg, timeout, priority);

    onlyPrioMsgs = false;

    if (MSGQ_IS_ERROR(ret))
    {
      if (!m_messageQueue.ReceivedAbortRequest())
        CLog::Log(LOGERROR, "MSGQ_IS_ERROR returned true ({})", ret);

      break;
    }
    else if (ret == MSGQ_TIMEOUT)
    {
      if (ProcessDecoderOutput(audioframe))
      {
        onlyPrioMsgs = true;
        continue;
      }

      // if we only wanted priority messages, this isn't a stall
      if (priority)
        continue;

      if (m_processInfo.IsTempoAllowed(static_cast<float>(m_speed)/DVD_PLAYSPEED_NORMAL) &&
          !m_stalled && m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
      {
        // while AE sync is active, we still have time to fill buffers
        if (m_syncTimer.IsTimePast())
        {
          CLog::Log(LOGINFO, "CVideoPlayerAudio::Process - stream stalled");
          m_stalled = true;
    // Nothing will publish again until packets flow, and the reporter
    // samples every frame: left standing, one stale reading is counted
    // once per frame for as long as the audio is gone.
    m_processInfo.SetAudioSyncError(DVD_NOPTS_VALUE);
        }
      }
      if (timeout == 0ms)
        CThread::Sleep(10ms);

      continue;
    }

    // handle messages
    if (pMsg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      if (std::static_pointer_cast<CDVDMsgGeneralSynchronize>(pMsg)->Wait(100ms, SYNCSOURCE_AUDIO))
        CLog::Log(LOGDEBUG, "CVideoPlayerAudio - CDVDMsg::GENERAL_SYNCHRONIZE");
      else
        m_messageQueue.Put(pMsg, 1); // push back as prio message, to process other prio messages
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESYNC))
    { //player asked us to set internal clock
      double pts = std::static_pointer_cast<CDVDMsgDouble>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio - CDVDMsg::GENERAL_RESYNC({:.3f} level: {:d} cache:{:.3f}",
                pts / DVD_TIME_BASE, m_messageQueue.GetLevel(), m_audioSink.GetDelay() / DVD_TIME_BASE);

      double delay = m_audioSink.GetDelay();
      if (pts > m_audioClock - delay + 0.5 * DVD_TIME_BASE)
      {
        m_audioSink.Flush();
      }
      m_audioClock = pts + delay;
      if (m_speed != DVD_PLAYSPEED_PAUSE)
        m_audioSink.Resume();
      m_syncState = IDVDStreamPlayer::SYNC_INSYNC;
      m_syncTimer.Set(3000ms);
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESET))
    {
      if (m_pAudioCodec)
        m_pAudioCodec->Reset();
      m_audioSink.Flush();
      m_stalled = true;
      m_audioClock = 0;
      audioframe.nb_frames = 0;
      m_syncState = IDVDStreamPlayer::SYNC_STARTING;
      // Nothing will publish again until packets flow, and the reporter
      // samples every frame: left standing, one stale reading is counted
      // once per frame for as long as the audio is gone.
      m_processInfo.SetAudioSyncError(DVD_NOPTS_VALUE);
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH))
    {
      bool sync = std::static_pointer_cast<CDVDMsgBool>(pMsg)->m_value;
      m_audioSink.Flush();
      m_stalled = true;
      m_audioClock = 0;
      audioframe.nb_frames = 0;
      // Nothing will publish again until packets flow, and the reporter
      // samples every frame: left standing, one stale reading is counted
      // once per frame for as long as the audio is gone.
      m_processInfo.SetAudioSyncError(DVD_NOPTS_VALUE);

      if (sync)
      {
        m_syncState = IDVDStreamPlayer::SYNC_STARTING;
        m_audioSink.Pause();
      }

      if (m_pAudioCodec)
        m_pAudioCodec->Reset();
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_EOF))
    {
      CLog::Log(LOGDEBUG, "CVideoPlayerAudio - CDVDMsg::GENERAL_EOF");
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SETSPEED))
    {
      double speed = std::static_pointer_cast<CDVDMsgInt>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio - CDVDMsg::PLAYER_SETSPEED: {:f} last: {:d}", speed, m_speed);

      if (m_processInfo.IsTempoAllowed(static_cast<float>(speed)/DVD_PLAYSPEED_NORMAL))
      {
        if (speed != m_speed)
        {
          if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
          {
            m_audioSink.Resume();
            m_stalled = false;
          }
        }
      }
      else
      {
        m_audioSink.Pause();
      }
      m_speed = (int)speed;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_STREAMCHANGE))
    {
      auto msg = std::static_pointer_cast<CDVDMsgAudioCodecChange>(pMsg);
      OpenStream(msg->m_hints, std::move(msg->m_codec));
      msg->m_codec = NULL;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_PAUSE))
    {
      m_paused = std::static_pointer_cast<CDVDMsgBool>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, "CVideoPlayerAudio - CDVDMsg::GENERAL_PAUSE: {}", m_paused);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_REQUEST_STATE))
    {
      SStateMsg msg;
      msg.player = VideoPlayer_AUDIO;
      msg.syncState = m_syncState;
      m_messageParent.Put(
          std::make_shared<CDVDMsgType<SStateMsg>>(CDVDMsg::PLAYER_REPORT_STATE, msg));
    }
    else if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* pPacket = std::static_pointer_cast<CDVDMsgDemuxerPacket>(pMsg)->GetPacket();
      bool bPacketDrop = std::static_pointer_cast<CDVDMsgDemuxerPacket>(pMsg)->GetPacketDrop();

      if (bPacketDrop)
      {
        if (m_syncState != IDVDStreamPlayer::SYNC_STARTING)
        {
          m_audioSink.Drain();
          m_audioSink.Flush();
          audioframe.nb_frames = 0;
        }
        m_syncState = IDVDStreamPlayer::SYNC_STARTING;
        continue;
      }

      if (!m_processInfo.IsTempoAllowed(static_cast<float>(m_speed) / DVD_PLAYSPEED_NORMAL) &&
          m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
      {
        continue;
      }

      if (!m_pAudioCodec->AddData(*pPacket))
      {
        m_messageQueue.PutBack(pMsg);
        onlyPrioMsgs = true;
        continue;
      }

      m_audioStats.AddSampleBytes(pPacket->iSize);
      UpdatePlayerInfo();

      if (ProcessDecoderOutput(audioframe))
      {
        onlyPrioMsgs = true;
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_DISPLAY_RESET))
    {
      m_displayReset = true;
    }
  }
}

bool CVideoPlayerAudio::ProcessDecoderOutput(DVDAudioFrame &audioframe)
{
  if (audioframe.nb_frames <= audioframe.framesOut)
  {
    audioframe.hasDownmix = false;

    m_pAudioCodec->GetData(audioframe);

    if (audioframe.nb_frames == 0)
    {
      return false;
    }

    audioframe.hasTimestamp = true;
    if (audioframe.pts == DVD_NOPTS_VALUE)
    {
      audioframe.pts = m_audioClock;
      audioframe.hasTimestamp = false;
    }
    else
    {
      m_audioClock = audioframe.pts;
    }

    if (audioframe.format.m_sampleRate && m_streaminfo.samplerate != (int) audioframe.format.m_sampleRate)
    {
      // The sample rate has changed or we just got it for the first time
      // for this stream. See if we should enable/disable passthrough due
      // to it.
      m_streaminfo.samplerate = audioframe.format.m_sampleRate;
      if (SwitchCodecIfNeeded())
      {
        audioframe.nb_frames = 0;
        return false;
      }
    }

    // Display reset event has occurred
    // See if we should enable passthrough
    if (m_displayReset)
    {
      if (SwitchCodecIfNeeded())
      {
        audioframe.nb_frames = 0;
        return false;
      }
    }

    // demuxer reads metatags that influence channel layout
    if (m_streaminfo.codec == AV_CODEC_ID_FLAC && m_streaminfo.channellayout)
      audioframe.format.m_channelLayout = CAEUtil::GetAEChannelLayout(m_streaminfo.channellayout);

    // we have successfully decoded an audio frame, setup renderer to match
    if (!m_audioSink.IsValidFormat(audioframe))
    {
      if (m_speed)
        m_audioSink.Drain();

      m_audioSink.Destroy(false);

      if (!m_audioSink.Create(audioframe, m_streaminfo.codec, m_synctype == SYNC_RESAMPLE))
        CLog::Log(LOGERROR, "{} - failed to create audio renderer", __FUNCTION__);

      m_prevsynctype = -1;

      if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
        m_audioSink.Resume();
    }

    m_audioSink.SetDynamicRangeCompression(
        static_cast<long>(m_processInfo.GetVideoSettings().m_VolumeAmplification * 100));

    SetSyncType(audioframe.passthrough);

    // downmix
    double clev = audioframe.hasDownmix ? audioframe.centerMixLevel : M_SQRT1_2;
    double curDB = 20 * log10(clev);
    audioframe.centerMixLevel = pow(10, (curDB + m_processInfo.GetVideoSettings().m_CenterMixLevel) / 20);
    audioframe.hasDownmix = true;
  }

  // Published for every sync type - the engine measures the error in resample
  // mode too, it is just corrected differently. It is read back and acted on.
  // The vsync adjust goes back on because the engine measured against a clock
  // that already had it (CAudioSinkAE::GetClock), and a reader comparing this
  // with the picture needs both against the same one.
  // A saturated reading is withheld as well as refused: it is the clamp, not a
  // measurement, and a reader that acted on it would be acting on the same
  // figure the correction above declines to use.
  m_processInfo.SetAudioSyncError(
      (m_audioSink.HasSyncError() && !m_audioSink.IsSyncErrorSaturated())
          ? m_audioSink.GetSyncErrorRaw() + m_pClock->GetVsyncAdjust()
          : DVD_NOPTS_VALUE);

  if (m_synctype == SYNC_DISCON)
  {
    double syncerror = m_audioSink.GetSyncError();

    // A saturated reading is a lower bound on the error, not a measurement of
    // it, and both copies are clamped - so there is nothing here worth acting
    // on in either domain. Waiting costs one interval; stepping the clock by a
    // clamp costs whatever the clamp happens to be.
    if (m_audioSink.HasSyncError() && m_audioSink.IsSyncErrorSaturated())
      syncerror = 0.0;

    // Once after each resync, the threshold is not consulted. It exists to stop
    // a settled stream being corrected for jitter, which is right; but what the
    // resync leaves behind is not jitter, it is a standing offset, and one that
    // is by construction too small to cross the threshold and too large to
    // ignore. Correcting it once, here, is the difference between a title that
    // starts where the last one did and a title that starts wherever the last
    // frame boundary happened to fall.

    // The clock tracks the flag on every path. The sink clears the flag in four
    // places - a flush, a stream teardown, the engine leaving INSYNC, and the
    // spend - and a timestamp left behind by any of them would be what the next
    // acquisition got measured against, which is to say it would arrive already
    // expired.
    if (!m_audioSink.PeekSyncAcquisition())
    {
      m_acquireSince = 0.0;

      // The pair below has to belong to the acquisition it guards. Nothing else
      // clears it, and the engine stops reporting an error at all across a
      // resync - so a reading kept from before a pause would be compared with
      // the first one after it, which is not two consecutive readings of
      // anything, and the gate would open on a reading it never checked.
      m_haveRawPrev = false;
      m_rawPrev = 0.0;
      m_rawSettled = false;
      m_rawSettledPrev = false;
      m_rawReadings = 0;
    }
    else if (m_acquireSince == 0.0)
      m_acquireSince = m_pClock->GetAbsoluteClock();
    else if (m_rawReadings >= ACQUIRE_READINGS ||
             m_pClock->GetAbsoluteClock() - m_acquireSince > ACQUIRE_EXPIRY)
    {
      m_audioSink.TakeSyncAcquisition();
      m_acquireSince = 0.0;
    }

    // On the figure that will actually be applied, not the reported one: the
    // floor has to mean the same thing as the step. Not while the vsync adjust
    // is running, where the clock quantises to whole frames - a frame is not the
    // residue this is looking for, and spending the one chance to move by one
    // would be worse than leaving the residue alone. The genlock stands down in
    // that regime too, for its own reasons.
    const double errorRaw = m_audioSink.HasSyncError() ? m_audioSink.GetSyncErrorRaw() : 0.0;

    // Only the changes count: the engine republishes about once a second, so
    // consecutive packets mostly carry the same figure and comparing those would
    // call anything stationary.
    if (m_audioSink.HasSyncError() && errorRaw != m_rawPrev)
    {
      // Three in a row, not two: a ramp gentle enough to move less than the
      // threshold between one reading and the next would otherwise pass while
      // still well short of where it is going, and the chance would be spent
      // short - which is the whole failure this exists to prevent, just at a
      // shallower slope.
      const double allowed = std::max(STATIONARY, STATIONARY_FRACTION * std::abs(errorRaw));
      const bool pairAgrees = m_haveRawPrev && std::abs(errorRaw - m_rawPrev) < allowed;
      m_rawSettled = pairAgrees && m_rawSettledPrev;
      m_rawSettledPrev = pairAgrees;
      m_rawPrev = errorRaw;
      m_haveRawPrev = true;
      ++m_rawReadings;
    }
    // Only where the argument for it holds. The residue this exists to remove is
    // one the engine cannot remove itself: in passthrough its finest move is a
    // whole IEC frame, twenty milliseconds, so it stops inside its own band and
    // what is left is stranded. Carrying samples it has no such floor - it can
    // move the audio by as little as it likes - so there is no stranded residue
    // to go after, and correcting outside the threshold there would be a clock
    // step for something the engine was going to take out anyway.
    const bool acquire = m_audioSink.IsPassthrough() && m_audioSink.PeekSyncAcquisition() &&
                         m_audioSink.HasSyncError() &&
                         !m_audioSink.IsSyncErrorSaturated() && m_pClock->GetVsyncAdjust() == 0.0 &&
                         m_pClock->GetSpeedAdjust() == 0.0 &&
                         m_rawSettled && std::abs(errorRaw) > ACQUIRE_FLOOR &&
                         std::abs(errorRaw) < ACQUIRE_LIMIT;

    if (acquire || std::abs(syncerror) > DVD_MSEC_TO_TIME(m_disconAdjustTimeMs))
    {
      // Decided on the error the engine reports, but corrected by the one it
      // measured. For TrueHD passthrough those are not the same number: the
      // engine shrinks what it reports so a stream whose frames arrive unevenly
      // does not provoke a correction every second, and that is a reasonable
      // thing to do to a threshold. Using the same shrunken figure as the size
      // of the step is not - the clock then moves a fraction of the way and
      // stops, and what is left is by construction too small to try again.
      // Measured at a title start: a 115ms offset answered with a 51.58ms step,
      // leaving 63ms in place for the rest of the film.
      // Never by a saturated reading. The engine clamps at 1s in sync and 5s out
      // of it, and a seek that leaves the decoder reporting a stale timestamp
      // makes the true error seconds wide - stepping the clock by the clamp
      // would move it by that much on a number that is only a lower bound.
      // The fallback is the reported error, which the guard above has zeroed.
      const double actual = (m_audioSink.HasSyncError() &&
                             !m_audioSink.IsSyncErrorSaturated())
                                ? m_audioSink.GetSyncErrorRaw()
                                : syncerror;
      double correction = m_pClock->ErrorAdjust(actual, "CVideoPlayerAudio::OutputPacket");
      if (correction != 0)
      {
        // Spent once the clock has moved, whichever path moved it. The residue
        // it was held for is gone either way, and an acquisition left armed
        // behind a correction is one that gets spent a second later on ordinary
        // jitter - a clock step under a running picture, which is the thing the
        // threshold is there to prevent.
        m_audioSink.TakeSyncAcquisition();
        m_acquireSince = 0.0;
        if (acquire)
          CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio:: acquisition {:.3f}",
                    correction / DVD_TIME_BASE);
        m_audioSink.SetSyncErrorCorrection(-correction);
        m_disconAdjustCounter++;
        CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio:: sync error correctiom:{:.3f}", correction / DVD_TIME_BASE);
      }
    }
  }
  CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio::OutputPacket: pts:{:.3f} curr_pts:{:.3f} clock:{:.3f} level:{:d}",
    audioframe.pts / DVD_TIME_BASE, m_info.pts / DVD_TIME_BASE, m_pClock->GetClock() / DVD_TIME_BASE, GetLevel());

  int framesOutput = m_audioSink.AddPackets(audioframe);

  // guess next pts
  m_audioClock += audioframe.duration * ((double)framesOutput / audioframe.nb_frames);

  audioframe.framesOut += framesOutput;

  // signal to our parent that we have initialized
  if (m_syncState == IDVDStreamPlayer::SYNC_STARTING)
  {
    double cachetotal = m_audioSink.GetCacheTotal();
    double cachetime = m_audioSink.GetCacheTime();
    if (cachetime >= cachetotal * 0.75)
    {
      m_syncState = IDVDStreamPlayer::SYNC_WAITSYNC;
      m_stalled = false;
      SStartMsg msg;
      msg.player = VideoPlayer_AUDIO;
      msg.cachetotal = m_audioSink.GetMaxDelay() * DVD_TIME_BASE;
      msg.cachetime = m_audioSink.GetDelay();
      msg.timestamp = audioframe.hasTimestamp ? audioframe.pts : DVD_NOPTS_VALUE;
      m_messageParent.Put(std::make_shared<CDVDMsgType<SStartMsg>>(CDVDMsg::PLAYER_STARTED, msg));

      m_streaminfo.channels = audioframe.format.m_channelLayout.Count();
      m_processInfo.SetAudioChannels(audioframe.format.m_channelLayout);
      m_processInfo.SetAudioSampleRate(audioframe.format.m_sampleRate);
      m_processInfo.SetAudioBitsPerSample(audioframe.bits_per_sample);
      m_processInfo.SetAudioDecoderName(m_pAudioCodec->GetName());
      m_messageParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_AVCHANGE));
    }
  }

  return true;
}

void CVideoPlayerAudio::SetSyncType(bool passthrough)
{
  if (passthrough && m_synctype == SYNC_RESAMPLE)
    m_synctype = SYNC_DISCON;

  //if SetMaxSpeedAdjust returns false, it means no video is played and we need to use clock feedback
  double maxspeedadjust = 0.0;
  if (m_synctype == SYNC_RESAMPLE)
    maxspeedadjust = m_maxspeedadjust;

  m_pClock->SetMaxSpeedAdjust(maxspeedadjust);

  if (m_synctype != m_prevsynctype)
  {
    const char *synctypes[] = {"clock feedback", "resample", "invalid"};
    int synctype = (m_synctype >= 0 && m_synctype <= 1) ? m_synctype : 2;
    CLog::Log(LOGDEBUG, "CVideoPlayerAudio:: synctype set to {}: {}", m_synctype,
              synctypes[synctype]);
    m_prevsynctype = m_synctype;
    if (m_synctype == SYNC_RESAMPLE)
      m_audioSink.SetResampleMode(1);
    else
      m_audioSink.SetResampleMode(0);
  }
}

void CVideoPlayerAudio::OnExit()
{
#ifdef TARGET_WINDOWS
  CoUninitialize();
#endif

  CLog::Log(LOGINFO, "thread end: CVideoPlayerAudio::OnExit()");
}

void CVideoPlayerAudio::SetSpeed(int speed)
{
  if(m_messageQueue.IsInited())
    m_messageQueue.Put(std::make_shared<CDVDMsgInt>(CDVDMsg::PLAYER_SETSPEED, speed), 1);
  else
    m_speed = speed;
}

void CVideoPlayerAudio::Flush(bool sync)
{
  m_messageQueue.Flush();
  m_messageQueue.Put(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_FLUSH, sync), 1);

  m_audioSink.AbortAddPackets();
}

bool CVideoPlayerAudio::AcceptsData() const
{
  bool full = m_messageQueue.IsFull();
  return !full;
}

bool CVideoPlayerAudio::SwitchCodecIfNeeded()
{
  if (m_displayReset)
    CLog::Log(LOGINFO, "CVideoPlayerAudio: display reset occurred, checking for passthrough");
  else
    CLog::Log(LOGDEBUG, "CVideoPlayerAudio: stream props changed, checking for passthrough");

  m_displayReset = false;

  bool allowpassthrough = !CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK);
  if (m_synctype == SYNC_RESAMPLE)
    allowpassthrough = false;

  CAEStreamInfo::DataType streamType = m_audioSink.GetPassthroughStreamType(
      m_streaminfo.codec, m_streaminfo.samplerate, m_streaminfo.profile);
  std::unique_ptr<CDVDAudioCodec> codec = CDVDFactoryCodec::CreateAudioCodec(
      m_streaminfo, m_processInfo, allowpassthrough, m_processInfo.AllowDTSHDDecode(), streamType);

  if (!codec || codec->NeedPassthrough() == m_pAudioCodec->NeedPassthrough())
  {
    // passthrough state has not changed
    return false;
  }

  m_pAudioCodec = std::move(codec);

  return true;
}

std::string CVideoPlayerAudio::GetPlayerInfo()
{
  std::unique_lock lock(m_info_section);
  return m_info.info;
}

int CVideoPlayerAudio::GetAudioChannels()
{
  return m_streaminfo.channels;
}

bool CVideoPlayerAudio::IsPassthrough() const
{
  std::unique_lock lock(m_info_section);
  return m_info.passthrough;
}
