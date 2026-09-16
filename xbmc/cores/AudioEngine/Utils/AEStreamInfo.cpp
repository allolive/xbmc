/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AEStreamInfo.h"

#include "utils/log.h"

#include <algorithm>
#include <string.h>

#define DTS_PREAMBLE_14BE 0x1FFFE800
#define DTS_PREAMBLE_14LE 0xFF1F00E8
#define DTS_PREAMBLE_16BE 0x7FFE8001
#define DTS_PREAMBLE_16LE 0xFE7F0180
#define DTS_PREAMBLE_HD 0x64582025
#define DTS_PREAMBLE_XCH 0x5a5a5a5a
#define DTS_PREAMBLE_XXCH 0x47004a03
#define DTS_PREAMBLE_X96K 0x1d95f262
#define DTS_PREAMBLE_XBR 0x655e315e
#define DTS_PREAMBLE_LBR 0x0a801921
#define DTS_PREAMBLE_XLL 0x41a29547
#define DTS_SFREQ_COUNT 16
#define MAX_EAC3_BLOCKS 6
#define UNKNOWN_DTS_EXTENSION 255

static const uint16_t AC3Bitrates[] = {32,  40,  48,  56,  64,  80,  96,  112, 128, 160,
                                       192, 224, 256, 320, 384, 448, 512, 576, 640};
static const uint16_t AC3FSCod[] = {48000, 44100, 32000, 0};
static const uint8_t AC3BlkCod[] = {1, 2, 3, 6};
static const uint8_t AC3Channels[] = {2, 1, 2, 3, 3, 4, 4, 5};
static const uint8_t DTSChannels[] = {1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 6, 6, 6, 7, 8, 8};
static const uint8_t THDChanMap[] = {2, 1, 1, 2, 2, 2, 2, 1, 1, 2, 2, 1, 1};

static const uint32_t DTSSampleRates[DTS_SFREQ_COUNT] = {0,     8000,  16000, 32000, 64000,  128000,
                                                         11025, 22050, 44100, 88200, 176400, 12000,
                                                         24000, 48000, 96000, 192000};

CAEStreamParser::CAEStreamParser() : m_syncFunc(&CAEStreamParser::DetectType)
{
  av_crc_init(m_crcTrueHD, 0, 16, 0x2D, sizeof(m_crcTrueHD));
}

double CAEStreamInfo::GetDuration() const
{
  double duration = 0;
  switch (m_type)
  {
    case STREAM_TYPE_AC3:
      duration = 1536.0 / m_sampleRate;
      break;
    case STREAM_TYPE_EAC3:
      duration = 6144.0 / m_sampleRate / 4;
      break;
    case STREAM_TYPE_TRUEHD:
      int rate;
      if (m_sampleRate == 48000 || m_sampleRate == 96000 || m_sampleRate == 192000)
        rate = 192000;
      else
        rate = 176400;
      duration = 3840.0 / rate;
      break;
    case STREAM_TYPE_DTS_512:
    case STREAM_TYPE_DTSHD_CORE:
    case STREAM_TYPE_DTSHD:
    case STREAM_TYPE_DTSHD_MA:
      duration = 512.0 / m_sampleRate;
      break;
    case STREAM_TYPE_DTS_1024:
      duration = 1024.0 / m_sampleRate;
      break;
    case STREAM_TYPE_DTS_2048:
      duration = 2048.0 / m_sampleRate;
      break;
    default:
      CLog::Log(LOGERROR, "CAEStreamInfo::GetDuration - invalid stream type");
      break;
  }
  return duration * 1000;
}

bool CAEStreamInfo::operator==(const CAEStreamInfo& info) const
{
  if (m_type != info.m_type)
    return false;
  if (m_dataIsLE != info.m_dataIsLE)
    return false;
  if (m_repeat != info.m_repeat)
    return false;
  return true;
}

void CAEStreamParser::Reset()
{
  m_skipBytes = 0;
  m_bufferSize = 0;
  m_needBytes = 0;
  m_hasSync = false;
}

int CAEStreamParser::AddData(uint8_t* data,
                             unsigned int size,
                             uint8_t** buffer,
                             unsigned int* bufferSize)
{
  if (size == 0)
  {
    if (bufferSize)
      *bufferSize = 0;
    return 0;
  }

  if (m_skipBytes)
  {
    unsigned int canSkip = std::min(size, m_skipBytes);
    unsigned int room = sizeof(m_buffer) - m_bufferSize;
    unsigned int copy = std::min(room, canSkip);

    memcpy(m_buffer + m_bufferSize, data, copy);
    m_bufferSize += copy;
    m_skipBytes -= copy;

    if (m_skipBytes)
    {
      if (bufferSize)
        *bufferSize = 0;
      return copy;
    }

    GetPacket(buffer, bufferSize);
    return copy;
  }
  else
  {
    unsigned int consumed = 0;
    unsigned int offset = 0;
    unsigned int room = sizeof(m_buffer) - m_bufferSize;
    while (true)
    {
      if (!size)
      {
        if (bufferSize)
          *bufferSize = 0;
        return consumed;
      }

      unsigned int copy = std::min(room, size);
      memcpy(m_buffer + m_bufferSize, data, copy);
      m_bufferSize += copy;
      consumed += copy;
      data += copy;
      size -= copy;
      room -= copy;

      if (m_needBytes > m_bufferSize)
        continue;

      m_needBytes = 0;
      offset = (this->*m_syncFunc)(m_buffer, m_bufferSize);

      if (m_hasSync)
        break;
      else
      {
        // lost sync
        m_syncFunc = &CAEStreamParser::DetectType;
        m_info.m_type = CAEStreamInfo::STREAM_TYPE_NULL;
        m_info.m_repeat = 1;

        // if the buffer is full, or the offset < the buffer size
        if (m_bufferSize == sizeof(m_buffer) || offset < m_bufferSize)
        {
          m_bufferSize -= offset;
          room += offset;
          memmove(m_buffer, m_buffer + offset, m_bufferSize);
        }
      }
    }

    // if we got here, we acquired sync on the buffer

    // align the buffer
    if (offset)
    {
      m_bufferSize -= offset;
      memmove(m_buffer, m_buffer + offset, m_bufferSize);
    }

    // bytes to skip until the next packet
    m_skipBytes = std::max(0, (int)m_fsize - (int)m_bufferSize);
    if (m_skipBytes)
    {
      if (bufferSize)
        *bufferSize = 0;
      return consumed;
    }

    if (!m_needBytes)
      GetPacket(buffer, bufferSize);
    else if (bufferSize)
      *bufferSize = 0;

    return consumed;
  }
}

void CAEStreamParser::GetPacket(uint8_t** buffer, unsigned int* bufferSize)
{
  // if the caller wants the packet
  if (buffer)
  {
    // if it is dtsHD and we only want the core, just fetch that
    unsigned int size = m_fsize;
    if (m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_CORE)
      size = m_coreSize;

    if (m_defeatAC3DialNorm && (m_info.m_type == CAEStreamInfo::STREAM_TYPE_AC3 ||
                                m_info.m_type == CAEStreamInfo::STREAM_TYPE_EAC3))
      DefeatAC3DialNorm(m_buffer, size);

    if (m_defeatDTSDialNorm && (m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTS_512 ||
                                m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTS_1024 ||
                                m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTS_2048 ||
                                m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD ||
                                m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_CORE ||
                                m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_MA))
      DefeatDTSDialNorm(m_buffer, size);

    // make sure the buffer is allocated and big enough
    if (!*buffer || !bufferSize || *bufferSize < size)
    {
      delete[] * buffer;
      *buffer = new uint8_t[size];
    }

    // copy the data into the buffer and update the size
    memcpy(*buffer, m_buffer, size);
    if (bufferSize)
      *bufferSize = size;
  }

  // remove the parsed data from the buffer
  m_bufferSize -= m_fsize;
  memmove(m_buffer, m_buffer + m_fsize, m_bufferSize);
  m_fsize = 0;
  m_coreSize = 0;
}

// SYNC FUNCTIONS

// This function looks for sync words across the types in parallel, and only does an exhaustive
// test if it finds a syncword. Once sync has been established, the relevant sync function sets
// m_syncFunc to itself. This function will only be called again if total sync is lost, which
// allows is to switch stream types on the fly much like a real receiver does.
unsigned int CAEStreamParser::DetectType(uint8_t* data, unsigned int size)
{
  unsigned int skipped = 0;
  unsigned int possible = 0;

  while (size > 8)
  {
    // if it could be DTS
    unsigned int header = data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
    if (header == DTS_PREAMBLE_14LE || header == DTS_PREAMBLE_14BE || header == DTS_PREAMBLE_16LE ||
        header == DTS_PREAMBLE_16BE)
    {
      unsigned int skip = SyncDTS(data, size);
      if (m_hasSync || m_needBytes)
        return skipped + skip;
      else
        possible = skipped;
    }

    // if it could be AC3
    if (data[0] == 0x0b && data[1] == 0x77)
    {
      unsigned int skip = SyncAC3(data, size);
      if (m_hasSync || m_needBytes)
        return skipped + skip;
      else
        possible = skipped;
    }

    // if it could be TrueHD
    if (data[4] == 0xf8 && data[5] == 0x72 && data[6] == 0x6f && data[7] == 0xba)
    {
      unsigned int skip = SyncTrueHD(data, size);
      if (m_hasSync)
        return skipped + skip;
      else
        possible = skipped;
    }

    // move along one byte
    --size;
    ++skipped;
    ++data;
  }

  return possible ? possible : skipped;
}

// AC-3 CRC helpers, as in FFmpeg libavcodec/ac3enc.c
#define AC3_CRC16_POLY ((1 << 0) | (1 << 2) | (1 << 15) | (1 << 16))

static unsigned int AC3MulPoly(unsigned int a, unsigned int b, unsigned int poly)
{
  unsigned int c = 0;
  while (a)
  {
    if (a & 1)
      c ^= b;
    a >>= 1;
    b <<= 1;
    if (b & (1 << 16))
      b ^= poly;
  }
  return c;
}

static unsigned int AC3PowPoly(unsigned int a, unsigned int n, unsigned int poly)
{
  unsigned int r = 1;
  while (n)
  {
    if (n & 1)
      r = AC3MulPoly(r, a, poly);
    a = AC3MulPoly(a, a, poly);
    n >>= 1;
  }
  return r;
}

static inline uint16_t AC3Bswap16(uint16_t x)
{
  return static_cast<uint16_t>((x >> 8) | (x << 8));
}

// E-AC-3 BSI up to addbsi, per FFmpeg ac3_parser.c. True when the
// extension_type_a (JOC) flag is set or the header cannot be parsed.
static bool EAC3HasJOC(const uint8_t* frame, unsigned int frameBytes)
{
  const unsigned int limit = (frameBytes - 2) * 8;
  unsigned int pos = 16;
  bool overrun = false;
  auto read = [&](unsigned int n) -> uint32_t
  {
    uint32_t value = 0;
    if (pos + n > limit)
    {
      overrun = true;
      return 0;
    }
    while (n--)
    {
      value = (value << 1) | ((frame[pos >> 3] >> (7 - (pos & 7))) & 1);
      ++pos;
    }
    return value;
  };
  auto skip = [&](unsigned int n)
  {
    pos += n;
    if (pos > limit)
      overrun = true;
  };

  const unsigned int strmtyp = read(2);
  skip(3 + 11); // substreamid, frmsiz
  const unsigned int fscod = read(2);
  unsigned int numblks = 6;
  if (fscod == 3)
    skip(2); // fscod2
  else
    numblks = AC3BlkCod[read(2)];
  const unsigned int acmod = read(3);
  const unsigned int lfeon = read(1);
  skip(5); // bsid
  for (unsigned int i = 0; i < (acmod ? 1u : 2u); i++)
  {
    skip(5); // dialnorm
    if (read(1))
      skip(8); // compr
  }
  if (strmtyp == 1 && read(1))
    skip(16); // chanmap
  if (read(1)) // mixmdate
  {
    if (acmod > 2)
    {
      skip(2); // dmixmod
      if (acmod & 1)
        skip(6); // ltrtcmixlev, lorocmixlev
      if (acmod & 4)
        skip(6); // ltrtsurmixlev, lorosurmixlev
    }
    if (lfeon && read(1))
      skip(5); // lfemixlevcod
    if (strmtyp == 0)
    {
      for (unsigned int i = 0; i < (acmod ? 1u : 2u); i++)
        if (read(1))
          skip(6); // pgmscl
      if (read(1))
        skip(6); // extpgmscl
      switch (read(2)) // mixdef
      {
        case 1:
          skip(5);
          break;
        case 2:
          skip(12);
          break;
        case 3:
          skip((read(5) + 2) * 8);
          break;
      }
      if (acmod < 2)
        for (unsigned int i = 0; i < (acmod ? 1u : 2u); i++)
          if (read(1))
            skip(14); // paninfo
      if (read(1)) // frmmixcfginfoe
      {
        for (unsigned int i = 0; i < numblks; i++)
          if (numblks == 1 || read(1))
            skip(5); // blkmixcfginfo
      }
    }
  }
  if (read(1)) // infomdate
  {
    skip(3 + 1 + 1); // bsmod, copyrightb, origbs
    if (acmod == 2)
      skip(4); // dsurmod, dheadphonmod
    if (acmod >= 6)
      skip(2); // dsurexmod
    for (unsigned int i = 0; i < (acmod ? 1u : 2u); i++)
      if (read(1))
        skip(8); // mixlevel, roomtyp, adconvtyp
    if (fscod != 3)
      skip(1); // sourcefscod
  }
  if (strmtyp == 0 && numblks != 6)
    skip(1); // convsync
  if (strmtyp == 2 && (numblks == 6 || read(1)))
    skip(6); // frmsizecod
  if (read(1)) // addbsie
  {
    skip(6 + 7); // addbsil, first addbsi bits
    if (read(1))
      return true;
  }
  return overrun;
}

void CAEStreamParser::DefeatAC3DialNorm(uint8_t* data, unsigned int size)
{
  const AVCRC* crcTable = av_crc_get_table(AV_CRC_16_ANSI);
  unsigned int offset = 0;

  while (offset + 8 <= size)
  {
    if (data[offset] != 0x0b || data[offset + 1] != 0x77)
      break;

    uint8_t* frame = data + offset;
    uint8_t bsid = frame[5] >> 3;
    unsigned int frameBytes = 0;

    if (bsid <= 10)
    {
      uint8_t fscod = frame[4] >> 6;
      uint8_t frmsizecod = frame[4] & 0x3F;
      if (fscod >= 3 || frmsizecod > 37)
        break;

      unsigned int bitRate = AC3Bitrates[frmsizecod >> 1];
      unsigned int framewords = 0;
      switch (fscod)
      {
        case 0:
          framewords = bitRate * 2;
          break;
        case 1:
          framewords = (320 * bitRate / 147 + (frmsizecod & 1 ? 1 : 0));
          break;
        case 2:
          framewords = bitRate * 4;
          break;
      }
      frameBytes = framewords * 2;
      if (offset + frameBytes > size)
        break;

      // dialnorm follows acmod, the optional mix levels / dsurmod and lfeon
      uint8_t acmod = frame[6] >> 5;
      unsigned int shift = 15;
      if ((acmod & 0x1) && (acmod != 0x1))
        shift -= 2;
      if (acmod & 0x4)
        shift -= 2;
      if (acmod == 0x2)
        shift -= 2;

      // leave frames with a bad CRC untouched
      uint32_t bits = (frame[6] << 16) | (frame[7] << 8) | frame[8];
      if (((bits >> shift) & 0x1F) != 31 &&
          av_crc(crcTable, 0, frame + 2, frameBytes - 2) == 0)
      {
        bits |= 0x1FU << shift;
        frame[6] = (bits >> 16) & 0xFF;
        frame[7] = (bits >> 8) & 0xFF;
        frame[8] = bits & 0xFF;

        // crc1 brings the CRC back to zero at 5/8, so crc2 stays valid
        unsigned int frameSize58 = ((frameBytes >> 2) + (frameBytes >> 4)) << 1;
        uint16_t crc1 = AC3Bswap16(av_crc(crcTable, 0, frame + 4, frameSize58 - 4));
        unsigned int crcInv =
            AC3PowPoly((AC3_CRC16_POLY >> 1), (8 * frameSize58) - 16, AC3_CRC16_POLY);
        crc1 = static_cast<uint16_t>(AC3MulPoly(crcInv, crc1, AC3_CRC16_POLY));
        frame[2] = (crc1 >> 8) & 0xFF;
        frame[3] = crc1 & 0xFF;
      }
    }
    else if (bsid <= 16)
    {
      uint8_t strmtyp = frame[2] >> 6;
      frameBytes = ((((frame[2] & 0x7) << 8) | frame[3]) + 1) * 2;
      if (offset + frameBytes > size)
        break;

      // leave dependent substreams and DD+ Atmos (JOC) alone
      uint8_t dialnorm = ((frame[5] & 0x07) << 2) | (frame[6] >> 6);
      if (strmtyp != 1 && frameBytes >= 10 && dialnorm != 31 &&
          !EAC3HasJOC(frame, frameBytes))
      {
        const uint8_t delta[2] = {static_cast<uint8_t>(~frame[5] & 0x07),
                                  static_cast<uint8_t>(~frame[6] & 0xC0)};
        frame[5] |= 0x07;
        frame[6] |= 0xC0;

        // CRC is linear: update the stored crc2 by the CRC of the changed bits
        uint32_t dcrc = av_crc(crcTable, 0, delta, 2);
        for (unsigned int i = 0; i < frameBytes - 9; i++)
          dcrc = crcTable[static_cast<uint8_t>(dcrc)] ^ (dcrc >> 8);

        uint16_t crc2 = ((frame[frameBytes - 2] << 8) | frame[frameBytes - 1]) ^
                        AC3Bswap16(static_cast<uint16_t>(dcrc));
        if (crc2 == 0x0B77)
        {
          frame[frameBytes - 3] ^= 0x1;
          crc2 ^= 0x8005;
        }
        frame[frameBytes - 2] = (crc2 >> 8) & 0xFF;
        frame[frameBytes - 1] = crc2 & 0xFF;
      }
    }
    else
      break;

    offset += frameBytes;
  }
}

static uint32_t DTSReadBits(const uint8_t* data, unsigned int& pos, unsigned int n)
{
  uint32_t value = 0;
  while (n--)
  {
    value = (value << 1) | ((data[pos >> 3] >> (7 - (pos & 7))) & 1);
    ++pos;
  }
  return value;
}

static void DTSWriteBits(uint8_t* data, unsigned int pos, unsigned int n, uint32_t value)
{
  for (unsigned int i = 0; i < n; ++i)
  {
    const uint8_t mask = 1 << (7 - ((pos + i) & 7));
    if ((value >> (n - 1 - i)) & 1)
      data[(pos + i) >> 3] |= mask;
    else
      data[(pos + i) >> 3] &= ~mask;
  }
}

static unsigned int DTSPopCount(uint32_t x)
{
  unsigned int count = 0;
  for (; x; x >>= 1)
    count += x & 1;
  return count;
}

// CRC-16-CCITT (poly 0x1021, init 0xFFFF), the EXSS header CRC
static uint16_t DTSCrc16(const uint8_t* data, unsigned int len)
{
  uint16_t crc = 0xFFFF;
  for (unsigned int i = 0; i < len; ++i)
  {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int b = 0; b < 8; ++b)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

// DTS dialnorm is attenuation in dB, so 0 defeats it. Only the 16-bit BE core
// is handled; layout per ETSI TS 102 114 and FFmpeg dca_exss.c.
void CAEStreamParser::DefeatDTSDialNorm(uint8_t* data, unsigned int size)
{
  if (size < 16)
    return;

  const uint32_t sync = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
  if (sync != DTS_PREAMBLE_16BE)
    return;

  // core DNG means attenuation for VERNUM 6/7; skip when there is a header CRC
  unsigned int pos = 32;
  DTSReadBits(data, pos, 1 + 5); // FTYPE, SHORT
  const unsigned int cpf = DTSReadBits(data, pos, 1);
  DTSReadBits(data, pos, 7); // NBLKS
  const unsigned int fsize = DTSReadBits(data, pos, 14) + 1;
  DTSReadBits(data, pos, 6 + 4 + 5); // AMODE, SFREQ, RATE
  DTSReadBits(data, pos, 1 + 1 + 1 + 1 + 1); // MIX, DYNF, TIMEF, AUXF, HDCD
  DTSReadBits(data, pos, 3 + 1 + 1 + 2 + 1); // EXT_AUDIO_ID, EXT_AUDIO, ASPF, LFF, HFLAG
  if (cpf)
    DTSReadBits(data, pos, 16); // HCRC
  DTSReadBits(data, pos, 1); // FILTS
  const unsigned int vernum = DTSReadBits(data, pos, 4);
  DTSReadBits(data, pos, 2 + 3 + 1 + 1); // CHIST, PCMR, SUMF, SUMS
  const unsigned int dngPos = pos;
  if (cpf == 0 && (vernum == 6 || vernum == 7) && DTSReadBits(data, pos, 4) != 0)
    DTSWriteBits(data, dngPos, 4, 0);

  // extension substream: nuDialNormCode of the first asset
  if (fsize + 16 > size)
    return;
  uint8_t* exss = data + fsize;
  const uint32_t exssSync = (exss[0] << 24) | (exss[1] << 16) | (exss[2] << 8) | exss[3];
  if (exssSync != DTS_PREAMBLE_HD)
    return;

  unsigned int ep = 32;
  DTSReadBits(exss, ep, 8); // nuUserDefinedBits
  const unsigned int extSSIndex = DTSReadBits(exss, ep, 2);
  const unsigned int wideHeader = DTSReadBits(exss, ep, 1);
  const unsigned int fsizeBits = wideHeader ? 20 : 16;
  const unsigned int headerSize = DTSReadBits(exss, ep, wideHeader ? 12 : 8) + 1;
  if (headerSize < 7 || fsize + headerSize > size)
    return;

  // only touch a header whose CRC validates
  if (DTSCrc16(exss + 5, headerSize - 5) != 0)
    return;
  // a valid CRC does not make the layout sane: never read past the header
  const unsigned int limit = (headerSize - 2) * 8;
  auto read = [&](unsigned int n) -> uint32_t {
    if (ep + n > limit)
    {
      ep = limit + 1;
      return 0;
    }
    return DTSReadBits(exss, ep, n);
  };

  read(fsizeBits); // nuExtSSFsize
  const unsigned int staticFields = read(1);
  unsigned int numAssets = 1;
  if (staticFields)
  {
    read(2 + 3); // nuRefClockCode, nuExSSFrameDurationCode
    if (read(1))
      read(32 + 4); // nuTimeStamp, nLSB
    const unsigned int numPresentations = read(3) + 1;
    numAssets = read(3) + 1;
    uint32_t masks[8] = {};
    for (unsigned int i = 0; i < numPresentations; ++i)
      masks[i] = read(extSSIndex + 1);
    for (unsigned int i = 0; i < numPresentations; ++i)
      read(DTSPopCount(masks[i]) * 8); // nuActiveAssetMask
    if (read(1)) // bMixMetadataEnbl
      return;
  }
  for (unsigned int i = 0; i < numAssets; ++i)
    read(fsizeBits); // nuAssetFsize

  read(9 + 3); // nuAssetDescriptFsize, nuAssetIndex
  if (staticFields)
  {
    if (read(1))
      read(4); // nuAssetTypeDescriptor
    if (read(1))
      read(24); // LanguageDescriptor
    if (read(1))
    {
      const unsigned int textBytes = read(10) + 1;
      ep = std::min(ep + textBytes * 8, limit + 1); // InfoTextString
    }
    read(5 + 4); // nuBitResolution, nuMaxSampleRate
    const unsigned int numChannels = read(8) + 1;
    if (read(1)) // bOne2OneMapChannels2Speakers
    {
      if (numChannels > 2)
        read(1); // bEmbeddedStereoFlag
      if (numChannels > 6)
        read(1); // bEmbeddedSixChFlag
      unsigned int maskBits = 0;
      if (read(1)) // bSpkrMaskEnabled
      {
        maskBits = (read(2) + 1) << 2;
        read(maskBits); // nuSpkrActivityMask
      }
      const unsigned int remapSets = read(3);
      if (remapSets && !maskBits)
        return;
      uint32_t layouts[8] = {};
      for (unsigned int i = 0; i < remapSets; ++i)
        layouts[i] = read(maskBits);
      for (unsigned int i = 0; i < remapSets; ++i)
      {
        const unsigned int decChannels = read(5) + 1;
        // some mask bits stand for a speaker pair
        const unsigned int speakers =
            DTSPopCount((layouts[i] & 0xffff) | ((layouts[i] & 0xae66) << 16));
        for (unsigned int c = 0; c < speakers; ++c)
          read(DTSPopCount(read(decChannels)) * 5);
      }
    }
    else
      read(3); // nuRepresentationType
  }
  if (read(1)) // bDRCCoefPresent
    read(8);
  if (!read(1)) // bDialNormPresent
    return;

  const unsigned int dnPos = ep;
  if (dnPos + 5 > limit || read(5) == 0)
    return;

  DTSWriteBits(exss, dnPos, 5, 0);
  const uint16_t crc = DTSCrc16(exss + 5, headerSize - 2 - 5);
  exss[headerSize - 2] = (crc >> 8) & 0xFF;
  exss[headerSize - 1] = crc & 0xFF;
}

bool CAEStreamParser::TrySyncAC3(uint8_t* data,
                                 unsigned int size,
                                 bool resyncing,
                                 bool wantEAC3dependent)
{
  if (size < 8)
    return false;

  // look for an ac3 sync word
  if (data[0] != 0x0b || data[1] != 0x77)
    return false;

  uint8_t bsid = data[5] >> 3;
  uint8_t acmod = data[6] >> 5;
  uint8_t lfeon;

  int8_t pos = 4;
  if ((acmod & 0x1) && (acmod != 0x1))
    pos -= 2;
  if (acmod & 0x4)
    pos -= 2;
  if (acmod == 0x2)
    pos -= 2;
  if (pos < 0)
    lfeon = (data[7] & 0x64) ? 1 : 0;
  else
    lfeon = ((data[6] >> pos) & 0x1) ? 1 : 0;

  if (bsid > 0x11 || acmod > 7)
    return false;

  if (bsid <= 10)
  {
    // Normal AC-3

    if (wantEAC3dependent)
      return false;

    uint8_t fscod = data[4] >> 6;
    uint8_t frmsizecod = data[4] & 0x3F;
    if (fscod == 3 || frmsizecod > 37)
      return false;

    // get the details we need to check crc1 and framesize
    unsigned int bitRate = AC3Bitrates[frmsizecod >> 1];
    unsigned int framesize = 0;
    switch (fscod)
    {
      case 0:
        framesize = bitRate * 2;
        break;
      case 1:
        framesize = (320 * bitRate / 147 + (frmsizecod & 1 ? 1 : 0));
        break;
      case 2:
        framesize = bitRate * 4;
        break;
    }

    m_fsize = framesize << 1;
    m_info.m_sampleRate = AC3FSCod[fscod];

    // dont do extensive testing if we have not lost sync
    if (m_info.m_type == CAEStreamInfo::STREAM_TYPE_AC3 && !resyncing)
      return true;

    // this may be the main stream of EAC3
    unsigned int fsizeMain = m_fsize;
    unsigned int reqBytes = fsizeMain + 8;
    if (size < reqBytes)
    {
      // not enough data to check for E-AC3 dependent frame, request more
      m_needBytes = reqBytes;
      m_fsize = 0;
      // no need to resync => return true
      return true;
    }
    m_info.m_frameSize = fsizeMain;
    if (TrySyncAC3(data + fsizeMain, size - fsizeMain, resyncing, true))
    {
      // concatenate the main and dependent frames
      m_fsize += fsizeMain;
      return true;
    }

    unsigned int crc_size;
    // if we have enough data, validate the entire packet, else try to validate crc2 (5/8 of the packet)
    if (framesize <= size)
      crc_size = framesize - 1;
    else
      crc_size = (framesize >> 1) + (framesize >> 3) - 1;

    if (crc_size <= size)
      if (av_crc(av_crc_get_table(AV_CRC_16_ANSI), 0, &data[2], crc_size * 2))
        return false;

    // if we get here, we can sync
    m_hasSync = true;
    m_info.m_channels = AC3Channels[acmod] + lfeon;
    m_syncFunc = &CAEStreamParser::SyncAC3;
    m_info.m_type = CAEStreamInfo::STREAM_TYPE_AC3;
    m_info.m_frameSize += m_fsize;
    m_info.m_repeat = 1;

    CLog::Log(LOGINFO, "CAEStreamParser::TrySyncAC3 - AC3 stream detected ({} channels, {}Hz)",
              m_info.m_channels, m_info.m_sampleRate);
    return true;
  }
  else
  {
    // Enhanced AC-3
    uint8_t strmtyp = data[2] >> 6;
    if (strmtyp == 3)
      return false;

    if (strmtyp != 1 && wantEAC3dependent)
      return false;

    unsigned int framesize = (((data[2] & 0x7) << 8) | data[3]) + 1;
    uint8_t fscod = (data[4] >> 6) & 0x3;
    uint8_t cod = (data[4] >> 4) & 0x3;
    uint8_t acmod = (data[4] >> 1) & 0x7;
    uint8_t lfeon = data[4] & 0x1;
    uint8_t blocks;

    if (fscod == 0x3)
    {
      if (cod == 0x3)
        return false;

      blocks = 6;
      m_info.m_sampleRate = AC3FSCod[cod] >> 1;
    }
    else
    {
      blocks = AC3BlkCod[cod];
      m_info.m_sampleRate = AC3FSCod[fscod];
    }

    m_fsize = framesize << 1;
    m_info.m_repeat = MAX_EAC3_BLOCKS / blocks;

    // EAC3 can have a dependent stream too
    if (!wantEAC3dependent)
    {
      unsigned int fsizeMain = m_fsize;
      unsigned int reqBytes = fsizeMain + 8;
      if (size < reqBytes)
      {
        // not enough data to check for E-AC3 dependent frame, request more
        m_needBytes = reqBytes;
        m_fsize = 0;
        // no need to resync => return true
        return true;
      }
      m_info.m_frameSize = fsizeMain;
      if (TrySyncAC3(data + fsizeMain, size - fsizeMain, resyncing, true))
      {
        // concatenate the main and dependent frames
        m_fsize += fsizeMain;
        return true;
      }
    }

    if (m_info.m_type == CAEStreamInfo::STREAM_TYPE_EAC3 && m_hasSync && !resyncing)
      return true;

    // if we get here, we can sync
    m_hasSync = true;
    m_info.m_channels = AC3Channels[acmod] + lfeon;
    m_syncFunc = &CAEStreamParser::SyncAC3;
    m_info.m_type = CAEStreamInfo::STREAM_TYPE_EAC3;
    m_info.m_frameSize += m_fsize;

    CLog::Log(LOGINFO, "CAEStreamParser::TrySyncAC3 - E-AC3 stream detected ({} channels, {}Hz)",
              m_info.m_channels, m_info.m_sampleRate);
    return true;
  }
}

unsigned int CAEStreamParser::SyncAC3(uint8_t* data, unsigned int size)
{
  unsigned int skip = 0;

  for (; size - skip > 7; ++skip, ++data)
  {
    bool resyncing = (skip != 0);
    if (TrySyncAC3(data, size - skip, resyncing, false))
      return skip;
  }

  // if we get here, the entire packet is invalid and we have lost sync
  CLog::Log(LOGINFO, "CAEStreamParser::SyncAC3 - AC3 sync lost");
  m_hasSync = false;
  return skip;
}

unsigned int CAEStreamParser::SyncDTS(uint8_t* data, unsigned int size)
{
  if (size < 13)
  {
    if (m_needBytes < 13)
      m_needBytes = 14;
    return 0;
  }

  unsigned int skip = 0;
  for (; size - skip > 13; ++skip, ++data)
  {
    unsigned int header = data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
    unsigned int hd_sync = 0;
    unsigned int dtsBlocks;
    unsigned int amode;
    unsigned int sfreq;
    unsigned int target_rate;
    unsigned int extension = 0;
    unsigned int ext_type = UNKNOWN_DTS_EXTENSION;
    unsigned int lfe;
    int bits;

    switch (header)
    {
      // 14bit BE
      case DTS_PREAMBLE_14BE:
        if (data[4] != 0x07 || (data[5] & 0xf0) != 0xf0)
          continue;
        dtsBlocks = (((data[5] & 0x7) << 4) | ((data[6] & 0x3C) >> 2)) + 1;
        m_fsize = (((((data[6] & 0x3) << 8) | data[7]) << 4) | ((data[8] & 0x3C) >> 2)) + 1;
        amode = ((data[8] & 0x3) << 4) | ((data[9] & 0xF0) >> 4);
        target_rate = ((data[10] & 0x3e) >> 1);
        extension = ((data[11] & 0x1));
        ext_type = ((data[11] & 0xe) >> 1);
        sfreq = data[9] & 0xF;
        lfe = (data[12] & 0x18) >> 3;
        m_info.m_dataIsLE = false;
        bits = 14;
        break;

      // 14bit LE
      case DTS_PREAMBLE_14LE:
        if (data[5] != 0x07 || (data[4] & 0xf0) != 0xf0)
          continue;
        dtsBlocks = (((data[4] & 0x7) << 4) | ((data[7] & 0x3C) >> 2)) + 1;
        m_fsize = (((((data[7] & 0x3) << 8) | data[6]) << 4) | ((data[9] & 0x3C) >> 2)) + 1;
        amode = ((data[9] & 0x3) << 4) | ((data[8] & 0xF0) >> 4);
        target_rate = ((data[11] & 0x3e) >> 1);
        extension = ((data[10] & 0x1));
        ext_type = ((data[10] & 0xe) >> 1);
        sfreq = data[8] & 0xF;
        lfe = (data[13] & 0x18) >> 3;
        m_info.m_dataIsLE = true;
        bits = 14;
        break;

      // 16bit BE
      case DTS_PREAMBLE_16BE:
        dtsBlocks = (((data[4] & 0x1) << 7) | ((data[5] & 0xFC) >> 2)) + 1;
        m_fsize = (((((data[5] & 0x3) << 8) | data[6]) << 4) | ((data[7] & 0xF0) >> 4)) + 1;
        amode = ((data[7] & 0x0F) << 2) | ((data[8] & 0xC0) >> 6);
        sfreq = (data[8] & 0x3C) >> 2;
        target_rate = ((data[8] & 0x03) << 3) | ((data[9] & 0xe0) >> 5);
        extension = (data[10] & 0x10) >> 4;
        ext_type = (data[10] & 0xe0) >> 5;
        lfe = (data[10] >> 1) & 0x3;
        m_info.m_dataIsLE = false;
        bits = 16;
        break;

      // 16bit LE
      case DTS_PREAMBLE_16LE:
        dtsBlocks = (((data[5] & 0x1) << 7) | ((data[4] & 0xFC) >> 2)) + 1;
        m_fsize = (((((data[4] & 0x3) << 8) | data[7]) << 4) | ((data[6] & 0xF0) >> 4)) + 1;
        amode = ((data[6] & 0x0F) << 2) | ((data[9] & 0xC0) >> 6);
        sfreq = (data[9] & 0x3C) >> 2;
        target_rate = ((data[9] & 0x03) << 3) | ((data[8] & 0xe0) >> 5);
        extension = (data[11] & 0x10) >> 4;
        ext_type = (data[11] & 0xe0) >> 5;
        lfe = (data[11] >> 1) & 0x3;
        m_info.m_dataIsLE = true;
        bits = 16;
        break;

      default:
        continue;
    }

    if (sfreq == 0 || sfreq >= DTS_SFREQ_COUNT)
      continue;

    // make sure the framesize is sane
    if (m_fsize < 96 || m_fsize > 16384)
      continue;

    CAEStreamInfo::DataType dataType{CAEStreamInfo::STREAM_TYPE_NULL};
    switch (dtsBlocks << 5)
    {
      case 512:
        dataType = CAEStreamInfo::STREAM_TYPE_DTS_512;
        break;
      case 1024:
        dataType = CAEStreamInfo::STREAM_TYPE_DTS_1024;
        break;
      case 2048:
        dataType = CAEStreamInfo::STREAM_TYPE_DTS_2048;
        break;
    }

    if (dataType == CAEStreamInfo::STREAM_TYPE_NULL)
      continue;

    // adjust the fsize for 14 bit streams
    if (bits == 14)
      m_fsize = m_fsize / 14 * 16;

    // we need enough data to check for DTS-HD
    if (size - skip < m_fsize + 10)
    {
      // we can assume DTS sync at this point
      m_syncFunc = &CAEStreamParser::SyncDTS;
      m_needBytes = m_fsize + 10;
      m_fsize = 0;

      return skip;
    }

    // look for DTS-HD
    hd_sync = (data[m_fsize] << 24) | (data[m_fsize + 1] << 16) | (data[m_fsize + 2] << 8) |
              data[m_fsize + 3];
    if (hd_sync == DTS_PREAMBLE_HD)
    {
      int hd_size;
      bool blownup = (data[m_fsize + 5] & 0x20) != 0;
      if (blownup)
        hd_size = (((data[m_fsize + 6] & 0x01) << 19) | (data[m_fsize + 7] << 11) |
                   (data[m_fsize + 8] << 3) | ((data[m_fsize + 9] & 0xe0) >> 5)) +
                  1;
      else
        hd_size = (((data[m_fsize + 6] & 0x1f) << 11) | (data[m_fsize + 7] << 3) |
                   ((data[m_fsize + 8] & 0xe0) >> 5)) +
                  1;

      int header_size;
      if (blownup)
        header_size = (((data[m_fsize + 5] & 0x1f) << 7) | ((data[m_fsize + 6] & 0xfe) >> 1)) + 1;
      else
        header_size = (((data[m_fsize + 5] & 0x1f) << 3) | ((data[m_fsize + 6] & 0xe0) >> 5)) + 1;

      hd_sync = data[m_fsize + header_size] << 24 | data[m_fsize + header_size + 1] << 16 |
                data[m_fsize + header_size + 2] << 8 | data[m_fsize + header_size + 3];

      // set the type according to core or not
      if (m_coreOnly)
        dataType = CAEStreamInfo::STREAM_TYPE_DTSHD_CORE;
      else if (hd_sync == DTS_PREAMBLE_XLL)
        dataType = CAEStreamInfo::STREAM_TYPE_DTSHD_MA;
      else if (hd_sync == DTS_PREAMBLE_XCH || hd_sync == DTS_PREAMBLE_XXCH ||
               hd_sync == DTS_PREAMBLE_X96K || hd_sync == DTS_PREAMBLE_XBR ||
               hd_sync == DTS_PREAMBLE_LBR)
        dataType = CAEStreamInfo::STREAM_TYPE_DTSHD;
      else
        dataType = m_info.m_type;

      m_coreSize = m_fsize;
      m_fsize += hd_size;
    }

    unsigned int sampleRate = DTSSampleRates[sfreq];
    if (!m_hasSync || skip || dataType != m_info.m_type || sampleRate != m_info.m_sampleRate ||
        dtsBlocks != m_dtsBlocks)
    {
      m_hasSync = true;
      m_info.m_type = dataType;
      m_info.m_sampleRate = sampleRate;
      m_dtsBlocks = dtsBlocks;
      m_info.m_channels = DTSChannels[amode] + (lfe ? 1 : 0);
      m_syncFunc = &CAEStreamParser::SyncDTS;
      m_info.m_frameSize = m_fsize;
      m_info.m_repeat = 1;

      if (dataType == CAEStreamInfo::STREAM_TYPE_DTSHD_MA)
      {
        m_info.m_channels += 2; // FIXME: this needs to be read out, not sure how to do that yet
        m_info.m_dtsPeriod = (192000 * (8 >> 1)) * (m_dtsBlocks << 5) / m_info.m_sampleRate;
      }
      else if (dataType == CAEStreamInfo::STREAM_TYPE_DTSHD)
      {
        m_info.m_dtsPeriod = (192000 * (2 >> 1)) * (m_dtsBlocks << 5) / m_info.m_sampleRate;
      }
      else
      {
        m_info.m_dtsPeriod =
            (m_info.m_sampleRate * (2 >> 1)) * (m_dtsBlocks << 5) / m_info.m_sampleRate;
      }

      std::string type;
      switch (dataType)
      {
        case CAEStreamInfo::STREAM_TYPE_DTSHD:
          type = "dtsHD";
          break;
        case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
          type = "dtsHD MA";
          break;
        case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
          type = "dtsHD (core)";
          break;
        default:
          type = "dts";
          break;
      }

      if (extension)
      {
        switch (ext_type)
        {
          case 0:
            type += " XCH";
            break;
          case 2:
            type += " X96";
            break;
          case 6:
            type += " XXCH";
            break;
          default:
            type += " ext unknown";
            break;
        }
      }

      CLog::Log(LOGINFO,
                "CAEStreamParser::SyncDTS - {} stream detected ({} channels, {}Hz, {}bit {}, "
                "period: {}, syncword: 0x{:x}, target rate: 0x{:x}, framesize {}))",
                type, m_info.m_channels, m_info.m_sampleRate, bits, m_info.m_dataIsLE ? "LE" : "BE",
                m_info.m_dtsPeriod, hd_sync, target_rate, m_fsize);
    }

    return skip;
  }

  // lost sync
  CLog::Log(LOGINFO, "CAEStreamParser::SyncDTS - DTS sync lost");
  m_hasSync = false;
  return skip;
}

inline unsigned int CAEStreamParser::GetTrueHDChannels(const uint16_t chanmap)
{
  int channels = 0;
  for (int i = 0; i < 13; ++i)
    channels += THDChanMap[i] * ((chanmap >> i) & 1);
  return channels;
}

unsigned int CAEStreamParser::SyncTrueHD(uint8_t* data, unsigned int size)
{
  unsigned int left = size;
  unsigned int skip = 0;

  // if MLP
  for (; left; ++skip, ++data, --left)
  {
    // if we dont have sync and there is less the 8 bytes, then break out
    if (!m_hasSync && left < 8)
      return size;

    // if its a major audio unit
    uint16_t length = ((data[0] & 0x0F) << 8 | data[1]) << 1;
    uint32_t syncword = ((((data[4] << 8 | data[5]) << 8) | data[6]) << 8) | data[7];
    if (syncword == 0xf8726fba)
    {
      // we need 32 bytes to sync on a master audio unit
      if (left < 32)
        return skip;

      // get the rate and ensure its valid
      int rate = (data[8] & 0xf0) >> 4;
      if (rate == 0xF)
        continue;

      unsigned int major_sync_size = 28;
      if (data[29] & 1)
      {
        // extension(s) present, look up count
        int extension_count = data[30] >> 4;
        major_sync_size += 2 + extension_count * 2;
      }

      if (left < 4 + major_sync_size)
        return skip;

      // verify the crc of the audio unit
      uint16_t crc = av_crc(m_crcTrueHD, 0, data + 4, major_sync_size - 4);
      crc ^= (data[4 + major_sync_size - 3] << 8) | data[4 + major_sync_size - 4];
      if (((data[4 + major_sync_size - 1] << 8) | data[4 + major_sync_size - 2]) != crc)
        continue;

      // 16ch (Atmos) dialogue_norm in extra_channel_meaning, 31 = 0 dB
      if (m_defeatTrueHDDialNorm && (data[21] & 0x80) && (data[29] & 1) && (data[30] >> 4) &&
          (((data[30] & 0x0F) << 1) | (data[31] >> 7)) != 31)
      {
        data[30] |= 0x0F;
        data[31] |= 0x80;

        crc = av_crc(m_crcTrueHD, 0, data + 4, major_sync_size - 4);
        crc ^= (data[4 + major_sync_size - 3] << 8) | data[4 + major_sync_size - 4];
        data[4 + major_sync_size - 2] = crc & 0xFF;
        data[4 + major_sync_size - 1] = (crc >> 8) & 0xFF;
      }

      // get the sample rate and substreams, we have a valid master audio unit
      m_info.m_sampleRate = (rate & 0x8 ? 44100 : 48000) << (rate & 0x7);
      m_substreams = (data[20] & 0xF0) >> 4;

      // get the number of encoded channels
      uint16_t channel_map = ((data[10] & 0x1F) << 8) | data[11];
      if (!channel_map)
        channel_map = (data[9] << 1) | (data[10] >> 7);
      m_info.m_channels = CAEStreamParser::GetTrueHDChannels(channel_map);

      if (!m_hasSync)
        CLog::Log(LOGINFO,
                  "CAEStreamParser::SyncTrueHD - TrueHD stream detected ({} channels, {}Hz)",
                  m_info.m_channels, m_info.m_sampleRate);

      m_hasSync = true;
      m_fsize = length;
      m_info.m_type = CAEStreamInfo::STREAM_TYPE_TRUEHD;
      m_syncFunc = &CAEStreamParser::SyncTrueHD;
      m_info.m_frameSize = length;
      m_info.m_repeat = 1;
      return skip;
    }
    else
    {
      // we cant sink to a subframe until we have the information from a master audio unit
      if (!m_hasSync)
        continue;

      // if there is not enough data left to verify the packet, just return the skip amount
      if (left < (unsigned int)m_substreams * 4)
        return skip;

      // verify the parity
      int p = 0;
      uint8_t check = 0;
      for (int i = -1; i < m_substreams; ++i)
      {
        check ^= data[p++];
        check ^= data[p++];
        if (i == -1 || data[p - 2] & 0x80)
        {
          check ^= data[p++];
          check ^= data[p++];
        }
      }

      // if the parity nibble does not match
      if ((((check >> 4) ^ check) & 0xF) != 0xF)
      {
        // lost sync
        m_hasSync = false;
        CLog::Log(LOGINFO, "CAEStreamParser::SyncTrueHD - Sync Lost");
        continue;
      }
      else
      {
        m_fsize = length;
        return skip;
      }
    }
  }

  // lost sync
  m_hasSync = false;
  return skip;
}
