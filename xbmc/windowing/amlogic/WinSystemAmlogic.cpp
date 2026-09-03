/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemAmlogic.h"

#include <string.h>
#include <float.h>
#include <exception>
#include <cmath>
#include <optional>

#include "ServiceBroker.h"
#include "cores/RetroPlayer/process/amlogic/RPProcessInfoAmlogic.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererOpenGLES.h"
#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/DVDCodecs/Video/AMLLatency.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/RendererAML.h"
#include "windowing/GraphicContext.h"
#include "windowing/Resolution.h"
#include "platform/linux/powermanagement/LinuxPowerSyscall.h"
#include "platform/linux/FDEventMonitor.h"
#include "rendering/gles/ScreenshotSurfaceGLES.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "settings/lib/SettingsManager.h"
#include "guilib/DispResource.h"
#include "utils/AMLUtils.h"
#include "utils/StringUtils.h"
#include "utils/log.h"
#include "threads/SingleLock.h"

#include "platform/linux/SysfsPath.h"

#include <linux/fb.h>
#include <poll.h>
#include <unistd.h>

#include "system_egl.h"

using namespace KODI;

std::unique_ptr<CAMLDisplay> CWinSystemAmlogic::m_amlDisplay = nullptr;

CWinSystemAmlogic::CWinSystemAmlogic()
:  m_libinput(new CLibInputHandler)
,  m_force_mode_switch(false)
,  m_fdMonitorId(-1)
,  m_udev(NULL)
,  m_callback_data(NULL, NULL)
{
  const char *env_framebuffer = getenv("FRAMEBUFFER");

  m_amlDisplay = std::make_unique<CAMLDisplay>();
  m_amlGBMUtils = std::make_unique<CAMLGBMUtils>(m_amlDisplay->aml_get_Device_handle());

  // default to framebuffer 0
  m_framebuffer_name = "fb0";
  if (env_framebuffer)
  {
    std::string framebuffer(env_framebuffer);
    std::string::size_type start = framebuffer.find("fb");
    if (start != std::string::npos)
      m_framebuffer_name = framebuffer.substr(start);
  }

  m_nativeDisplay = EGL_NO_DISPLAY;
  m_stereo_mode = RenderStereoMode::OFF;
  m_delayDispReset = false;

  m_nativeGUI = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING);

  m_libinput->Start();
}

CWinSystemAmlogic::~CWinSystemAmlogic()
{
  MonitorStop();
}

void CWinSystemAmlogic::SettingOptionsComponentsFiller(const SettingConstPtr& setting,
                                                 std::vector<IntegerSettingOption>& list,
                                                 int& current)
{
  if (m_amlDisplay->aml_display_support_dv())
  {
    const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
    CHDRCapabilities dv_cap = m_amlDisplay->GetHDRCaps();

    if (dv_cap.SupportsDVTVLED())
      list.emplace_back(CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(14426),
                        AML_DV_TV_LED);

    if (dv_cap.SupportsDVPlayerLED())
      list.emplace_back(CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(14427),
                        AML_DV_PLAYER_LED);

    AML_DISPLAY_DV_LED old_value = static_cast<AML_DISPLAY_DV_LED>(
      settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_LED));
    AML_DISPLAY_DV_LED new_value = old_value;

    if (old_value == AML_DV_TV_LED && !dv_cap.SupportsDVTVLED())
      new_value = static_cast<AML_DISPLAY_DV_LED>(dv_cap.SupportsDVPlayerLED() ? AML_DV_PLAYER_LED : -1);

    if (old_value == AML_DV_PLAYER_LED && !dv_cap.SupportsDVPlayerLED())
      new_value = static_cast<AML_DISPLAY_DV_LED>(dv_cap.SupportsDVTVLED()? AML_DV_TV_LED : -1);

    if (new_value != -1)
      current = new_value;
  }
}

void CWinSystemAmlogic::MonitorStart()
{
  int err;

  if (!m_udev && m_fdMonitorId == -1)
  {
    m_udev = udev_new();
    if (!m_udev)
    {
      CLog::Log(LOGWARNING, "CWinSystemAmlogic::Start - Unable to open udev handle");
      return;
    }

    m_callback_data.udevMonitor = udev_monitor_new_from_netlink(m_udev, "udev");
    if (!m_callback_data.udevMonitor)
    {
      CLog::Log(LOGERROR, "CWinSystemAmlogic::Start - udev_monitor_new_from_netlink() failed");
      goto err_unref_udev;
    }

    err = udev_monitor_filter_add_match_subsystem_devtype(m_callback_data.udevMonitor, "drm", NULL);
    if (err)
    {
      CLog::Log(LOGERROR, "CWinSystemAmlogic::Start - udev_monitor_filter_add_match_subsystem_devtype() failed");
      goto err_unref_udev;
    }

    err = udev_monitor_enable_receiving(m_callback_data.udevMonitor);
    if (err)
    {
      CLog::Log(LOGERROR, "CWinSystemAmlogic::Start - udev_monitor_enable_receiving() failed");
      goto err_unref_udev;
    }

    const auto eventMonitor = CServiceBroker::GetPlatform().GetService<CFDEventMonitor>();
    m_callback_data.object = this;
    m_fdMonitorId = 0;
    eventMonitor->AddFD(
        CFDEventMonitor::MonitoredFD(udev_monitor_get_fd(m_callback_data.udevMonitor),
                                     POLLIN, FDEventCallback, (void *)&m_callback_data),
        m_fdMonitorId);
  }

  return;

err_unref_udev:
  MonitorStop();
}

void CWinSystemAmlogic::MonitorStop()
{
  if (m_fdMonitorId != -1)
  {
    const auto eventMonitor = CServiceBroker::GetPlatform().GetService<CFDEventMonitor>();
    eventMonitor->RemoveFD(m_fdMonitorId);
    m_fdMonitorId = -1;
  }

  if (m_callback_data.udevMonitor)
  {
    udev_monitor_unref(m_callback_data.udevMonitor);
    m_callback_data.udevMonitor = NULL;
  }

  if (m_udev)
  {
    udev_unref(m_udev);
    m_udev = NULL;
  }
}

bool CWinSystemAmlogic::MessagePump()
{
  if (m_hotplugPending.exchange(false))
    HotplugEvent();

  return false;
}

void CWinSystemAmlogic::HotplugEvent()
{
  SetPresentationReady(false);

  try
  {
    m_amlDisplay->aml_init_drmDevice();
  }
  catch (const std::exception& e)
  {
    CLog::Log(LOGERROR, "CWinSystemAmlogic::{} - display rebuild failed: {}", __FUNCTION__,
              e.what());
    return;
  }

  drmModeConnection connection;
  int mode_count = m_amlDisplay->aml_get_display_modes_count(&connection);

  RefreshDisplayCapabilities();

  if (connection == DRM_MODE_DISCONNECTED && mode_count == 1)
  {
    CLog::Log(LOGWARNING,
      "CWinSystemAmlogic - HotplugEvent mode switch ignored while HDMI DRM connector is not ready ({:d} modes)",
      mode_count);
    return;
  }

  std::string preferred_mode = m_amlDisplay->aml_get_preferred_mode();
  RESOLUTION res = static_cast<RESOLUTION>(RES_DESKTOP);

  CDisplaySettings::GetInstance().ClearCustomResolutions();
  RefreshResolutions();
  CDisplaySettings::GetInstance().ApplyCalibrations();

  if (!preferred_mode.empty())
  {
    for (size_t resolution = RES_DESKTOP; resolution < CDisplaySettings::GetInstance().ResolutionInfoSize(); resolution++)
    {
      RESOLUTION_INFO resinfo = CDisplaySettings::GetInstance().GetResolutionInfo(resolution);
      if (StringUtils::EqualsNoCase(resinfo.strId, preferred_mode))
      {
        res = static_cast<RESOLUTION>(resolution);
        break;
      }
    }

    CLog::Log(LOGDEBUG, "CWinSystemAmlogic - HotplugEvent, preferred mode: {}, display mode: {}",
      preferred_mode, CDisplaySettings::GetInstance().GetResolutionInfo(res).strId);
  }
  else
    CLog::Log(LOGWARNING, "CWinSystemAmlogic - HotplugEvent, no preferred mode defined, use display mode: {}",
      CDisplaySettings::GetInstance().GetResolutionInfo(res).strId);

  m_amlDisplay->SetHotPlug();
  CServiceBroker::GetWinSystem()->GetGfxContext().SetVideoResolution(res, true);
  CServiceBroker::GetWinSystem()->GetGfxContext().ApplyModeChange(res);
}

void CWinSystemAmlogic::FDEventCallback(int id, int fd, short revents, void *data)
{
  struct callback_data *callbackData = (struct callback_data *)data;
  if (!callbackData || !callbackData->udevMonitor || !callbackData->object)
    return;

  struct udev_monitor *udevMonitor = callbackData->udevMonitor;
  CWinSystemAmlogic *winSystem = callbackData->object;
  struct udev_device *device;

  while ((device = udev_monitor_receive_device(udevMonitor)) != NULL)
  {
    const char* action = udev_device_get_action(device);
    const char* syspath = udev_device_get_syspath(device);
    const char* devpath = udev_device_get_devpath(device);
    const char* hotplug = udev_device_get_property_value(device, "HOTPLUG");
    CLog::Log(LOGDEBUG, "CWinSystemAmlogic - FDEventCallback (\"{}\", \"{}\"), action: {}",
      syspath ? syspath : "<null>", devpath ? devpath : "<null>", action ? action : "<null>");

    const bool isHotplug = action && hotplug && StringUtils::EqualsNoCase(action, "change") &&
                           strcmp(hotplug, "1") == 0;

    udev_device_unref(device);

    if (isHotplug)
      winSystem->m_hotplugPending.store(true);
  }
}

bool CWinSystemAmlogic::InitWindowSystem()
{
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  RefreshDisplayCapabilities();

  if (settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_NOISEREDUCTION))
  {
     CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem -- disabling noise reduction");
     CSysfsPath("/sys/module/aml_media/parameters/nr2_en", 0);
  }

  if (!IsHDRDisplay())
  {
    CSysfsPath("/sys/module/aml_media/parameters/sdr_mode", 0);
    CSysfsPath("/sys/module/aml_media/parameters/hdr_mode", 0);
    CSysfsPath("/sys/module/aml_media/parameters/dolby_vision_policy", 1);
    CSysfsPath("/sys/module/aml_media/parameters/hdr_policy", 1);
  }

  if (!aml_support_dolby_vision())
  {
    settings->SetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE, false);
    settings->SetBool(CSettings::SETTING_COREELEC_AMLOGIC_SDR2DV, false);
    settings->SetBool(CSettings::SETTING_COREELEC_AMLOGIC_HDR2DV, false);
    settings->SetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_LED, AML_DV_TV_LED);
    settings->SetBool(CSettings::SETTING_VIDEOPLAYER_DOVIZEROLEVEL5, true);
  }

  CServiceBroker::GetSettingsComponent()->GetSettings()->
    GetSettingsManager()->RegisterSettingOptionsFiller("dv_led_modes", SettingOptionsComponentsFiller);

  m_nativeDisplay = EGL_DEFAULT_DISPLAY;

  CDVDVideoCodecAmlogic::Register();
  CLinuxRendererGLES::Register();
  RETRO::CRPProcessInfoAmlogic::Register();
  RETRO::CRPProcessInfoAmlogic::RegisterRendererFactory(new RETRO::CRendererFactoryOpenGLES);
  CRendererAML::Register();
  CScreenshotSurfaceGLES::Register();

  auto setting = settings->GetSetting(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK);
  if (setting)
  {
    setting->SetVisible(false);
    settings->SetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK, false);
  }

  // Close the OpenVFD splash and switch the display into time mode.
  CSysfsPath("/tmp/openvfd_service", 0);

  drmModeConnection connection;
  int mode_count = m_amlDisplay->aml_get_display_modes_count(&connection);

  if (connection == DRM_MODE_DISCONNECTED)
  {
    if (mode_count > 1)
    {
      CLog::Log(LOGWARNING,
        "CWinSystemAmlogic::InitWindowSystem HDMI modes are present but DRM connector is not ready, defer hotplug");
      m_hotplugPending.store(true);
    }
    else if (mode_count == 1)
    {
      CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem Looks like no display is connected, wait for hotplug");
    }
  }

  MonitorStart();

  // kill a running animation
  CLog::Log(LOGDEBUG,"CWinSystemAmlogic: Sending SIGUSR1 to 'splash-image'");
  std::system("killall -s SIGUSR1 splash-image &> /dev/null");

  return CWinSystemBase::InitWindowSystem();
}

bool CWinSystemAmlogic::DestroyWindowSystem()
{
  return true;
}

bool CWinSystemAmlogic::CreateNewWindow(const std::string& name,
                                    bool fullScreen,
                                    RESOLUTION_INFO& res)
{
  bool ret;

  SetPresentationReady(false);
  m_nWidth        = res.iWidth;
  m_nHeight       = res.iHeight;
  m_fRefreshRate  = res.fRefreshRate;

  int delay = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt("videoscreen.delayrefreshchange");
  if (delay > 0)
  {
    m_delayDispReset = true;
    m_dispResetTimer.Set(std::chrono::milliseconds(static_cast<unsigned int>(delay * 100)));
  }

  {
    std::unique_lock<CCriticalSection> lock(m_resourceSection);
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
    {
      (*i)->OnLostDisplay();
    }
  }

  if ((ret = m_amlDisplay->set_native_resolution(res, m_framebuffer_name, m_stereo_mode,
                                           m_force_mode_switch, m_hotplug_mode_switch)))
  {
    m_bWindowCreated = true;
  }

  m_force_mode_switch = false;
  m_hotplug_mode_switch = false;
  return ret;
}

bool CWinSystemAmlogic::DestroyWindow()
{
  SetPresentationReady(false);
  m_bWindowCreated = false;
  return true;
}

void CWinSystemAmlogic::RefreshResolutions()
{
  RESOLUTION_INFO resDesktop, curDisplay;
  std::vector<RESOLUTION_INFO> resolutions;

  if (!m_amlDisplay->aml_probe_resolutions(resolutions) || resolutions.empty())
    CLog::Log(LOGWARNING, "{}: ProbeResolutions failed.",__FUNCTION__);

  // get all resolutions supported by connected device
  if (m_amlDisplay->aml_get_native_resolution(&curDisplay))
    resDesktop = curDisplay;

  for (auto& res : resolutions)
  {
    CLog::Log(LOGINFO, "Found resolution {:d} x {:d} with {:d} x {:d}{} @ {:f} Hz",
      res.iWidth,
      res.iHeight,
      res.iScreenWidth,
      res.iScreenHeight,
      res.dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "",
      res.fRefreshRate);

    // add new custom resolution
    CServiceBroker::GetWinSystem()->GetGfxContext().ResetOverscan(res);
    CDisplaySettings::GetInstance().AddResolutionInfo(res);

    // check if resolution match current mode
    if(resDesktop.iWidth == res.iWidth &&
       resDesktop.iHeight == res.iHeight &&
       resDesktop.iScreenWidth == res.iScreenWidth &&
       resDesktop.iScreenHeight == res.iScreenHeight &&
       (resDesktop.dwFlags & D3DPRESENTFLAG_MODEMASK) == (res.dwFlags & D3DPRESENTFLAG_MODEMASK) &&
       fabs(resDesktop.fRefreshRate - res.fRefreshRate) < FLT_EPSILON)
    {
      // update desktop resolution
      CDisplaySettings::GetInstance().GetResolutionInfo(RES_DESKTOP) = res;
    }
  }
}

void CWinSystemAmlogic::UpdateResolutions()
{
  CWinSystemBase::UpdateResolutions();

  RefreshResolutions();
}

void CWinSystemAmlogic::RefreshDisplayCapabilities()
{
  m_amlDisplay->aml_refresh_display_caps();

  // disabledolbyvision, sdr2dv and hdr2dv are chained by enable dependencies,
  // keep them device-keyed so a disabled row always has its cause on screen
  const bool device_dv = aml_support_dolby_vision();
  const bool sink_dv = device_dv && m_amlDisplay->aml_display_support_dv();

  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  auto setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE);
  if (setting)
    setting->SetVisible(device_dv);

  setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_SDR2DV);
  if (setting)
    setting->SetVisible(device_dv);

  setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_HDR2DV);
  if (setting)
    setting->SetVisible(device_dv);

  setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_DV_LED);
  if (setting)
    setting->SetVisible(sink_dv);

  setting = settings->GetSetting(CSettings::SETTING_VIDEOPLAYER_DOVIZEROLEVEL5);
  if (setting)
    setting->SetVisible(sink_dv);

  if (IsHDRDisplay())
  {
    CServiceBroker::GetSettingsComponent()
        ->GetSettings()
        ->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR)
        ->SetVisible(true);

    int sdr2hdr = CServiceBroker::GetSettingsComponent()
        ->GetSettings()
        ->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR);
    if (sdr2hdr)
    {
      CLog::Log(LOGDEBUG, "CWinSystemAmlogic::{} -- setting sdr2hdr mode to {:d}", __FUNCTION__, sdr2hdr);
      CSysfsPath("/sys/module/aml_media/parameters/sdr_mode", sdr2hdr);
      CSysfsPath("/sys/module/aml_media/parameters/dolby_vision_policy", 0);
      CSysfsPath("/sys/module/aml_media/parameters/hdr_policy", 0);
    }

    CServiceBroker::GetSettingsComponent()
        ->GetSettings()
        ->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR)
        ->SetVisible(true);

    int hdr2sdr = CServiceBroker::GetSettingsComponent()
        ->GetSettings()
        ->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR);
    if (hdr2sdr)
    {
      CLog::Log(LOGDEBUG, "CWinSystemAmlogic::{} -- setting hdr2sdr mode to {:d}", __FUNCTION__, hdr2sdr);
      CSysfsPath("/sys/module/aml_media/parameters/hdr_mode", hdr2sdr);
    }
  }
  else
  {
    setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR);
    if (setting)
      setting->SetVisible(false);

    setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR);
    if (setting)
      setting->SetVisible(false);
  }
}

bool CWinSystemAmlogic::IsHDRDisplay()
{
  CHDRCapabilities caps = m_amlDisplay->GetHDRCaps();
  return (caps.SupportsHDR10() | caps.SupportsHDR10Plus() | caps.SupportsHLG() |
         (caps.SupportsDolbyVision() != DolbyVisionFormat::DOLBYVISION_TYPE_NONE));
}

CHDRCapabilities CWinSystemAmlogic::GetDisplayHDRCapabilities() const
{
  return m_amlDisplay->GetHDRCaps();
}

float CWinSystemAmlogic::GetGuiSdrPeakLuminance() const
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const int guiSdrPeak = settings->GetInt(CSettings::SETTING_VIDEOSCREEN_GUISDRPEAKLUMINANCE);

  return ((0.7f * guiSdrPeak + 30.0f) / 100.0f);
}

HDR_STATUS CWinSystemAmlogic::GetOSHDRStatus()
{
  return (IsHDRDisplay() ? HDR_STATUS::HDR_ON : HDR_STATUS::HDR_UNSUPPORTED);
}

float CWinSystemAmlogic::GetDisplayLatency()
{
  return CAMLLatencyStore::GetInstance().Get();
}

void CWinSystemAmlogic::RegisterRenderThread()
{
  m_renderThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
}

float CWinSystemAmlogic::GetFrameLatencyAdjustment()
{
  // Read where it is used. PrepareNextRender() runs earlier in the pass than the
  // renderer, so a value sampled there is a pass old: out by however long the
  // pass takes, and wrapping a whole refresh when that approaches one, which
  // costs a frame. Only the decode thread, which arrives through
  // CDVDVideoCodecAmlogic::RenderDisplayLatency() and has no relationship to the
  // vblank, is served the cached one rather than taking a DRM read of its own.
  if (std::this_thread::get_id() == m_renderThread.load(std::memory_order_relaxed))
    return SampleFrameLatency();

  return m_frameLatencyMs.load(std::memory_order_relaxed);
}

float CWinSystemAmlogic::SampleFrameLatency()
{
  // Nothing steady to report. Zero is what the base class returns, so the
  // renderer simply schedules without the term rather than with a stale one.
  const auto stand_down = [this]() {
    m_frameLatencyMs.store(0.0f, std::memory_order_relaxed);
    m_vblankPhaseMs.store(0.0f, std::memory_order_relaxed);
    return 0.0f;
  };

  const double rate = GetGfxContext().GetFPS();
  if (rate <= 0.0)
    return stand_down();

  // Trick play and pause have the player driving the clock.
  if (CServiceBroker::GetDataCacheCore().GetSpeed() != 1.0f)
    return stand_down();

  // Stand down while CRenderManager centres the frame itself, or this would add
  // a second half period on top of its.
  if (CServiceBroker::GetDataCacheCore().IsRenderClockSync())
    return stand_down();

  // Whole-number match only, so pulldown, where there is no fixed phase, is left
  // alone. Compared with a tolerance: the two rates come from different sources
  // and exact equality on a ratio of floats does not hold.
  const double refreshes =
      aml_refreshes_per_frame(rate, CServiceBroker::GetDataCacheCore().GetVideoFps());
  if (refreshes <= 0.0 || std::abs(std::round(refreshes) - refreshes) > 0.0005)
    return stand_down();

  const double periodUs = 1000000.0 / rate;
  const std::optional<double> sinceFrameStart = aml_since_frame_start_us(2.0 * periodUs);
  if (!sinceFrameStart)
    return stand_down();

  // Time since the LAST vblank, which is what PrepareNextRender() subtracts from
  // a clock read of its own - and it does not take that difference modulo the
  // period, so the fold is what keeps it one. Inside the blanking interval the
  // kernel extrapolates to the frame about to start, so the reading comes back a
  // little negative and refers to the next vblank rather than the last; adding a
  // period puts it back. A record a whole period stale still says where in the
  // frame the display is, so fold that too rather than refuse it. Only a reading
  // with nothing sensible left in it is turned away.
  double phase = *sinceFrameStart;
  if (phase >= periodUs)
  {
    // Said once, because there is no known mechanism for it: the kernel
    // recomputes the vblank from the scanout position on every call, so it
    // should not be able to lag a whole period. If this never appears the
    // branch can go, and refusing the reading would then cost nothing.
    if (!m_staleVblankSeen)
    {
      m_staleVblankSeen = true;
      CLog::Log(LOGDEBUG, LOGVIDEO, "CWinSystemAmlogic: vblank record a period stale ({:.2f}ms)",
                *sinceFrameStart / 1000.0);
    }
    phase -= periodUs;
  }
  if (phase < 0.0)
    phase += periodUs;

  if (phase < 0.0 || phase >= periodUs)
    return stand_down();

  // Half a period of lead, so renderPts >= pts settles half a frame before the
  // display switches rather than on the flip. Only while the display shows each
  // frame once: with two or more refreshes per frame PrepareNextRender() already
  // flips early on the vblanks with no frame due.
  const double lead = std::round(refreshes) == 1.0 ? periodUs / 2.0 : 0.0;

  const float adjustment = static_cast<float>((phase - lead) / 1000.0);
  m_frameLatencyMs.store(adjustment, std::memory_order_relaxed);
  m_vblankPhaseMs.store(static_cast<float>(phase / 1000.0), std::memory_order_relaxed);

  return adjustment;
}

DEBUG_INFO_RENDER CWinSystemAmlogic::GetDebugInfo()
{
  const float pipeline = CAMLLatencyStore::GetInstance().Get();

  DEBUG_INFO_RENDER info;
  info.videoOutput = StringUtils::Format(
      "AML pipeline: {} | align: {:+.2f}ms | vblank phase: {:+.2f}ms",
      pipeline < 0.0f ? std::string("measuring") : StringUtils::Format("{:.1f}ms", pipeline),
      CAMLLatencyStore::GetInstance().GetAlignment(),
      m_vblankPhaseMs.load(std::memory_order_relaxed));
  return info;
}

void CWinSystemAmlogic::Register(IDispResource *resource)
{
  std::unique_lock<CCriticalSection> lock(m_resourceSection);
  m_resources.push_back(resource);
}

void CWinSystemAmlogic::Unregister(IDispResource *resource)
{
  std::unique_lock<CCriticalSection> lock(m_resourceSection);
  std::vector<IDispResource*>::iterator i = find(m_resources.begin(), m_resources.end(), resource);
  if (i != m_resources.end())
    m_resources.erase(i);
}
