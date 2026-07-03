/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AMLHdr10PlusHook.h"

#include "DVDStreamInfo.h"
#include "ServiceBroker.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/AMLUtils.h"
#include "utils/BitstreamConverter.h"
#include "utils/HDRCapabilities.h"
#include "utils/log.h"

using namespace KODI::AML::HDR;

void CAMLHdr10PlusHook::Arm(CDVDStreamInfo& hints,
                            CBitstreamConverter* bitstream,
                            const CHDRCapabilities& caps,
                            bool& strip)
{
  m_convert = false;

  // Latched here, at Open: the hotplug handler rewrites the display caps
  // unsynchronised from the app thread, so the decode path must not re-read
  // them once playback is under way.
  m_sinkLacksHdr10Plus = !caps.SupportsHDR10Plus();

  if (!bitstream)
    return;

#if defined(HAVE_LIBDOVI)
    // Convert HDR10+ dynamic metadata to Dolby Vision profile 8.1 RPUs. Only
    // for PQ HEVC on a DV-capable display, and never for streams that carry
    // their own DV metadata. Require a PQ signal (transfer or HDR10/HDR10+
    // flag) so HLG/SDR streams with a stray HDR10+ SEI are not wrapped as DV;
    // accepting the flag as well avoids missing files with no container
    // transfer. (The HDR10PLUS arm is defensive only - nothing in the tree ever
    // assigns that hdrType to a stream; DetermineHdrType yields HDR10 for it.)
    const bool isPq = hints.colorTransferCharacteristic == AVCOL_TRC_SMPTE2084 ||
                      hints.hdrType == StreamHdrType::HDR_TYPE_HDR10 ||
                      hints.hdrType == StreamHdrType::HDR_TYPE_HDR10PLUS;
    // One setting carries both the on/off decision and the content mapping
    // version: 0 off, 1 convert emitting CMv4.0, 2 convert emitting CMv2.9.
    // Read once here so the choice is a setting change, not a rebuild.
    const int hdr10PlusToDv = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
        CSettings::SETTING_COREELEC_AMLOGIC_HDR10PLUS_TO_DV);
    if (hdr10PlusToDv != 0 &&
        hints.codec == AV_CODEC_ID_HEVC && hints.extradata && !hints.cryptoSession &&
        isPq &&
        hints.hdrType != StreamHdrType::HDR_TYPE_DOLBYVISION &&
        // Redundant on the FFmpeg path, where hdrType is set from the same
        // configuration record - but a client demuxer copies hdr_type and dovi
        // across independently (DVDDemuxClient), so an add-on can report
        // profile 8 alongside HDR10 and only this stops it arming. Neither
        // covers a remux that kept in-band RPUs but lost the record: nothing
        // reads the elementary stream this early. See ProcessAccessUnit().
        hints.dovi.dv_profile == 0 &&
        aml_support_dolby_vision() &&
        caps.SupportsDolbyVision() != DolbyVisionFormat::DOLBYVISION_TYPE_NONE &&
        !CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
            CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE) &&
        // The settings dependencies only grey the control out; the stored value
        // survives, so the Dolby Vision master switch and the three VS10 modes
        // that can claim this stream are re-checked here. sdr2dv is not among
        // them: aml_convert_to_dv_by_vs_engine only reads it for
        // hdrType == HDR_TYPE_NONE, which the isPq gate above excludes. The
        // hdr2dv check is redundant today - its only mechanism is the hdrType
        // rewrite in CVideoPlayer::OpenStream, which the hdrType clause above
        // already blocks - and is kept because that coupling lives in
        // cross-platform code this patch does not own. A silent break there
        // would mean VS10 and this both converting the same stream.
        !CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
            CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR) &&
        !CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
            CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR) &&
        !CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
            CSettings::SETTING_COREELEC_AMLOGIC_HDR2DV))
    {
      m_convert = true;
      // CMv2.9 cannot represent an average below 819 (2.43 nits) and dark
      // content sits under it; CMv4.0 carries the remainder in level 3.
      m_session.SetCmMode(hdr10PlusToDv == 1 ? DvCmMode::V40 : DvCmMode::V29);
      bitstream->SetRemoveHdr10Plus(false);
      // the SEI is consumed, not discarded - don't report it as removed
      strip = false;

      // seed the static HDR metadata from the container; the converter
      // refreshes it from MDCV/CLL SEIs in the bitstream, up to and including the
      // access unit that produces the first RPU; static metadata is frozen after
      HDRStaticMetadataInfo metadata;
      if (hints.masteringMetadata)
      {
        // has_primaries and has_luminance are set independently, and ffmpeg
        // copies the side data whole either way - reading luminance without the
        // flag divides 0/0 and casts a NaN.
        if (hints.masteringMetadata->has_luminance)
        {
          metadata.max_lum =
              static_cast<uint32_t>(av_q2d(hints.masteringMetadata->max_luminance) + 0.5);
          metadata.min_lum =
              static_cast<uint32_t>(av_q2d(hints.masteringMetadata->min_luminance) * 10000 + 0.5);
        }
        if (hints.masteringMetadata->has_primaries)
        {
          // ffmpeg orders display_primaries R,G,B; the SEI - and so
          // HDRStaticMetadataInfo - orders them G,B,R (HEVC D.3.27). Remap
          // rather than copy, and scale to the SEI's 0.00002 units. A straight
          // copy would emit level 9 with the primaries permuted; without the seed
          // at all, a stream whose mastering metadata lives only in the container
          // falls back to CMv2.9.
          constexpr int kOurGbrFromFfmpegRgb[3] = {1, 2, 0};
          for (int i = 0; i < 3; ++i)
          {
            const auto& prim =
                hints.masteringMetadata->display_primaries[kOurGbrFromFfmpegRgb[i]];
            metadata.display_primaries_x[i] =
                static_cast<uint16_t>(av_q2d(prim[0]) * 50000 + 0.5);
            metadata.display_primaries_y[i] =
                static_cast<uint16_t>(av_q2d(prim[1]) * 50000 + 0.5);
          }
          metadata.white_point_x = static_cast<uint16_t>(
              av_q2d(hints.masteringMetadata->white_point[0]) * 50000 + 0.5);
          metadata.white_point_y = static_cast<uint16_t>(
              av_q2d(hints.masteringMetadata->white_point[1]) * 50000 + 0.5);
          metadata.has_mdcv = true;
        }
      }
      if (hints.contentLightMetadata)
      {
        metadata.max_cll = hints.contentLightMetadata->MaxCLL;
        metadata.max_fall = hints.contentLightMetadata->MaxFALL;
      }
      m_session.SetStaticMetadata(metadata);

      CLog::Log(LOGINFO, "{}::{} - HDR10+ to Dolby Vision profile 8.1 conversion enabled",
                "CAMLHdr10PlusHook", __FUNCTION__);
    }
#endif

}

void CAMLHdr10PlusHook::RewriteAu(uint8_t*& data, uint32_t& size)
{
  // Runs on the shared converter's output, so nothing cross-platform changes.
  if (m_convert && m_session.ProcessAccessUnit(data, size, m_au))
  {
    data = m_au.data();
    size = static_cast<uint32_t>(m_au.size());
  }
}

void CAMLHdr10PlusHook::OnDecoderOpen(CDVDStreamInfo& hints,
                                      StreamHdrType& bufferHdrType,
                                      bool& isHdr10Plus,
                                      CBitstreamConverter* bitstream,
                                      bool& strip)
{
      // HDR10+ metadata was converted to DV RPUs: open the decoder in Dolby
      // Vision profile 8.1 mode instead of plain HDR10.
      if (m_convert && m_session.Converted())
      {
        CLog::Log(LOGINFO,
                  "CAMLHdr10PlusHook::{}: HDR10+ converted to DV P8.1, opening "
                  "decoder in DV mode",
                  __FUNCTION__);
        hints.hdrType = StreamHdrType::HDR_TYPE_DOLBYVISION;
        hints.dovi.dv_version_major = 1;
        hints.dovi.dv_version_minor = 0;
        hints.dovi.dv_profile = 8;
        hints.dovi.dv_level = 6;
        hints.dovi.rpu_present_flag = 1;
        hints.dovi.el_present_flag = 0;
        hints.dovi.bl_present_flag = 1;
        hints.dovi.dv_bl_signal_compatibility_id = 1;
        // the output is now Dolby Vision: mark the picture as DV so the AML
        // renderer takes its DV path (dv_is_used) matching the actual output
        bufferHdrType = StreamHdrType::HDR_TYPE_DOLBYVISION;
        isHdr10Plus = true; // converted => source was HDR10+; report that, not the DV output
      }
      else if (m_convert)
      {
        // DEGRADED STATE: conversion was armed but no HDR10+ RPU was produced by
        // the first access unit (e.g. HDR10+ carried only as container side data,
        // or an unparseable first SEI). Fall back to native HDR10/HDR10+
        // rather than dropping anything. The decoder is now latched to HDR10 for
        // the whole stream, so the converter has to be disarmed with it: left
        // armed, a later access unit would strip its HDR10+ SEI and inject a DV
        // RPU into a decode session opened as HDR10, and mute the native side
        // channel on the way past, leaving neither an RPU nor HDR10+.
        m_convert = false;
        if (bitstream && m_sinkLacksHdr10Plus)
        {
          bitstream->SetRemoveHdr10Plus(true);
          strip = true;
        }
        CLog::Log(LOGWARNING, "CAMLHdr10PlusHook::{}: HDR10+ to DV conversion armed "
                  "but no RPU generated on first AU - disarmed, playing as native "
                  "HDR10/HDR10+",
                  __FUNCTION__);
      }

}
