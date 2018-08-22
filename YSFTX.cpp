/*
 *   Copyright (C) 2009-2018 by Jonathan Naylor G4KLX
 *   Copyright (C) 2017 by Andy Uribe CA6JAU
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "Config.h"
#include "Globals.h"
#include "YSFTX.h"

#include "YSFDefines.h"

// Generated using rcosdesign(0.2, 8, 10, 'sqrt') in MATLAB
static q15_t RRC_0_2_FILTER[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 850, 592, 219, -234, -720, -1179,
                                -1548, -1769, -1795, -1597, -1172, -544, 237, 1092, 1927, 2637,
                                3120, 3286, 3073, 2454, 1447, 116, -1431, -3043, -4544, -5739,
                                -6442, -6483, -5735, -4121, -1633, 1669, 5651, 10118, 14822,
                                19484, 23810, 27520, 30367, 32156, 32767, 32156, 30367, 27520,
                                23810, 19484, 14822, 10118, 5651, 1669, -1633, -4121, -5735,
                                -6483, -6442, -5739, -4544, -3043, -1431, 116, 1447, 2454,
                                3073, 3286, 3120, 2637, 1927, 1092, 237, -544, -1172, -1597,
                                -1795, -1769, -1548, -1179, -720, -234, 219, 592, 850}; // numTaps = 90, L = 10
const uint16_t RRC_0_2_FILTER_PHASE_LEN = 9U; // phaseLength = numTaps/L

const q15_t YSF_LEVELA_HI =  1893;
const q15_t YSF_LEVELB_HI =  631;
const q15_t YSF_LEVELC_HI = -631;
const q15_t YSF_LEVELD_HI = -1893;

const q15_t YSF_LEVELA_LO =  948;
const q15_t YSF_LEVELB_LO =  316;
const q15_t YSF_LEVELC_LO = -316;
const q15_t YSF_LEVELD_LO = -948;

const uint8_t YSF_START_SYNC = 0x77U;
const uint8_t YSF_END_SYNC   = 0xFFU;
const uint8_t YSF_HANG       = 0x00U;

CYSFTX::CYSFTX() :
m_buffer(TX_BUFFER_LEN),
m_modFilter(),
m_modState(),
m_poBuffer(),
m_poLen(0U),
m_poPtr(0U),
m_txDelay(240U),      // 200ms
m_loDev(false),
m_txHang(4800U),      // 4s
m_txCount(0U)
{
  ::memset(m_modState, 0x00U, 16U * sizeof(q15_t));

  m_modFilter.L           = YSF_RADIO_SYMBOL_LENGTH;
  m_modFilter.phaseLength = RRC_0_2_FILTER_PHASE_LEN;
  m_modFilter.pCoeffs     = RRC_0_2_FILTER;
  m_modFilter.pState      = m_modState;
}


void CYSFTX::process()
{
  if (m_buffer.getData() == 0U && m_poLen == 0U && m_txCount == 0U)
    return;

  // If we have YSF data to transmit, do so.
  if (m_poLen == 0U && m_buffer.getData() > 0U) {
    if (!m_tx) {
      for (uint16_t i = 0U; i < m_txDelay; i++)
        m_poBuffer[m_poLen++] = YSF_START_SYNC;
    } else {
      for (uint8_t i = 0U; i < YSF_FRAME_LENGTH_BYTES; i++) {
        uint8_t c = m_buffer.get();
        m_poBuffer[m_poLen++] = c;
      }
    }

    m_poPtr = 0U;
  }

  if (m_poLen > 0U) {
    // Transmit YSF data.
    uint16_t space = io.getSpace();

    while (space > (4U * YSF_RADIO_SYMBOL_LENGTH)) {
      uint8_t c = m_poBuffer[m_poPtr++];
      writeByte(c);

      // Reduce space and reset the hang timer.
      space -= 4U * YSF_RADIO_SYMBOL_LENGTH;
      if (m_duplex)
        m_txCount = m_txHang;

      if (m_poPtr >= m_poLen) {
        m_poPtr = 0U;
        m_poLen = 0U;
        return;
      }
    }
  } else if (m_txCount > 0U) {
    // Transmit silence until the hang timer has expired.
    uint16_t space = io.getSpace();

    while (space > (4U * YSF_RADIO_SYMBOL_LENGTH)) {
      writeSilence();

      space -= 4U * YSF_RADIO_SYMBOL_LENGTH;
      m_txCount--;

      if (m_txCount == 0U)
        return;
    }
  }
}

uint8_t CYSFTX::writeData(const uint8_t* data, uint8_t length)
{
  if (length != (YSF_FRAME_LENGTH_BYTES + 1U))
    return 4U;

  uint16_t space = m_buffer.getSpace();
  if (space < YSF_FRAME_LENGTH_BYTES)
    return 5U;

  for (uint8_t i = 0U; i < YSF_FRAME_LENGTH_BYTES; i++)
    m_buffer.put(data[i + 1U]);

  return 0U;
}

void CYSFTX::writeByte(uint8_t c)
{
  q15_t inBuffer[4U];
  q15_t outBuffer[YSF_RADIO_SYMBOL_LENGTH * 4U];

  const uint8_t MASK = 0xC0U;

  for (uint8_t i = 0U; i < 4U; i++, c <<= 2) {
    switch (c & MASK) {
      case 0xC0U:
        inBuffer[i] = m_loDev ? YSF_LEVELA_LO : YSF_LEVELA_HI;
        break;
      case 0x80U:
        inBuffer[i] = m_loDev ? YSF_LEVELB_LO : YSF_LEVELB_HI;
        break;
      case 0x00U:
        inBuffer[i] = m_loDev ? YSF_LEVELC_LO : YSF_LEVELC_HI;
        break;
      default:
        inBuffer[i] = m_loDev ? YSF_LEVELD_LO : YSF_LEVELD_HI;
        break;
    }
  }

  ::arm_fir_interpolate_q15(&m_modFilter, inBuffer, outBuffer, 4U);

  io.write(STATE_YSF, outBuffer, YSF_RADIO_SYMBOL_LENGTH * 4U);
}

void CYSFTX::writeSilence()
{
  q15_t inBuffer[4U] = {0x00U, 0x00U, 0x00U, 0x00U};
  q15_t outBuffer[YSF_RADIO_SYMBOL_LENGTH * 4U];

  ::arm_fir_interpolate_q15(&m_modFilter, inBuffer, outBuffer, 4U);

  io.write(STATE_YSF, outBuffer, YSF_RADIO_SYMBOL_LENGTH * 4U);
}

void CYSFTX::setTXDelay(uint8_t delay)
{
  m_txDelay = 600U + uint16_t(delay) * 12U;        // 500ms + tx delay

  if (m_txDelay > 1200U)
    m_txDelay = 1200U;
}

uint8_t CYSFTX::getSpace() const
{
  return m_buffer.getSpace() / YSF_FRAME_LENGTH_BYTES;
}

void CYSFTX::setParams(bool on, uint8_t txHang)
{
  m_loDev  = on;
  m_txHang = txHang * 1200U;
}

