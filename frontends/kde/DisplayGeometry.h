/*
 * Where the picture goes inside the display, shared by the GPU and the
 * software display.
 *
 * On a television a Master System pixel is wider than tall: 8:7 on NTSC, so
 * the 268x224 frame shows as about 306x224; on PAL it is the 14.75 MHz
 * square-pixel rate over the VDP's 5.32 MHz dot clock, halved because each
 * non-interlaced line is two of the interlaced lines that rate is square
 * against -- about 1.39. Square pixels show the raw buffer.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QRectF>
#include <QSizeF>

#include "smssession.h"

inline QRectF displayRect(QSizeF avail, int lines, bool tvAspect, bool pal)
{
    double par = 1.0;
    if (tvAspect)
        par = pal ? 14.75 / 5.3203424 / 2.0 : 8.0 / 7.0;
    const double want = (double)SMSSESSION_FB_WIDTH * par / (double)lines;
    const double w = avail.width(), h = avail.height();
    double sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    return QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh);
}
