/*
 *  Copyright (C) 2011-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/StreamDetails.h"

#include <optional>

enum AML_SUPPORT_H264_4K2K
{
  AML_SUPPORT_H264_4K2K_UNINIT = -1,
  AML_NO_H264_4K2K,
  AML_HAS_H264_4K2K,
  AML_HAS_H264_4K2K_SAME_PROFILE
};

enum AML_DISPLAY_DV_LED
{
  AML_DV_TV_LED = 0,
  AML_DV_PLAYER_LED
};

#define HDR10_PLUS_CAP      (int)(1<<0)
#define HDR10_CAP           (int)(1<<2)
#define SMPTE_ST_2084_CAP   (int)(1<<3)
#define HLG_CAP             (int)(1<<4)

#define DV_2160p60Hz        (int)(1<<2)
#define DV_RGB_444_8BIT     (int)(1<<3)
#define LL_YCbCr_422_12BIT  (int)(1<<5)

#define AML_GXBB    0x1F
#define AML_GXL     0x21
#define AML_GXM     0x22
#define AML_G12A    0x28
#define AML_G12B    0x29
#define AML_SM1     0x2B
#define AML_SC2     0x32
#define AML_T7      0x36
#define AML_S4      0x37
#define AML_S5      0x3E
#define AML_S7      0x46
#define AML_S7D     0x47
#define AML_S6      0x48

int  aml_get_cpufamily_id();
std::string aml_get_cpufamily_name(int cpuid = -1);
bool aml_support_hevc();
bool aml_support_hevc_4k2k();
bool aml_support_hevc_8k4k();
bool aml_support_hevc_10bit();
bool aml_support_h266();
AML_SUPPORT_H264_4K2K aml_support_h264_4k2k();
bool aml_support_vp9();
bool aml_support_av1();
bool aml_support_avs2();
bool aml_support_avs3();
bool aml_support_dolby_vision();
bool aml_dolby_vision_enabled();
bool aml_convert_to_dv_by_vs_engine(StreamHdrType hdrType);
bool aml_video_started();
int aml_amdv_wait(StreamHdrType hdrType);
void aml_set_3d_video_mode(unsigned int mode, bool framepacking_support, int view_mode);

//! \brief Microseconds since the display began painting the frame on screen,
//! from the DRM vblank timestamp. Empty if DRM declines or the reading is older
//! than maxAgeUs. App thread only.
std::optional<double> aml_since_frame_start_us(double maxAgeUs);

//! \brief Microseconds on CLOCK_MONOTONIC, the base the vblank age is measured
//! against. Not CurrentHostCounter(), which is a different clock entirely.
double aml_monotonic_us();

//! \brief The display's vblank counter and how long ago that vblank was. The
//! counter difference between two readings is an exact whole number of refresh
//! periods - no timebase, no jitter, nothing to fold - and the age is what the
//! frame on screen is measured against.
std::optional<std::pair<uint64_t, double>> aml_vblank_seq_and_age(double maxAgeUs);

//! \brief The timestamp of the frame the driver has on screen, in the 90kHz
//! ticks it stores natively. Kept in ticks so it can be compared for equality:
//! the kernel truncates once when it checks a frame in, so a microsecond round
//! trip does not come back to the same number.
std::optional<uint32_t> aml_displayed_pts_ticks();

//! \brief What CRenderManager adds when it schedules a frame, in master clock
//! units, without the vblank term that only the frame loop can use. See
//! CRenderManager::PrepareNextRender().
double aml_render_display_latency(StreamHdrType hdrType, float audioDelay);
double aml_render_chosen_offset(StreamHdrType hdrType, float audioDelay);

//! \brief How many times the display shows each frame of a stream at fps.
//! Zero if either rate is unknown, or if the content outruns the display.
//! Callers apply their own tolerance; CheckEnableClockSync() uses 0.0005.
double aml_refreshes_per_frame(double displayRate, double fps);
