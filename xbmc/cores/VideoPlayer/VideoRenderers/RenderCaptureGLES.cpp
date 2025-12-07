/*
 *  Copyright (C) 2005-2021 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RenderCaptureGLES.h"

CRenderCaptureGLES::~CRenderCaptureGLES()
{
  delete[] m_pixels;
}

void CRenderCaptureGLES::BeginRender()
{
  const size_t requiredSize = static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * 4;
  if (m_bufferSize != requiredSize)
  {
    delete[] m_pixels;
    m_bufferSize = requiredSize;
    m_pixels = new uint8_t[m_bufferSize];
  }
}

void CRenderCaptureGLES::EndRender()
{
  SetState(CAPTURESTATE_DONE);
}
