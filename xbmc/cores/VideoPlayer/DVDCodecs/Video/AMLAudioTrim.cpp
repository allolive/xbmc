/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AMLAudioTrim.h"

#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "platform/linux/SysfsPath.h"
#include "utils/AMLUtils.h"
#include "ServiceBroker.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
constexpr const char* ENABLE_PATH = "/storage/.kodi/userdata/aml-audio-trim";
constexpr const char* TRIM_PATH =
    "/sys/module/amlogic_clk_soc_g12a/parameters/audio_sdm_trim";

//! The hardware ignores the field's low bit and rounds up, so a step of one
//! lands on the same divider half the time. Two always moves.
constexpr int STEP = 2;

//! What the level may reach while the loop is only nulling the rate. Sixty parts
//! per million is already an order more than the drift measured here.
constexpr double MAX_LEVEL_RATE = 2.0;

//! What it may reach while it is also walking an offset out. Bounded by the
//! kernel, which refuses more than forty.
constexpr double MAX_LEVEL_HW = 20.0;

//! Where the offset loop is switched on, and how hard. The file holds the
//! largest rate error it may ask for, in parts per million; absent, the loop
//! only nulls the rate, which is what it did before this existed.
//!
//! Runtime rather than built in because the number is not ours to choose: it is
//! however much the receiver will follow without dropping lock, and that is a
//! property of the receiver. It has to be found by ear once, on the box.

//! Sanity bounds on what that file may say.
constexpr double SLEW_MIN = 5e-6;
constexpr double SLEW_MAX = 600e-6;

//! How long the offset is given to decay. With the level handed straight to the
//! feedforward the rate is in place within a few ticks, so this really is the
//! decay the offset follows rather than an upper bound the inner loop spoils.
//! Short enough that a title's offset is gone before anyone settles in.
constexpr double OFFSET_TAU = 25.0 * DVD_TIME_BASE;

//! Below this the offset is not worth a rate error. It has to sit well under
//! whatever the loop actually settles at, not beside it: measured converging to
//! 0.46ms against a band of 0.5, the aim switched on and off as the figure
//! crossed it four times a second and the register alternated with it. A band is
//! meant to be the point the loop stops caring, so it belongs below the noise it
//! is ignoring rather than in the middle of it.
constexpr double OFFSET_DEADBAND = DVD_MSEC_TO_TIME(0.15);

//! How long the error is averaged before a slope is taken from it. A minute of
//! means differ by ~0.02ms while a minute of drift is ~1.2ms, so fifteen seconds
//! still leaves the slope several times its own noise.
constexpr double BUCKET = 15.0 * DVD_TIME_BASE;

//! Wanted for a bucket to count. Short of this the mean is of too few frames to
//! difference against the last one.
constexpr unsigned int BUCKET_MIN_SAMPLES = 200;

//! Samples land once per presented frame, so anything approaching a second is a
//! gap rather than a cadence - a pause, a buffering hold, a source that stopped
//! answering. Well clear of a frame period at any rate this runs at.
constexpr double SAMPLE_GAP = 1.0 * DVD_TIME_BASE;

//! What one step is assumed to be worth, as a rate. Measured at 30ppm on g12b at
//! 192kHz, but only the sign and the order matter: this is the gain of an
//! integrator, so being wrong by a factor slows convergence rather than moving
//! where it converges to.
constexpr double STEP_RATE = 30e-6;

//! Fraction of the error taken out per decision.
//!
//! The median does not multiply the gain - a sliding median of three has unity
//! DC gain, since a sample is the middle one once on average - but it does delay
//! it by about a sample, and that lowers the ceiling this can sit under rather
//! than raising the number underneath it. Measured on this box the plant is 29
//! parts per million to a step against the 30 assumed here, so there is no
//! hidden strength to leave room for either.
//!
//! At this value the loop settles in about two and a half minutes from cold
//! without overshooting, and carries a closed-loop pole around seven tenths -
//! well damped. A third of one, which is where this started, sat at eight and a
//! half tenths and rang: it reached the band sooner, went four parts per million
//! past it, and took three minutes more to be rid of that. The slower answer is
//! the one that arrives once.
constexpr double GAIN = 0.18;

//! Below this the loop is as close as its own measurement can tell, and moving
//! again would only be chasing the noise.
//!
//! Three parts per million because that is what the noise measures. Consecutive
//! readings on a settled title run between one and six parts per million with
//! the sign alternating, so a band of one and a half sat underneath its own
//! measurement floor: every reading looked worth acting on, the level walked
//! back and forth across a third of a step for hours, and nothing was ever
//! wrong. A band above the noise lets the loop stop.
constexpr double DEADBAND = 3.0e-6;

//! A move this big between one frame and the next is not a rate, it is somebody
//! moving the clock: the audio loop taking out a resync residue, a seek, the
//! genlock. Twenty milliseconds inside a fifteen second bucket reads as 1300ppm,
//! far beyond the whole range this can correct, so such an interval has to be
//! thrown away rather than averaged into a slope.
constexpr double JUMP = DVD_MSEC_TO_TIME(5.0);

//! Nothing this fast is a clock rate. Behind the jump guard rather than instead
//! of it: a step landing on the seam between two buckets is not a jump in any
//! single sample, and would otherwise arrive here looking like drift.
constexpr double MAX_PLAUSIBLE = 200e-6;

//! How often the dithered level is put out. Short enough that the ripple it
//! leaves - a step's worth of rate for this long - is microseconds.
constexpr double DITHER = 0.25 * DVD_TIME_BASE;

//! Given up after this many writes the clock would not take. Something else owns
//! it, and asking four times a second forever helps nobody.
constexpr unsigned int MAX_REFUSED = 5;

//! Decisions the level may sit against its limit while the drift refuses to
//! move. Past this the knob is not reaching whatever is playing - a rate the
//! actuator does not clock, something else holding the register - and the honest
//! thing is to put it back rather than leave the clamp applied for the title.
constexpr unsigned int MAX_CLAMPED = 8;

//! Drift still worth correcting. Sitting at the limit with more than this left
//! means the level has been moved by its whole range and the measurement has not
//! answered - so whatever the knob reaches, it is not this stream.
constexpr double ANSWERED = 3e-6;
} // namespace

CAMLAudioTrim::~CAMLAudioTrim()
{
  // Only if something was actually written. A box that never armed this must not
  // have its audio clock moved by the destructor of a singleton it never used.
  //
  // Bare, and without logging: this runs at static destruction, after the log
  // service has gone, and both a null service and an exception escaping a
  // destructor end the process rather than the write.
  if (!m_everWrote || m_applied == 0)
    return;

  try
  {
    CSysfsPath{TRIM_PATH}.Set(0);
  }
  catch (...)
  {
  }
}

bool CAMLAudioTrim::Armed()
{
  try
  {
    return CSysfsPath{ENABLE_PATH}.Exists();
  }
  catch (...)
  {
    return false;
  }
}

std::optional<int> CAMLAudioTrim::ReadLevel() const
{
  try
  {
    CSysfsPath path{TRIM_PATH};
    if (!path.Exists())
      return std::nullopt;

    // Against the hardware's range and not the loop's. A level this refuses is
    // read as no answer at all, which freezes the loop and strands whatever the
    // register happens to hold - so a ceiling here that is lower than one the
    // loop can legitimately reach is a latch, not a guard.
    const std::optional<int> got = path.Get<int>();
    if (!got || std::abs(*got) > static_cast<int>(MAX_LEVEL_HW) * STEP)
      return std::nullopt;

    return got;
  }
  catch (...)
  {
    return std::nullopt;
  }
}

bool CAMLAudioTrim::Supported()
{
  try
  {
    return CSysfsPath{TRIM_PATH}.Exists();
  }
  catch (...)
  {
    return false;
  }
}

bool CAMLAudioTrim::WriteLevel(int level)
{
  try
  {
    CSysfsPath{TRIM_PATH}.Set(level);
  }
  catch (...)
  {
    // The write may still have landed, so the read below decides.
  }

  m_everWrote = true;

  // Said on every write while this is young. A level the loop believes it has
  // applied and the register does not hold is indistinguishable, from the
  // outside, from an actuator that does nothing - and that mistake has already
  // been made once, on hardware, for the minutes it took the watchdog to
  // fire - a stretch that grows as the gain falls, since the stand-down counts
  // decisions and a slower loop reaches its limit later.
  CLog::Log(LOGDEBUG, LOGAUDIO, "CAMLAudioTrim: write {} (had {})", level, m_applied);

  // Read back rather than assume. The kernel drops its own record when the clock
  // framework reprograms the divider - an audio rate change does that - and
  // answers with what it is actually holding, which is how that becomes visible
  // here instead of the loop steering against a level that no longer exists.
  const std::optional<int> got = ReadLevel();
  if (!got)
  {
    m_applied = 0;
    return false;
  }

  if (*got != level)
  {
    CLog::Log(LOGDEBUG, LOGAUDIO, "CAMLAudioTrim: write {} came back {}", level, *got);
    if (++m_refused >= MAX_REFUSED)
    {
      CLog::Log(LOGWARNING, "CAMLAudioTrim: asked for {} and the clock kept {}, giving up", level,
                *got);
      m_stopped = true;
    }
    m_applied = *got;
    return false;
  }

  m_refused = 0;
  m_applied = *got;
  CLog::Log(LOGDEBUG, LOGAUDIO, "CAMLAudioTrim: write {} took", level);
  return true;
}

void CAMLAudioTrim::ForgetLocked()
{
  m_haveLastMean = false;
  m_haveLastError = false;
  m_bucketCount = 0;
  m_bucketStart = 0.0;
  m_lastSampleAt = 0.0;
  m_bucketSum = 0.0;
  m_slopeCount = 0;
}

void CAMLAudioTrim::Forget()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  ForgetLocked();
}

void CAMLAudioTrim::Release()
{
  std::lock_guard<std::mutex> lock(m_mutex);

  ForgetLocked();

  if (m_everWrote && m_applied != 0)
    WriteLevel(0);

  m_want = 0.0;
  m_dither = 0.0;

  // A refusal or a stand-down belongs to the playback that provoked it. One
  // stream the knob could not reach, or a handful of writes refused while the
  // audio path was changing rate underneath, must not be the reason every later
  // film goes uncorrected - and without this the only way back is a restart.
  m_stopped = false;
  m_refused = 0;
  m_clamped = 0;
  m_haveDriftAtZero = false;
  m_lastDither = 0.0;
}

double CAMLAudioTrim::Aim() const
{
  if (!m_haveSlew || !m_haveLead || std::abs(m_lead) < OFFSET_DEADBAND)
    return 0.0;

  return std::clamp(-m_lead / OFFSET_TAU, -m_slew, m_slew);
}

double CAMLAudioTrim::Feedforward() const
{
  // What the aim is worth in levels, computed rather than integrated toward.
  // The step was measured on this hardware at 15.05ppm per sigma-delta unit -
  // 30.1ppm per level against the 30.0 assumed here, inside half a percent -
  // so the level a given rate needs is known in advance and there is nothing to
  // be learned by walking to it.
  //
  // This is what makes the correction quick. Left to the integrator alone the
  // offset could only come out as fast as the rate loop settles, which is
  // minutes: the outer loop would ask for a rate, the inner one would take three
  // minutes to get there, and by then the offset had moved on. Handed the level
  // outright, the rate is there within a few ticks and the offset decays on
  // OFFSET_TAU as intended, with the integrator left doing what it is good at -
  // taking out the part this got wrong.
  return Aim() / STEP_RATE;
}

//! The cap, in parts per million, or nullopt for a file that is not there or
//! does not say a usable number. Anything out of bounds is refused rather than
//! clamped: a file saying 6000 is a typo for 600, and honouring it as the
//! largest legal value would be reading a mistake as an instruction.
std::optional<double> CAMLAudioTrim::ReadSlew() const
{
  const auto component = CServiceBroker::GetSettingsComponent();
  if (!component)
    return std::nullopt;

  const auto settings = component->GetSettings();
  if (!settings)
    return std::nullopt;

  // Zero is off, and so is anything the loop cannot act on: the bounds are the
  // ones the setting is clamped to anyway, so a value outside them reads as off
  // rather than as a rate nobody meant.
  const double ppm =
      static_cast<double>(settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_AUDIOLEAD));
  const double slew = std::abs(ppm) * 1e-6;
  if (!std::isfinite(slew) || slew < SLEW_MIN || slew > SLEW_MAX)
    return std::nullopt;

  return slew;
}

void CAMLAudioTrim::Settle(double reading)
{
  // Kept as a small history and acted on by the middle value. A clock step too
  // small for the jump guard - the audio loop taking out a millisecond or two -
  // lands on one slope only, and the middle of three is never the odd one out.
  // Costs a bucket of lag and buys immunity to a whole class of disturbance the
  // loop cannot otherwise tell from a rate.
  //
  // The window overlaps rather than being decimated, so the loop keeps its
  // fifteen second sample period and a reading can be the middle one for two or
  // three decisions running. That is a delay, not a gain: the median passes the
  // average sample through once, so what it costs is phase and not amplitude.
  m_slopes[m_slopeCount % m_slopes.size()] = reading;
  if (++m_slopeCount < m_slopes.size())
    return;

  std::array<double, 3> sorted = m_slopes;
  std::sort(sorted.begin(), sorted.end());
  const double drift = sorted[sorted.size() / 2];

  // drift is dimensionless: how fast the audio gains on the clock. Positive is
  // audio running fast, which wants a lower level. The sign is the measured one:
  // raising the level was seen to raise the rate on this hardware, whatever the
  // divider arithmetic suggests about which way round that ought to be.
  const double aim = Aim();

  // Widened by whatever the level is allowed to command. This test is here to
  // throw out readings no clock could have produced, and once the loop may ask
  // for 600ppm a 600ppm reading is the loop working, not a bad measurement.
  // Left at a fixed 200ppm it discards every reading taken while the offset is
  // being walked out, the level stops being updated, and the rate it happens to
  // be stuck at then runs uncorrected for the rest of the title.
  //
  // Not measured against the aim either, tempting as that is: during the ramp
  // the rate legitimately lags what was asked for by most of the step, so that
  // test rejects the whole transient it is supposed to allow.
  const double plausible = MAX_PLAUSIBLE + m_levelMax * STEP_RATE;
  if (std::abs(drift) > plausible)
  {
    CLog::Log(LOGDEBUG, LOGAUDIO, "CAMLAudioTrim: {:+.0f}ppm is not a rate, ignoring it",
              drift * 1e6);
    return;
  }

  // The rate the loop is asked to hold. Zero while nothing is driving the
  // offset, and then this is the loop it always was.
  //
  // Otherwise the offset is walked out by aiming off: an offset of l is taken
  // away over OFFSET_TAU by running the audio at -l/tau, and when l reaches zero
  // so does the aim, which is the loop stopping by itself rather than being
  // stopped. It has to enter as a setpoint and not as a bias on the level - the
  // integral below drives the rate to whatever it is told, so a bias added after
  // it would simply be integrated back out again.
  const double err = drift - aim;

  if (std::abs(err) < DEADBAND)
    return;

  const double was = m_want;
  m_want -= GAIN * err / STEP_RATE;

  // The integrator keeps the rate range it always had. What the offset needs on
  // top of that is not integrated at all - see Feedforward().
  m_want = std::clamp(m_want, -MAX_LEVEL_RATE, MAX_LEVEL_RATE);

  // Held against its limit while the drift says nothing changed. The level has
  // been moved by its whole range and the measurement has not answered, so
  // whatever the knob reaches, it is not this stream: put it back and stop,
  // rather than leave sixty parts per million of correction on a clock that
  // never asked for it.
  // What the drift was while the level was near zero, so that sitting at the
  // limit can be told from the limit doing nothing.
  if (std::abs(was) < 0.5)
  {
    m_driftAtZero = drift;
    m_haveDriftAtZero = true;
  }

  // Two different things pin the level to its stop. The knob may not reach this
  // stream, and then the trim should come off. Or it reaches it and the error is
  // simply larger than the range, and then it is taking out all it can and
  // giving up would hand back the part it had. So the test is not "still
  // drifting" but "moved the level and nothing happened".
  const double moved = m_haveDriftAtZero ? std::abs(drift - m_driftAtZero) : 0.0;
  const bool answered = moved > 0.5 * MAX_LEVEL_RATE * STEP_RATE;

  // Against err and not drift. Walking an offset out means deliberately holding
  // a large rate error, and a level sitting at its limit doing exactly what it
  // was told is not a knob that has stopped listening.
  if (std::abs(m_want) >= MAX_LEVEL_RATE && std::abs(err) > ANSWERED && !answered)
    ++m_clamped;
  else
    m_clamped = 0;

  if (m_clamped >= MAX_CLAMPED)
  {
    CLog::Log(LOGWARNING,
              "CAMLAudioTrim: {:+.1f}ppm after {} decisions at the limit, the clock is not "
              "listening - standing down",
              drift * 1e6, m_clamped);
    m_want = 0.0;
    m_dither = 0.0;
    if (m_everWrote && m_applied != 0)
      WriteLevel(0);
    m_stopped = true;
    return;
  }

  if (aim != 0.0)
    CLog::Log(LOGDEBUG, LOGAUDIO,
              "CAMLAudioTrim: audio {:+.1f}ppm, aiming {:+.1f}ppm to take out {:+.1f}ms, level "
              "{:.2f} -> {:.2f}",
              drift * 1e6, aim * 1e6, m_lead / 1000.0, was, m_want);
  else
    CLog::Log(LOGDEBUG, LOGAUDIO, "CAMLAudioTrim: audio {:+.1f}ppm, level {:.2f} -> {:.2f}",
              drift * 1e6, was, m_want);
}

void CAMLAudioTrim::Update(const std::optional<double>& error,
                           const std::optional<double>& lead,
                           double now)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  m_haveLead = lead.has_value();
  if (lead)
    m_lead = *lead;

  if (m_stopped)
    return;

  // The clock this paces itself against belongs to the player, and stopping a
  // playback destroys it: the next one starts a new clock counting from zero.
  // A timestamp kept from the last one is then in the future by however long
  // that playback ran, and the gate at the bottom holds this off the register
  // for exactly that long - silently, because nothing below it is reached, so
  // the level integrates to its limit against an actuator it never wrote to.
  // Taking a backwards clock as a reset covers that, and also a straggling tick
  // from the codec being torn down, which would otherwise stamp the old time
  // back on afterwards.
  if (now < m_lastDither || now < m_bucketStart)
  {
    ForgetLocked();
    m_lastDither = 0.0;
  }

  // A bucket left open across a gap is not a measurement either: nothing ticks
  // while the player is held, so the next tick would close a bucket's worth of
  // samples over however long the hold lasted and read the drift that much too
  // small. Keyed on the gap since the last sample rather than on the bucket's
  // age, because a pause is caught wherever in the bucket it happens to fall -
  // an age test only sees the ones that outlast the bucket itself.
  if (m_bucketStart > 0.0 && m_lastSampleAt > 0.0 && now - m_lastSampleAt > SAMPLE_GAP)
    ForgetLocked();

  if (!m_checked)
  {
    m_checked = true;
    m_present = ReadLevel().has_value();
    if (!m_present)
      CLog::Log(LOGINFO, "CAMLAudioTrim: no audio_sdm_trim in this kernel");
  }

  if (!m_present)
    return;

  // Looked for on the dither tick rather than every frame: this is a stat of a
  // file on the app thread, and a quarter second is soon enough to notice.
  if (now - m_lastDither >= DITHER && !Armed())
  {
    if (m_everWrote && m_applied != 0)
    {
      ForgetLocked();
      WriteLevel(0);
      m_want = 0.0;
      m_dither = 0.0;
    }
    m_lastDither = now;
    return;
  }

  // Below the arming gate, so a disarmed loop does no file IO at all. Re-read
  // rather than latched: changing it mid playback is how the cap gets found.
  if (now - m_lastDither >= DITHER)
  {
    const std::optional<double> slew = ReadSlew();

    // Acted on only once it has said the same thing twice. The setting can move
    // under the loop while a slider is being dragged, and taking the first value
    // seen would otherwise dump the integrator and log a spurious
    // pair of on/off lines.
    if (slew == m_slewSeen)
    {
      const bool had = m_haveSlew;
      m_haveSlew = slew.has_value();
      m_slew = slew.value_or(0.0);

      // Floored to a whole level. The clamp below rounds to an integer before
      // applying it, so a fractional ceiling clamps a rounded level back to a
      // fraction, the cast to int then truncates it, and the sigma-delta carry
      // it did not pay off latches and is never paid off again.
      // Off means the clock is not touched at all, not merely that the offset is
      // left alone: with no ceiling the level is always zero, the write is skipped,
      // and what remains is measurement. That keeps the drift readings - cleaner
      // ones, since nothing is correcting underneath them - on hardware this has
      // never been tried on, without writing that hardware's audio clock.
      m_levelMax = m_haveSlew
                       ? std::floor(std::min(MAX_LEVEL_HW, MAX_LEVEL_RATE + m_slew / STEP_RATE))
                       : 0.0;

      // Deliberately not clamping m_want here. Dropping the ceiling under a
      // wound-up level would step the rate by the whole difference at once, and
      // taking the file away is the documented kill switch - the one path that
      // must not itself be the most violent thing the loop ever does. With the
      // aim gone the integrator walks it back down within a few decisions, and
      // the per-tick limit in WriteLevel bounds how fast that reaches the clock.
      if (m_haveSlew != had)
        CLog::Log(LOGINFO, "CAMLAudioTrim: offset loop {}",
                  m_haveSlew ? fmt::format("on, up to {:+.0f}ppm", m_slew * 1e6) : "off");
    }
    m_slewSeen = slew;
  }

  if (error)
  {
    // A clock step moves the error without the audio rate having changed, and
    // the bucket it lands in is no longer a measurement of anything.
    if (m_haveLastError && std::abs(*error - m_lastError) > JUMP)
    {
      m_bucketStart = 0.0;
      m_bucketSum = 0.0;
      m_bucketCount = 0;
      m_haveLastMean = false;
    }

    m_lastError = *error;
    m_haveLastError = true;

    if (m_bucketStart == 0.0)
      m_bucketStart = now;

    m_bucketSum += *error;
    ++m_bucketCount;
    m_lastSampleAt = now;

    if (now - m_bucketStart >= BUCKET)
    {
      if (m_bucketCount >= BUCKET_MIN_SAMPLES)
      {
        const double mean = m_bucketSum / m_bucketCount;

        // Between the middles of two buckets, which is what the difference of
        // their means measures - not between their edges. True only while the
        // buckets are consecutive, which is why a short one drops the pairing
        // below rather than leaving its mean to be differenced across the gap.
        if (m_haveLastMean)
          Settle((mean - m_lastMean) / (now - m_bucketStart));

        m_lastMean = mean;
        m_haveLastMean = true;
      }
      else
      {
        // Too few frames to average. The next bucket is no longer adjacent to
        // the last one that counted, and the interval between them is not the
        // one the slope would be divided by.
        m_haveLastMean = false;
        m_slopeCount = 0;
      }

      m_bucketStart = now;
      m_bucketSum = 0.0;
      m_bucketCount = 0;
    }
  }

  if (now - m_lastDither < DITHER)
    return;

  m_lastDither = now;

  // Sigma-delta: the fraction the level cannot express is carried and paid off
  // by the next slot that can, so the average over a few slots is the wanted
  // level even though every slot is a whole one.
  const double target =
      std::clamp(m_want + Feedforward(), -m_levelMax, m_levelMax) + m_dither;

  // Clamped after rounding as well as before: the carried fraction can take a
  // level already at the limit half a step past it, and the kernel refuses the
  // whole write rather than the excess.
  // Read before the level is decided, not after. What the step below may be is
  // measured from where the register actually is, so this has to be the fresh
  // value - against last tick's remembered one the limit is computed from a base
  // that may have moved, and bounds the wrong distance.
  //
  // Against the register, not against what was last written to it. A remembered
  // value that the register no longer holds is a loop that has stopped driving
  // and cannot tell: it goes on integrating, reaches its limit, and leaves the
  // clock untouched the whole time. Observed on a 48kHz title after a run of
  // rapid format changes - level at the clamp, register at zero, and nothing in
  // between the two to notice. One read a quarter second is worth more than the
  // memory it replaces, and it also heals whatever else moved the knob.
  const std::optional<int> got = ReadLevel();
  if (!got)
    return;

  m_applied = *got;

  double put = std::clamp(std::round(target), -m_levelMax, m_levelMax);

  // One level a tick. The dither already moves by exactly one, so this is
  // invisible in normal running; what it bounds is the ways the loop can be
  // asked to move the level far in one go - the ceiling dropping when the cap
  // file is taken away, an integrator that wound up while the register was being
  // refused, or the register having been moved from outside. A rate
  // discontinuity is the one thing a receiver locked to this clock cannot be
  // asked to follow.
  //
  // Only this path. Release, a stand-down and a disarm all write zero directly
  // and are meant to: the first two mean the level must not outlive what asked
  // for it, and the third is a kill switch. Those go straight there.
  if (m_everWrote)
    put = std::clamp(put, static_cast<double>(m_applied) / STEP - 1.0,
                     static_cast<double>(m_applied) / STEP + 1.0);

  m_dither = std::clamp(target - put, -1.0, 1.0);

  const int level = static_cast<int>(put) * STEP;
  if (level != m_applied)
    WriteLevel(level);
}
