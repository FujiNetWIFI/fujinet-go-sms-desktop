/*
 * DisplayWidget -- see DisplayWidget.h.
 *
 * A QTimer rather than a frame-clock callback: Qt Widgets has no equivalent
 * of GdkFrameClock. The timer runs a little faster than the machine so no
 * frame waits a whole period, and the session's own wall-clock pacing does
 * the real work -- notify_vsync is still fed, so the phase lock engages
 * whenever the ticks happen to be steady.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include <QPainter>
#include <chrono>
#include <cstring>

DisplayWidget::DisplayWidget(smssession *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    m_fb.resize(SMSSESSION_FB_WIDTH * SMSSESSION_FB_MAX_HEIGHT);
    /* 268x224 at the NTSC TV aspect */
    setMinimumSize(306, 224);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);

    connect(&m_timer, &QTimer::timeout, this, &DisplayWidget::tick);
    /* ~120 Hz: comfortably above the machine's 60 so a finished frame is
     * never held back by the poll interval. */
    m_timer.start(8);
}

void DisplayWidget::tick()
{
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    smssession_notify_vsync(
        m_session,
        (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());

    int h = 0;
    if (!smssession_copy_frame(m_session, m_fb.data(), &h, &m_serial))
        return;
    if (h <= 0 || h > SMSSESSION_FB_MAX_HEIGHT) return;

    /* The session's pixels are 0x00RRGGBB, which is exactly
     * QImage::Format_RGB32 (the alpha byte is ignored); a straight copy. */
    if (m_height != h) {
        m_height = h;
        m_image = QImage(SMSSESSION_FB_WIDTH, h, QImage::Format_RGB32);
    }
    std::memcpy(m_image.bits(), m_fb.data(), (size_t)SMSSESSION_FB_WIDTH * h * 4);
    update();
}

void DisplayWidget::setTvAspect(bool tv) { m_tvAspect = tv; update(); }
void DisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }

void DisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_height <= 0) return;

    /* On a television a Master System pixel is wider than tall: 8:7 on
     * NTSC, so the 268x224 frame shows as about 306x224; on PAL it is the
     * 14.75 MHz square-pixel rate over the VDP's 5.32 MHz dot clock, halved
     * because each non-interlaced line is two of the interlaced lines that
     * rate is square against -- about 1.39. Square pixels show the raw
     * buffer. */
    double par = 1.0;
    if (m_tvAspect)
        par = smssession_refresh_rate(m_session) == 50 ? 14.75 / 5.3203424 / 2.0 : 8.0 / 7.0;
    const double want = (double)SMSSESSION_FB_WIDTH * par / (double)m_height;
    double w = width(), h = height(), sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }

    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    p.drawImage(QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh), m_image);
}
