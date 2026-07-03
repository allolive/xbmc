/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "AMLHdr10PlusToDv.h"
#include "cores/VideoPlayer/Interface/StreamInfo.h"

#include <cstdint>
#include <vector>

class CDVDStreamInfo;
class CBitstreamConverter;
class CHDRCapabilities;

//! \brief Turning a stream's HDR10+ dynamic metadata into Dolby Vision profile
//! 8.1, kept out of the codec that drives it.
//!
//! The codec reaches this through a handful of calls and keeps its own state:
//! m_stripHdr10Plus belongs to CoreELEC and stays there, passed in by reference
//! where arming has to change it. Nothing here takes over anything the codec
//! already owned, so this file can grow without moving lines upstream may move.
class CAMLHdr10PlusHook
{
public:
  //! \brief Decide whether this stream converts, and configure the bitstream
  //! converter to match. Clears \p strip when the SEI is consumed rather than
  //! discarded. Called once, from the codec's Open().
  void Arm(CDVDStreamInfo& hints,
           CBitstreamConverter* bitstream,
           const CHDRCapabilities& caps,
           bool& strip);

  //! \brief Rewrite one assembled access unit so it carries a generated RPU in
  //! place of its HDR10+ SEI. \p data and \p size are replaced when it does.
  void RewriteAu(uint8_t*& data, uint32_t& size);

  //! \brief Settle how the decoder opens: Dolby Vision when an RPU was
  //! produced, native HDR10 when arming did not pay off - in which case the
  //! converter is disarmed with it and \p strip restored.
  void OnDecoderOpen(CDVDStreamInfo& hints,
                     StreamHdrType& bufferHdrType,
                     bool& isHdr10Plus,
                     CBitstreamConverter* bitstream,
                     bool& strip);

  //! \brief Whether an RPU was written into the access unit just handled. The
  //! native HDR10+ side channel is suppressed on that, never on intent alone:
  //! a stream armed but not converted would otherwise lose both.
  bool ConvertedThisAu() const { return m_session.ConvertedThisAu(); }

  //! \brief The decoder refused the access unit and will be re-offered it, so
  //! give back the cache key generation already committed. Forcing a refresh
  //! would invent a scene cut on every mid-shot stall.
  void RollbackAu() { m_session.RollbackAu(); }

  //! \brief A seek breaks the shot: the next RPU carries scene_refresh_flag=1
  //! so the display re-anchors rather than smoothing on. Cached RPUs are kept.
  void OnFlush() { m_session.Reset(); }

private:
  KODI::AML::HDR::CHdr10PlusToDvSession m_session;
  std::vector<uint8_t> m_au;
  bool m_convert{false};
  bool m_sinkLacksHdr10Plus{false};
};
