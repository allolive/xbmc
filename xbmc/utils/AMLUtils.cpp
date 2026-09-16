/*
 *  Copyright (C) 2011-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <fcntl.h>
#include <regex>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "AMLUtils.h"

#include <cstdint>

#include <cstring>

#include <charconv>
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "settings/AdvancedSettings.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "ServiceBroker.h"
#include "utils/RegExp.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "platform/linux/SysfsPath.h"
#include "windowing/amlogic/WinSystemAmlogic.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <amcodec/codec.h>
#include <xf86drm.h>

int aml_get_cpufamily_id()
{
  static int aml_cpufamily_id = -1;
  if (aml_cpufamily_id == -1)
  {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::regex re(".*: (.*)$");

    for (std::string line; std::getline(cpuinfo, line);)
    {
      if (line.find("Serial") != std::string::npos)
      {
        std::smatch match;

        if (std::regex_match(line, match, re) && match.size() == 2)
        {
          std::ssub_match value = match[1];
          std::string cpu_family = value.str().substr(0, 2);
          try
          {
            aml_cpufamily_id = std::stoi(cpu_family, nullptr, 16);
          }
          catch (const std::exception&)
          {
            aml_cpufamily_id = -1;
          }
          break;
        }
      }
    }
  }
  return aml_cpufamily_id;
}

std::string aml_get_cpufamily_name(int cpuid)
{
  switch(cpuid)
  {
    case AML_G12A:
      return "G12A";
    case AML_G12B:
      return "G12B";
    case AML_SM1:
      return "SM1";
    case AML_SC2:
      return "SC2";
    case AML_T7:
      return "T7";
    case AML_S4:
      return "S4";
    case AML_S5:
      return "S5";
    case AML_S7:
      return "S7";
    case AML_S7D:
      return "S7D";
    case AML_S6:
      return "S6";
    default:
      {
        int resolved = aml_get_cpufamily_id();
        if (cpuid != resolved)
          return aml_get_cpufamily_name(resolved);
      }
  }
  return "Unknown";
}

static bool aml_support_vcodec_profile(const char *regex)
{
  int profile = 0;
  CRegExp regexp;
  regexp.RegComp(regex);
  std::string valstr;
  CSysfsPath vcodec_profile{"/sys/class/amstream/vcodec_profile"};
  if (vcodec_profile.Exists())
  {
    valstr = vcodec_profile.Get<std::string>().value_or("");
    profile = (regexp.RegFind(valstr) >= 0) ? 1 : 0;
  }

  return profile;
}

bool aml_support_hevc()
{
  static int has_hevc = -1;

  if (has_hevc == -1)
      has_hevc = aml_support_vcodec_profile("(\\bhevc\\b|\\bhevc_fb\\b):");

  return (has_hevc == 1);
}

bool aml_support_hevc_4k2k()
{
  static int has_hevc_4k2k = -1;

  if (has_hevc_4k2k == -1)
    has_hevc_4k2k = aml_support_vcodec_profile("(\\bhevc\\b|\\bhevc_fb\\b):(?!\\;).*(4k|8k)");

  return (has_hevc_4k2k == 1);
}

bool aml_support_hevc_8k4k()
{
  static int has_hevc_8k4k = -1;

  if (has_hevc_8k4k == -1)
    has_hevc_8k4k = aml_support_vcodec_profile("(\\bhevc\\b|\\bhevc_fb\\b):(?!\\;).*8k");

  return (has_hevc_8k4k == 1);
}

bool aml_support_hevc_10bit()
{
  static int has_hevc_10bit = -1;

  if (has_hevc_10bit == -1)
    has_hevc_10bit = aml_support_vcodec_profile("(\\bhevc\\b|\\bhevc_fb\\b):(?!\\;).*10bit");

  return (has_hevc_10bit == 1);
}

bool aml_support_h266()
{
  static int has_h266 = -1;

  if (has_h266 == -1)
    has_h266 = aml_support_vcodec_profile("\\bh266\\b:");

  return (has_h266 == 1);
}

AML_SUPPORT_H264_4K2K aml_support_h264_4k2k()
{
  static AML_SUPPORT_H264_4K2K has_h264_4k2k = AML_SUPPORT_H264_4K2K_UNINIT;

  if (has_h264_4k2k == AML_SUPPORT_H264_4K2K_UNINIT)
  {
    has_h264_4k2k = AML_NO_H264_4K2K;

    if (aml_support_vcodec_profile("(\\bh264\\b|\\bmh264\\b):4k"))
      has_h264_4k2k = AML_HAS_H264_4K2K_SAME_PROFILE;
    else if (aml_support_vcodec_profile("\\bh264_4k2k\\b:"))
      has_h264_4k2k = AML_HAS_H264_4K2K;
  }
  return has_h264_4k2k;
}

bool aml_support_vp9()
{
  static int has_vp9 = -1;

  if (has_vp9 == -1)
    has_vp9 = aml_support_vcodec_profile("(\\bvp9\\b|\\bvp9_fb\\b):(?!\\;).*compressed");

  return (has_vp9 == 1);
}

bool aml_support_av1()
{
  static int has_av1 = -1;

  if (has_av1 == -1)
    has_av1 = aml_support_vcodec_profile("(\\bav1\\b|\\bav1_fb\\b):(?!\\;).*compressed");

  return (has_av1 == 1);
}

bool aml_support_avs2()
{
  static int has_avs2 = -1;

  if (has_avs2 == -1)
    has_avs2 = aml_support_vcodec_profile("(\\bavs2\\b|\\bavs2_fb\\b):(?!\\;).*compressed");

  return (has_avs2 == 1);
}

bool aml_support_avs3()
{
  static int has_avs3 = -1;

  if (has_avs3 == -1)
    has_avs3 = aml_support_vcodec_profile("\\bavs3\\b:(?!\\;).*compressed");

  return (has_avs3 == 1);
}

bool aml_support_dolby_vision()
{
  static int support_dv = -1;

  if (support_dv == -1)
  {
    CSysfsPath support_info{"/sys/class/amdolby_vision/support_info"};
    support_dv = 0;
    if (support_info.Exists())
    {
      support_dv = (int)((support_info.Get<int>().value_or(0) & 7) == 7);
      if (support_dv == 1) {
        CSysfsPath ko_info{"/sys/class/amdolby_vision/ko_info"};
        if (ko_info.Exists())
          CLog::Log(LOGINFO, "Amlogic Dolby Vision info: {}", ko_info.Get<std::string>().value_or("").c_str());
      }
    }
  }

  return (support_dv == 1);
}

bool aml_dolby_vision_enabled()
{
  bool dv_user_enabled(!CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE));

  bool dv_enabled = (!!aml_support_dolby_vision() &&
                     static_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem())
                         ->GetAmlDisplay()->aml_display_support_dv());

  return ((dv_enabled && !!dv_user_enabled) == 1);
}

bool aml_convert_to_dv_by_vs_engine(StreamHdrType hdrType)
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  bool dv_user_enabled(!settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE));
  bool user_convert_to_dv;

  // a tone mapping choice wins; it can be set while Dolby Vision is disabled
  if (hdrType == StreamHdrType::HDR_TYPE_NONE)
    user_convert_to_dv = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_SDR2DV) &&
                         !settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR);
  else
    user_convert_to_dv = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_HDR2DV) &&
                         !settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR);

  bool convert_to_dv = (!!aml_support_dolby_vision() &&
                        static_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem())
                            ->GetAmlDisplay()->aml_display_support_dv());

  return ((convert_to_dv && !!user_convert_to_dv && !!dv_user_enabled) == 1);
}

bool aml_video_started()
{
  CSysfsPath videostarted{"/sys/class/tsync/videostarted"};
  return (StringUtils::EqualsNoCase(videostarted.Get<std::string>().value_or("0x0"), "0x1"));
}

int aml_amdv_wait(StreamHdrType hdrType)
{
  if (hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
  {
    CSysfsPath amdv_wait_delay{"/sys/module/aml_media/parameters/amdv_wait_delay"};
    if (amdv_wait_delay.Exists())
      return amdv_wait_delay.Get<int>().value_or(0);
  }

  return 0;
}

void aml_set_3d_video_mode(unsigned int mode, bool framepacking_support, int view_mode)
{
  int fd;
  if ((fd = open("/dev/amvideo", O_RDWR)) >= 0)
  {
    if (ioctl(fd, AMSTREAM_IOC_SET_3D_TYPE, mode) != 0)
      CLog::Log(LOGERROR, "AMLUtils::{} - unable to set 3D video mode {:#x}", __FUNCTION__, mode);
    close(fd);

    CSysfsPath("/sys/module/aml_media/parameters/g_framepacking_support", framepacking_support ? 1 : 0);
    CSysfsPath("/sys/module/amvdec_h264mvc/parameters/view_mode", view_mode);
  }
}

std::optional<double> aml_since_frame_start_us(double maxAgeUs)
{
  const auto reading = aml_vblank_seq_and_age(maxAgeUs);
  if (!reading)
    return std::nullopt;

  return reading->second;
}

double aml_monotonic_us()
{
  // The base DRM reports on, so an age read against it can be carried forward.
  // CurrentHostCounter() is CLOCK_MONOTONIC_RAW and runs 180ms apart here.
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0.0;

  return static_cast<double>(now.tv_sec) * 1000000.0 +
         static_cast<double>(now.tv_nsec) / 1000.0;
}

std::optional<std::pair<uint64_t, double>> aml_vblank_seq_and_age(double maxAgeUs)
{
  const auto* winSystem = static_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  if (!winSystem)
    return std::nullopt;

  const CAMLDisplay* display = winSystem->GetAmlDisplay();
  if (!display)
    return std::nullopt;

  const int fd = display->aml_get_Device_handle();
  const uint32_t crtcId = display->aml_get_Crtc_id();
  if (fd < 0 || crtcId == 0)
    return std::nullopt;

  uint64_t sequence = 0;
  uint64_t vblankNs = 0;
  if (drmCrtcGetSequence(fd, crtcId, &sequence, &vblankNs) != 0 || vblankNs == 0)
    return std::nullopt;

  // DRM reports on CLOCK_MONOTONIC, so read the clock DRM used.
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return std::nullopt;

  const int64_t nowNs =
      static_cast<int64_t>(now.tv_sec) * 1000000000 + static_cast<int64_t>(now.tv_nsec);

  const double age = static_cast<double>(nowNs - static_cast<int64_t>(vblankNs)) / 1000.0;
  if (age < -maxAgeUs || age > maxAgeUs)
    return std::nullopt;

  return std::make_pair(sequence, age);
}

std::optional<uint32_t> aml_displayed_pts_ticks()
{
  CSysfsPath path("/sys/class/tsync/pts_video");
  if (!path.Exists())
    return std::nullopt;

  const std::string raw = path.Get<std::string>().value_or("");
  const char* p = raw.c_str();
  int base = 10;
  if (raw.size() > 2 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X'))
  {
    p += 2;
    base = 16;
  }

  uint32_t ticks = 0;
  const auto res = std::from_chars(p, p + std::strlen(p), ticks, base);
  if (res.ec != std::errc())
    return std::nullopt;
  if (ticks == 0)
    return std::nullopt;

  return ticks;
}

double aml_render_display_latency(StreamHdrType hdrType, float audioDelay)
{
  const auto winSystem = CServiceBroker::GetWinSystem();
  CGraphicContext& gfx = winSystem->GetGfxContext();

  const bool isHDRUsed =
      winSystem->GetOSHDRStatus() == HDR_STATUS::HDR_ON && hdrType != StreamHdrType::HDR_TYPE_NONE;
  float refresh = gfx.GetFPS();
  if (gfx.GetVideoResolution() == RES_WINDOW)
    refresh = 0;

  const double latencyTweak = static_cast<double>(
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->GetLatencyTweak(
          refresh, isHDRUsed, gfx.GetResInfo().iScreenHeight));

  // Whole milliseconds, the way CRenderManager::SetDelay() receives it, so the
  // aim and the renderer's scheduling do not differ by the truncation.
  const double videoDelay = static_cast<double>(static_cast<int>(audioDelay * 1000.0f));

  // Without GetFrameLatencyAdjustment(). Callers that want the vblank term
  // subtract it themselves, and CAMLGenlock does, so including it here would
  // take it off twice and move the aim by the frame loop's wake jitter.
  return DVD_MSEC_TO_TIME(latencyTweak + static_cast<double>(gfx.GetDisplayLatency()) -
                          videoDelay);
}

//! How much of the renderer's display latency was chosen rather than measured,
//! expressed the way it lands in the audio-versus-picture sum.
//!
//! CRenderManager builds m_displayLatency from four terms, and two of them are
//! somebody's decision rather than a property of the pipeline: the latency tweak
//! from advancedsettings, which says the display adds time that the picture
//! should be moved early to cover, and the audio offset, which asks for the two
//! to be deliberately apart. Both move which frame is on screen at a given clock
//! time, so both arrive in the alignment term looking exactly like error.
//!
//! Returned with the tweak's sign flipped against the offset's, because that is
//! how they enter: the sum carries +tweak and -offset, so their effect on
//! alignment is the other way round. A reader that subtracts this is left with
//! the part nobody asked for, which is the only part worth correcting.
double aml_render_chosen_offset(StreamHdrType hdrType, float audioDelay)
{
  const auto winSystem = CServiceBroker::GetWinSystem();
  CGraphicContext& gfx = winSystem->GetGfxContext();

  const bool isHDRUsed =
      winSystem->GetOSHDRStatus() == HDR_STATUS::HDR_ON && hdrType != StreamHdrType::HDR_TYPE_NONE;
  float refresh = gfx.GetFPS();
  if (gfx.GetVideoResolution() == RES_WINDOW)
    refresh = 0;

  const double latencyTweak = static_cast<double>(
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->GetLatencyTweak(
          refresh, isHDRUsed, gfx.GetResInfo().iScreenHeight));

  // Whole milliseconds, the way CRenderManager::SetDelay() receives it, so this
  // and the renderer's own sum cannot differ by the truncation.
  const double videoDelay = static_cast<double>(static_cast<int>(audioDelay * 1000.0f));

  return DVD_MSEC_TO_TIME(videoDelay - latencyTweak);
}

double aml_refreshes_per_frame(double displayRate, double fps)
{
  // Zero when the content outruns the display, which shows some frames and
  // drops others rather than repeating each of them.
  if (displayRate <= 0.0 || fps <= 0.0 || fps > displayRate)
    return 0.0;

  return displayRate / fps;
}
