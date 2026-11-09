/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * The console as a player sees it: a black case, the Master System's top strip
 * (SEGA and the model name in red, PAUSE and, on the models that have one,
 * RESET or RAPID) and the two Control Pads below, Player 1 on the left. Every
 * part is painted from the session's layout (smssession_controls_layout, in
 * units of a D-pad arm's width), so every frontend shows the same parts in the
 * same places; only the colours live here.
 *
 * Gestures act on mouse press AND release: the console samples its buttons
 * once a frame, so a press present only for the instant of a click would fall
 * between frames. A press on a cross arm holds that direction; a press on the
 * D-pad's disc and a drag holds one or two (smssession_dpad_from_vector);
 * buttons 1 and 2 and the console's own buttons are held while the mouse is
 * down on them. Dragging off a part lets it go. Hiding the window and losing
 * the focus let go of everything the mouse holds.
 *
 * What the console sees held -- host keyboard, gamepads and this window -- is
 * lit in the accent, so the window doubles as an input tester for a new pad.
 * Keys typed with it active go to the console as in the main window.
 *
 * FIXED SIZE: the drawing is a fixed number of pixels per layout unit (the
 * layout has the same extent for every console), Map legends are fitted into
 * their parts, and gamepad names are elided, so neither Map mode, a console
 * change nor a long name resizes the window. A singleton, hidden rather than
 * destroyed, so a half-done remap survives closing it. Timers run only while
 * it is visible.
 *
 * MAP MODE: Map arms a two-step rebind -- click a part (an arm, a button or a
 * console button, or one of the app's actions along the bottom), then press a
 * host key or a gamepad button. Rebinding steals the key from whatever held
 * it (named in the hint). Clear unbinds the picked part. Escape backs out;
 * while Map is armed nothing is typed into the console.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QtMath>

#include <functional>
#include <utility>
#include <vector>

#include "../FujiNetWindows.h"
#include "../KeyForward.h"
#include "../debugger/DebuggerWindow.h"
#include "../help/HelpWindow.h"

namespace {

constexpr qreal kUnit = 38.0;        /* pixels per layout unit (a D-pad arm's width) */
constexpr int kPadX = 16;            /* the case's margin round the controls */
constexpr int kTop = 12;
constexpr int kBottom = 14;
constexpr qreal kDishRadius = 1.55;  /* the D-pad disc, in units */
constexpr int kMaxPadRows = 3;
constexpr int kStripButtonWidth = 188;
constexpr int kStripGap = 10;
constexpr int kStripHeight = 34;

const QColor kInk(0xED, 0xED, 0xED);          /* light text on the case */
const QColor kCaptionInk(0x9A, 0x9A, 0xA0);
const QColor kMarkInk(0x8E, 0x8E, 0x94);
const QColor kLegendInk(0xF4, 0xF4, 0xF4);
const QColor kPlate(0x2E, 0x2E, 0x32);        /* a part's resting face */
const QColor kPlateEdge(0x05, 0x05, 0x06);
const QColor kWhite(0xFF, 0xFF, 0xFF);

QString local(const char *text) { return QString::fromUtf8(text ? text : ""); }

QFont scaledFont(QFont font, qreal scale, bool bold = false)
{
    font.setPointSizeF((font.pointSizeF() > 0 ? font.pointSizeF() : 10) * scale);
    font.setBold(bold);
    return font;
}

/* Lines centred in rect, the font shrunk until they fit. */
void drawFitted(QPainter &pnt, const QRectF &rect, const QString &text, QFont font)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (;;) {
        const QFontMetricsF metrics(font);
        qreal wide = 0;
        for (const QString &line : lines) wide = qMax(wide, metrics.horizontalAdvance(line));
        if ((wide <= rect.width() && metrics.lineSpacing() * lines.size() <= rect.height() + 2)
            || font.pointSizeF() <= 5.0)
            break;
        font.setPointSizeF(font.pointSizeF() - 0.5);
    }
    pnt.setFont(font);
    pnt.drawText(rect, Qt::AlignCenter, text);
}

QString keyName(smssession *session, int target)
{
    const sms_binding bind = smssession_binding_get(session, target);
    char key[48] = "";
    if (bind.keysym) smssession_keysym_name(bind.keysym, key, sizeof key);
    return local(key);
}

/* A key as a legend on a small part: the arrows as glyphs, a long name cut. */
QString keyLegend(smssession *session, int target, int limit)
{
    const QString key = keyName(session, target);
    if (key.isEmpty()) return QStringLiteral("—");
    if (key == QLatin1String("Up")) return QStringLiteral("↑");
    if (key == QLatin1String("Down")) return QStringLiteral("↓");
    if (key == QLatin1String("Left")) return QStringLiteral("←");
    if (key == QLatin1String("Right")) return QStringLiteral("→");
    return key.size() > limit ? key.left(limit - 1) + QStringLiteral("…") : key;
}

/* A part's binding as Map mode shows it: the key over the gamepad button. */
QString bindingText(smssession *session, int target, bool withButton, int limit = 6)
{
    const sms_binding bind = smssession_binding_get(session, target);
    QString text = keyLegend(session, target, limit);
    if (withButton && bind.button != SMS_PAD_BTN_NONE)
        text += QLatin1Char('\n') + local(sms_pad_button_name(bind.button));
    return text;
}

QString bindingTip(smssession *session, int target)
{
    const sms_binding bind = smssession_binding_get(session, target);
    const QString key = keyName(session, target);
    return QStringLiteral("%1\nKey: %2   Gamepad: %3")
        .arg(local(sms_target_name(target)),
             key.isEmpty() ? QStringLiteral("none") : key,
             bind.button != SMS_PAD_BTN_NONE ? local(sms_pad_button_name(bind.button)) : QStringLiteral("none"));
}

const char *const kStripLabel[SMS_SYSACT_COUNT] = { "Reset to CONFIG", "Soft Reset", "Stop in Debugger" };

/* ---- ElidedLabel ------------------------------------------------------------ */

/* A label that never widens the window: one line elides, more wrap into a
 * fixed height. */
class ElidedLabel : public QWidget {
public:
    explicit ElidedLabel(bool dim, int lines = 1) : m_dim(dim), m_lines(lines)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setText(const QString &text)
    {
        if (text == m_text) return;
        m_text = text;
        update();
    }
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * m_lines); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setPen(m_dim ? QColor(Qt::gray) : palette().color(QPalette::WindowText));
        if (m_lines > 1)
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        else
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
    }

private:
    QString m_text;
    bool m_dim;
    int m_lines;
};

/* ---- ConsoleWidget: the console strip and its two control pads ------------------------- */

class ConsoleWidget : public QWidget {
public:
    /* onPick: a part was chosen in Map mode (its target); onHint: a message for the hint line */
    ConsoleWidget(smssession *session, std::function<void(int)> onPick,
                  std::function<void(const QString &)> onHint)
        : m_session(session), m_onPick(std::move(onPick)), m_onHint(std::move(onHint))
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        load();
    }

    /* Lets go of everything the mouse holds (also on focus loss and rebuild). */
    void releaseAll()
    {
        for (int port = 0; port < SMS_PLAYERS; ++port) {
            m_ptrMask[port] = 0;
            smssession_pointer_set(m_session, port, 0);
        }
        if (m_holdTarget >= 0) {
            smssession_press(m_session, m_holdTarget, 0);
            m_holdTarget = -1;
        }
        m_hold = None;
        m_holdIdx = -1;
        update();
    }

    /* -2 idle, -1 armed, >= 0 waiting for a key or pad button for that target */
    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void setShowBindings(bool show)
    {
        m_showBindings = show;
        update();
    }

    /* The console in use differs from the layout drawn (a power cycle with
     * another model): take its layout. The extent is the same for every
     * console, so the window does not resize. */
    void refreshLayout()
    {
        if (smssession_console(m_session) == m_console) return;
        releaseAll();
        load();
        update();
    }

    /* What the console sees held (keyboard, gamepads, the mouse); repaints
     * only on a change. */
    void pollHeld()
    {
        bool changed = false;
        for (int port = 0; port < SMS_PLAYERS; ++port) {
            const unsigned mask = smssession_buttons_held(m_session, port);
            if (mask != m_held[port]) {
                m_held[port] = mask;
                changed = true;
            }
        }
        for (int swnum = 0; swnum < SMS_SW_COUNT; ++swnum) {
            const bool isLit = smssession_switch_held(m_session, swnum) != 0;
            if (isLit != m_swHeld[swnum]) {
                m_swHeld[swnum] = isLit;
                changed = true;
            }
        }
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        paintCase(pnt);
        for (int idx = 0; idx < m_count; ++idx) {
            const sms_control &ctl = m_layout[idx];
            const bool dim = m_map >= 0 && pressable(ctl) && ctl.target != m_map;
            if (dim) pnt.setOpacity(0.4);
            switch (ctl.kind) {
            case SMS_CTL_CONSOLE: paintStrip(pnt, idx); break;
            case SMS_CTL_SWITCH: paintSwitch(pnt, idx); break;
            case SMS_CTL_CAPTION: paintBlock(pnt, idx); break;
            case SMS_CTL_PAD: paintPad(pnt, idx); break;
            case SMS_CTL_DPAD: paintDish(pnt, idx); break;
            case SMS_CTL_DIRECTION: paintArm(pnt, idx); break;
            case SMS_CTL_BUTTON: paintButton(pnt, idx); break;
            default: break;
            }
            if (dim) pnt.setOpacity(1.0);
        }
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        const QPointF pos = evt->position();
        if (m_map != -2) {
            pick(pos);
            return;
        }
        if (m_hold != None) return;          /* one thing at a time */
        const int idx = hitAt(pos);
        if (idx < 0) return;
        const sms_control &ctl = m_layout[idx];
        switch (ctl.kind) {
        case SMS_CTL_SWITCH:
            m_hold = Part;
            m_holdIdx = idx;
            m_holdTarget = ctl.target;
            smssession_press(m_session, ctl.target, 1);
            break;
        case SMS_CTL_DIRECTION:
        case SMS_CTL_BUTTON:
            m_hold = Part;
            m_holdIdx = idx;
            setPointer(ctl.port, ctl.act_mask);
            break;
        case SMS_CTL_DPAD:
            m_hold = Disc;
            m_holdIdx = idx;
            setPointer(ctl.port, discMask(idx, pos));
            break;
        default: return;
        }
        update();
    }

    /* Dragging off a button or an arm lets it go (and back on, holds it
     * again); on the disc it moves from one direction to the next. */
    void mouseMoveEvent(QMouseEvent *evt) override
    {
        const QPointF pos = evt->position();
        if (m_hold == Disc) {
            setPointer(m_layout[m_holdIdx].port, discMask(m_holdIdx, pos));
        } else if (m_hold == Part) {
            const bool over = hitAt(pos) == m_holdIdx;
            const sms_control &ctl = m_layout[m_holdIdx];
            if (ctl.kind == SMS_CTL_SWITCH) {
                if (over && m_holdTarget < 0) {
                    m_holdTarget = ctl.target;
                    smssession_press(m_session, ctl.target, 1);
                } else if (!over && m_holdTarget >= 0) {
                    smssession_press(m_session, m_holdTarget, 0);
                    m_holdTarget = -1;
                }
            } else {
                setPointer(ctl.port, over ? ctl.act_mask : 0);
            }
        } else {
            const int idx = hitAt(pos);
            if (idx != m_hover) {
                m_hover = idx;
                update();
            }
        }
    }

    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        if (m_hold != None) releaseAll();
    }

    void leaveEvent(QEvent *evt) override
    {
        if (m_hover >= 0) {
            m_hover = -1;
            update();
        }
        QWidget::leaveEvent(evt);
    }

    bool event(QEvent *evt) override
    {
        if (evt->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(evt);
            const QString tip = tipAt(help->pos());
            if (tip.isEmpty())
                QToolTip::hideText();
            else
                QToolTip::showText(help->globalPos(), tip, this);
            return true;
        }
        return QWidget::event(evt);
    }

private:
    enum Hold { None, Disc, Part };

    /* one pad's parts, by index into the layout */
    struct Pad {
        int caption = -1, body = -1;
        QRectF block;
    };

    static bool pressable(const sms_control &ctl)
    {
        return ctl.kind == SMS_CTL_SWITCH || ctl.kind == SMS_CTL_DIRECTION || ctl.kind == SMS_CTL_BUTTON;
    }

    void load()
    {
        m_console = smssession_console(m_session);
        float wide = 0, high = 0;
        m_layout = smssession_controls_layout(m_console, &m_count, &wide, &high);
        for (Pad &pad : m_pad) pad = Pad();
        for (int idx = 0; idx < m_count; ++idx) {
            const sms_control &ctl = m_layout[idx];
            if (ctl.port < 0 || ctl.port >= SMS_PLAYERS) continue;
            if (ctl.kind == SMS_CTL_CAPTION) m_pad[ctl.port].caption = idx;
            else if (ctl.kind == SMS_CTL_PAD) m_pad[ctl.port].body = idx;
        }
        for (Pad &pad : m_pad) {
            if (pad.caption < 0 || pad.body < 0) continue;
            pad.block = rectOf(pad.caption).united(rectOf(pad.body)).adjusted(-6, -5, 6, 6);
        }
        const int panelW = qCeil(wide * kUnit) + 2 * kPadX;
        const int panelH = kTop + qCeil(high * kUnit) + kBottom;
        if (size() != QSize(panelW, panelH)) setFixedSize(panelW, panelH);
    }

    QRectF rectOf(int idx) const
    {
        const sms_control &ctl = m_layout[idx];
        return QRectF(kPadX + ctl.left * kUnit, kTop + ctl.top * kUnit, ctl.width * kUnit, ctl.height * kUnit);
    }

    int hitAt(const QPointF &pos) const
    {
        return smssession_controls_hit(m_console, float((pos.x() - kPadX) / kUnit),
                                       float((pos.y() - kTop) / kUnit));
    }

    void setPointer(int port, unsigned mask)
    {
        if (port < 0 || port >= SMS_PLAYERS || m_ptrMask[port] == mask) return;
        m_ptrMask[port] = mask;
        smssession_pointer_set(m_session, port, mask);
    }

    /* What a pointer at pos holds on the disc it began on. */
    unsigned discMask(int idx, const QPointF &pos) const
    {
        const QPointF delta = pos - rectOf(idx).center();
        return smssession_dpad_from_vector(float(delta.x()), float(delta.y()), float(kDishRadius * kUnit));
    }

    /* Map mode: a click on a part picks its target. */
    void pick(const QPointF &pos)
    {
        const int idx = hitAt(pos);
        if (idx < 0) return;
        const sms_control &ctl = m_layout[idx];
        if (ctl.target >= 0)
            m_onPick(ctl.target);
        else if (ctl.kind == SMS_CTL_DPAD)
            m_onHint(QStringLiteral("The disc holds two arms at once: map Up, Down, Left and Right."));
    }

    QString tipAt(const QPoint &pos) const
    {
        const int idx = hitAt(pos);
        if (idx < 0) return QString();
        const sms_control &ctl = m_layout[idx];
        if (ctl.target >= 0) return bindingTip(m_session, ctl.target);
        if (ctl.kind == SMS_CTL_DPAD)
            return QStringLiteral("Press and drag to hold a direction, or two for a diagonal");
        return QString();
    }

    bool mapping() const { return m_map != -2; }
    bool showBind() const { return mapping() || m_showBindings; }

    bool lit(const sms_control &ctl) const
    {
        if (mapping()) return false;
        switch (ctl.kind) {
        case SMS_CTL_SWITCH: return m_swHeld[ctl.target - SMS_TARGET_SWITCH(0)];
        case SMS_CTL_DIRECTION:
        case SMS_CTL_BUTTON: return (m_held[ctl.port] & ctl.act_mask) != 0;
        default: return false;
        }
    }

    bool picked(const sms_control &ctl) const { return m_map >= 0 && ctl.target == m_map; }

    /* ---- painting ---- */

    void paintCase(QPainter &pnt)
    {
        const QRectF body(0.5, 0.5, width() - 1.0, height() - 1.0);
        QLinearGradient shell(0, 0, 0, height());
        shell.setColorAt(0, QColor(0x23, 0x23, 0x26));
        shell.setColorAt(1, QColor(0x13, 0x13, 0x15));
        pnt.setPen(QColor(0x08, 0x08, 0x09));
        pnt.setBrush(shell);
        pnt.drawRoundedRect(body, 14, 14);
        QPainterPath clip;
        clip.addRoundedRect(body, 14, 14);
        pnt.save();
        pnt.setClipPath(clip);
        pnt.setPen(QPen(QColor(0x3C, 0x3C, 0x40), 1.5));
        pnt.drawLine(QPointF(0, 1.5), QPointF(width(), 1.5));
        pnt.restore();
    }

    /* The strip across the top: SEGA, the model in red, an accent stripe. */
    void paintStrip(QPainter &pnt, int idx)
    {
        const sms_control &ctl = m_layout[idx];
        const QRectF rect = rectOf(idx);
        pnt.setPen(QPen(QColor(0x2A, 0x2A, 0x2D), 1));
        pnt.setBrush(QColor(0x0B, 0x0B, 0x0C));
        pnt.drawRoundedRect(rect, 6, 6);
        QPainterPath clip;
        clip.addRoundedRect(rect, 6, 6);
        pnt.save();
        pnt.setClipPath(clip);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(smsAccentColor());
        pnt.drawRect(QRectF(rect.left(), rect.bottom() - 3, rect.width(), 3));
        pnt.restore();

        /* "SEGA" light, the rest in the accent, kept clear of the buttons */
        QString text = local(ctl.legend);
        QString model = text.startsWith(QLatin1String("SEGA ")) ? text.mid(5) : text;
        qreal limit = rect.right();
        for (int swIdx = 0; swIdx < m_count; ++swIdx)
            if (m_layout[swIdx].kind == SMS_CTL_SWITCH) limit = qMin(limit, rectOf(swIdx).left());
        const qreal avail = limit - rect.left() - 28;
        QFont mark = font();
        mark.setBold(true);
        mark.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
        mark.setPixelSize(22);
        qreal sega = 0, gap = 0, wide = 0;
        for (;;) {
            const QFontMetricsF metrics(mark);
            sega = metrics.horizontalAdvance(QStringLiteral("SEGA"));
            gap = metrics.horizontalAdvance(QLatin1Char(' ')) * 1.5;
            wide = sega + gap + metrics.horizontalAdvance(model);
            if (wide <= avail || mark.pixelSize() <= 9) break;
            mark.setPixelSize(mark.pixelSize() - 1);
        }
        const QRectF line(rect.left() + 14, rect.top(), wide + 4, rect.height() - 3);
        pnt.setFont(mark);
        pnt.setPen(kLegendInk);
        pnt.drawText(line, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("SEGA"));
        pnt.setPen(smsAccentColor());
        pnt.drawText(line.adjusted(sega + gap, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, model);
    }

    /* PAUSE and RESET/RAPID: small black caps on the strip. */
    void paintSwitch(QPainter &pnt, int idx)
    {
        const sms_control &ctl = m_layout[idx];
        const bool isLit = lit(ctl);
        const QRectF base = rectOf(idx);
        const qreal drop = isLit ? 2.0 : 0.0;
        const QRectF rect = base.translated(0, drop).adjusted(0, 0, 0, -3);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(QColor(0x07, 0x07, 0x08));
        pnt.drawRoundedRect(base.adjusted(0, 3, 0, 0).translated(0, isLit ? -1 : 0), 4, 4);
        QLinearGradient face(rect.topLeft(), rect.bottomLeft());
        if (isLit) {
            face.setColorAt(0, smsAccentColor().lighter(120));
            face.setColorAt(1, smsAccentColor());
        } else {
            const int boost = idx == m_hover ? 112 : 100;
            face.setColorAt(0, QColor(0x3A, 0x3A, 0x3E).lighter(boost));
            face.setColorAt(1, QColor(0x1C, 0x1C, 0x1F).lighter(boost));
        }
        pnt.setBrush(face);
        pnt.setPen(isLit ? QPen(kWhite, 2) : QPen(kPlateEdge, 1));
        pnt.drawRoundedRect(rect, 4, 4);
        pnt.setPen(kInk);
        if (showBind())
            drawFitted(pnt, rect.adjusted(3, 0, -3, 0), bindingText(m_session, ctl.target, true, 12),
                       scaledFont(font(), 0.75, true));
        else
            drawFitted(pnt, rect, local(ctl.label), scaledFont(font(), 0.95, true));
        if (picked(ctl)) outline(pnt, rect, 4);
    }

    /* "Player N" over the dark recess the pad sits in. */
    void paintBlock(QPainter &pnt, int idx)
    {
        const sms_control &ctl = m_layout[idx];
        const Pad &pad = m_pad[ctl.port];
        pnt.setPen(QPen(QColor(0x0F, 0x0F, 0x10), 1));
        pnt.setBrush(QColor(0x1B, 0x1B, 0x1D));
        pnt.drawRoundedRect(pad.block, 10, 10);
        pnt.setPen(kCaptionInk);
        pnt.setFont(scaledFont(font(), 0.9, true));
        pnt.drawText(rectOf(idx), Qt::AlignCenter, local(ctl.label));
    }

    /* The Control Pad's black body, SEGA printed on it. */
    void paintPad(QPainter &pnt, int idx)
    {
        const QRectF rect = rectOf(idx);
        const qreal radius = 0.7 * kUnit;
        QLinearGradient body(rect.topLeft(), rect.bottomLeft());
        body.setColorAt(0, QColor(0x30, 0x30, 0x33));
        body.setColorAt(1, QColor(0x17, 0x17, 0x19));
        pnt.setPen(QPen(QColor(0x0A, 0x0A, 0x0B), 1));
        pnt.setBrush(body);
        pnt.drawRoundedRect(rect, radius, radius);
        QPainterPath clip;
        clip.addRoundedRect(rect, radius, radius);
        pnt.save();
        pnt.setClipPath(clip);
        pnt.setPen(QPen(QColor(0x50, 0x50, 0x55), 1.5));
        pnt.drawLine(QPointF(rect.left(), rect.top() + 1.5), QPointF(rect.right(), rect.top() + 1.5));
        pnt.restore();
        /* the mark, above the buttons */
        QFont mark = font();
        mark.setPixelSize(SMS_PAD_MARK_PX);
        mark.setBold(true);
        mark.setLetterSpacing(QFont::AbsoluteSpacing, SMS_PAD_MARK_SPACING);
        pnt.setFont(mark);
        pnt.setPen(kMarkInk);
        pnt.drawText(QRectF(rect.left() + SMS_PAD_MARK_LEFT * kUnit, rect.top() + SMS_PAD_MARK_TOP * kUnit,
                            SMS_PAD_MARK_WIDTH * kUnit, SMS_PAD_MARK_HEIGHT * kUnit),
                     Qt::AlignCenter, QStringLiteral("SEGA"));
    }

    /* The D-pad's dish and the cross's centre; the arms are painted after. */
    void paintDish(QPainter &pnt, int idx)
    {
        const sms_control &ctl = m_layout[idx];
        const QRectF rect = rectOf(idx);
        const QPointF centre = rect.center();
        const unsigned dirs = (1u << SMS_ACT_UP) | (1u << SMS_ACT_DOWN) | (1u << SMS_ACT_LEFT) | (1u << SMS_ACT_RIGHT);
        const unsigned held = mapping() ? 0u : (m_held[ctl.port] & dirs);
        const bool diagonal = (held & (held - 1)) != 0;
        pnt.setBrush(QColor(0x0C, 0x0C, 0x0D));
        pnt.setPen(QPen(diagonal ? smsAccentColor() : QColor(0x2B, 0x2B, 0x2E), diagonal ? 2.5 : 1.5));
        pnt.drawEllipse(centre, kDishRadius * kUnit, kDishRadius * kUnit);

        /* the centre cell of the cross, with its dimple and screw hole */
        const QRectF cell(centre.x() - 0.5 * kUnit, centre.y() - 0.5 * kUnit, kUnit, kUnit);
        QLinearGradient face(cell.topLeft(), cell.bottomLeft());
        face.setColorAt(0, QColor(0x2A, 0x2A, 0x2D));
        face.setColorAt(1, QColor(0x15, 0x15, 0x17));
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(face);
        pnt.drawRect(cell);
        pnt.setBrush(QColor(0x0C, 0x0C, 0x0D));
        pnt.drawEllipse(centre, 0.3 * kUnit, 0.3 * kUnit);
        pnt.setBrush(QColor(0x05, 0x05, 0x05));
        pnt.drawEllipse(centre, 0.12 * kUnit, 0.12 * kUnit);
    }

    /* One cross arm: a triangle, or the key bound to it. */
    void paintArm(QPainter &pnt, int idx)
    {
        const sms_control &ctl = m_layout[idx];
        const bool isLit = lit(ctl);
        const QRectF rect = rectOf(idx);
        QLinearGradient face(rect.topLeft(), rect.bottomLeft());
        if (isLit) {
            face.setColorAt(0, smsAccentColor().lighter(120));
            face.setColorAt(1, smsAccentColor());
        } else {
            const int boost = idx == m_hover ? 112 : 100;
            face.setColorAt(0, QColor(0x2A, 0x2A, 0x2D).lighter(boost));
            face.setColorAt(1, QColor(0x15, 0x15, 0x17).lighter(boost));
        }
        pnt.setBrush(face);
        pnt.setPen(isLit ? QPen(kWhite, 2) : QPen(kPlateEdge, 1));
        pnt.drawRoundedRect(rect.adjusted(1, 1, -1, -1), 4, 4);
        if (!isLit) {
            pnt.setPen(QPen(QColor(0x4A, 0x4A, 0x4F), 1));
            pnt.drawLine(QPointF(rect.left() + 5, rect.top() + 2.5), QPointF(rect.right() - 5, rect.top() + 2.5));
        }
        const QColor ink = isLit ? kWhite : QColor(0x77, 0x77, 0x80);
        if (showBind()) {
            pnt.setPen(isLit ? kWhite : kInk);
            drawFitted(pnt, rect.adjusted(3, 3, -3, -3), bindingText(m_session, ctl.target, false),
                       scaledFont(font(), 0.9, true));
        } else {
            const QPointF mid = rect.center();
            const qreal reach = 7.5;
            QPolygonF tri;
            if (ctl.act_mask & (1u << SMS_ACT_UP))
                tri << mid + QPointF(0, -reach) << mid + QPointF(-reach, reach * 0.7) << mid + QPointF(reach, reach * 0.7);
            else if (ctl.act_mask & (1u << SMS_ACT_DOWN))
                tri << mid + QPointF(0, reach) << mid + QPointF(-reach, -reach * 0.7) << mid + QPointF(reach, -reach * 0.7);
            else if (ctl.act_mask & (1u << SMS_ACT_LEFT))
                tri << mid + QPointF(-reach, 0) << mid + QPointF(reach * 0.7, -reach) << mid + QPointF(reach * 0.7, reach);
            else
                tri << mid + QPointF(reach, 0) << mid + QPointF(-reach * 0.7, -reach) << mid + QPointF(-reach * 0.7, reach);
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(ink);
            pnt.drawPolygon(tri);
        }
        if (picked(ctl)) outline(pnt, rect.adjusted(1, 1, -1, -1), 4);
    }

    /* Buttons 1 and 2: black rounded squares that sink when pressed. */
    void paintButton(QPainter &pnt, int idx)
    {
        const sms_control &ctl = m_layout[idx];
        const bool isLit = lit(ctl);
        const QRectF base = rectOf(idx).adjusted(0, 0, 0, -3);
        const qreal radius = 0.28 * kUnit;
        const QRectF rect = base.translated(0, isLit ? 2.0 : 0.0);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(QColor(0x07, 0x07, 0x08));
        pnt.drawRoundedRect(base.adjusted(0, isLit ? 2.5 : 3.5, 0, 3.0), radius, radius);
        QLinearGradient face(rect.topLeft(), rect.bottomLeft());
        if (isLit) {
            face.setColorAt(0, smsAccentColor().lighter(120));
            face.setColorAt(1, smsAccentColor());
        } else {
            const int boost = idx == m_hover ? 112 : 100;
            face.setColorAt(0, QColor(0x2E, 0x2E, 0x32).lighter(boost));
            face.setColorAt(1, QColor(0x15, 0x15, 0x17).lighter(boost));
        }
        pnt.setBrush(face);
        pnt.setPen(isLit ? QPen(kWhite, 2) : QPen(kPlateEdge, 1));
        pnt.drawRoundedRect(rect, radius, radius);
        pnt.setPen(isLit ? kWhite : QColor(0xE6, 0xE6, 0xE6));
        if (showBind())
            drawFitted(pnt, rect.adjusted(2, 0, -2, 0), bindingText(m_session, ctl.target, true),
                       scaledFont(font(), 0.8, true));
        else
            drawFitted(pnt, rect, local(ctl.label), scaledFont(font(), 1.5, true));
        if (picked(ctl)) outline(pnt, rect, radius);
    }

    /* The part Map is waiting on. */
    void outline(QPainter &pnt, const QRectF &rect, qreal radius)
    {
        pnt.setBrush(Qt::NoBrush);
        pnt.setPen(QPen(smsAccentColor(), 3));
        pnt.drawRoundedRect(rect.adjusted(-1, -1, 1, 1), radius, radius);
    }

    smssession *m_session;
    std::function<void(int)> m_onPick;
    std::function<void(const QString &)> m_onHint;
    int m_console = -1;                 /* the console the layout is for */
    const sms_control *m_layout = nullptr;
    int m_count = 0;
    Pad m_pad[SMS_PLAYERS];
    unsigned m_held[SMS_PLAYERS] = {};
    bool m_swHeld[SMS_SW_COUNT] = {};
    unsigned m_ptrMask[SMS_PLAYERS] = {};
    bool m_showBindings = false;
    int m_map = -2;
    int m_hover = -1;
    Hold m_hold = None;
    int m_holdIdx = -1;          /* the part the mouse holds */
    int m_holdTarget = -1;       /* ... and its target, while it is down on a console button */
};

/* ---- StripWidget: the app's own actions --------------------------------------------- */

/* Reset to CONFIG, Soft Reset and Stop in Debugger: always along the bottom.
 * A press fires on release over the button; in Map mode a press picks it. */
class StripWidget : public QWidget {
public:
    StripWidget(smssession *session, std::function<void(int)> onPick)
        : m_session(session), m_onPick(std::move(onPick))
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        setFixedSize(SMS_SYSACT_COUNT * kStripButtonWidth + (SMS_SYSACT_COUNT - 1) * kStripGap, kStripHeight);
    }

    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void setShowBindings(bool show)
    {
        m_showBindings = show;
        update();
    }
    void letGo()
    {
        m_down = -1;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        const bool show = m_map != -2 || m_showBindings;
        for (int sysact = 0; sysact < SMS_SYSACT_COUNT; ++sysact) {
            const int target = SMS_TARGET_SYSACT(sysact);
            const bool isLit = m_map == target || (m_down == sysact && m_hover == sysact);
            const QRectF rect = buttonRect(sysact);
            const bool dim = m_map >= 0 && target != m_map;
            if (dim) pnt.setOpacity(0.4);
            QLinearGradient face(rect.topLeft(), rect.bottomLeft());
            const int boost = m_hover == sysact ? 112 : 100;
            if (isLit) {
                face.setColorAt(0, smsAccentColor().lighter(120));
                face.setColorAt(1, smsAccentColor());
            } else {
                face.setColorAt(0, QColor(0x3A, 0x3A, 0x3E).lighter(boost));
                face.setColorAt(1, QColor(0x1C, 0x1C, 0x1F).lighter(boost));
            }
            pnt.setBrush(face);
            pnt.setPen(isLit ? QPen(kWhite, 2) : QPen(kPlateEdge, 1));
            pnt.drawRoundedRect(rect.adjusted(1, 1, -1, -1), 6, 6);
            pnt.setPen(kInk);
            QString text = QString::fromLatin1(kStripLabel[sysact]);
            if (show) text += QStringLiteral("   ") + bindingText(m_session, target, false, 14);
            drawFitted(pnt, rect.adjusted(8, 0, -8, 0), text, scaledFont(font(), 0.9, true));
            if (dim) pnt.setOpacity(1.0);
        }
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        const int sysact = indexAt(evt->position());
        if (sysact < 0) return;
        if (m_map != -2) {
            m_onPick(SMS_TARGET_SYSACT(sysact));
            return;
        }
        m_down = sysact;
        update();
    }

    void mouseMoveEvent(QMouseEvent *evt) override
    {
        const int sysact = indexAt(evt->position());
        if (sysact != m_hover) {
            m_hover = sysact;
            update();
        }
    }

    /* A system action is posted rather than run here, so the main window
     * takes it as it does a gamepad's: with its status message, and with the
     * debugger window for Debugger Stop. */
    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton || m_down < 0) return;
        const int sysact = m_down;
        m_down = -1;
        if (indexAt(evt->position()) == sysact && m_map == -2) smssession_sysaction_post(m_session, sysact);
        update();
    }

    void leaveEvent(QEvent *evt) override
    {
        m_hover = -1;
        update();
        QWidget::leaveEvent(evt);
    }

    bool event(QEvent *evt) override
    {
        if (evt->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(evt);
            const int sysact = indexAt(help->pos());
            if (sysact < 0)
                QToolTip::hideText();
            else
                QToolTip::showText(help->globalPos(), bindingTip(m_session, SMS_TARGET_SYSACT(sysact)), this);
            return true;
        }
        return QWidget::event(evt);
    }

private:
    QRectF buttonRect(int sysact) const
    {
        return QRectF(sysact * (kStripButtonWidth + kStripGap), 0, kStripButtonWidth, kStripHeight);
    }
    int indexAt(const QPointF &pos) const
    {
        for (int sysact = 0; sysact < SMS_SYSACT_COUNT; ++sysact)
            if (buttonRect(sysact).contains(pos)) return sysact;
        return -1;
    }

    smssession *m_session;
    std::function<void(int)> m_onPick;
    bool m_showBindings = false;
    int m_map = -2;
    int m_hover = -1;
    int m_down = -1;
};

/* ---- ControllersWindow --------------------------------------------------------------- */

class ControllersWindow : public QWidget {
public:
    ControllersWindow(smssession *session, QWidget *parent);
    void present();
    void refresh();

protected:
    void keyPressEvent(QKeyEvent *evt) override;
    void keyReleaseEvent(QKeyEvent *evt) override;
    void showEvent(QShowEvent *evt) override;
    void hideEvent(QHideEvent *evt) override;
    void changeEvent(QEvent *evt) override;
    /* Tab is a key of the console's, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void letGo();
    void setMapState(int state);
    void bound(int target, const QString &what, const char *stolen);
    void pollCapture();
    void pollHeld();
    void pollSlow();
    void rebuildPads();
    void refreshPadPorts();

    smssession *m_session;
    ConsoleWidget *m_console = nullptr;
    StripWidget *m_strip = nullptr;
    QWidget *m_padArea = nullptr;
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;       /* each pad's "drives player N" */
    unsigned m_padGen = ~0u;
    QPushButton *m_mapButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    ElidedLabel *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    uint32_t m_swallow = 0;               /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
     * gamepad button for that target. */
    int m_mapState = -2;
};

ControllersWindow::ControllersWindow(smssession *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel
     * of fixed-size parts and nothing here wants more room. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers"));
    setProperty("smsHelpContext", QStringLiteral("controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its parts and kept there: a resizable panel gets tiled and its
     * parts stretched. */
    root->setSizeConstraint(QLayout::SetFixedSize);

    auto onPick = [this](int target) { setMapState(target); };
    m_console = new ConsoleWidget(session, onPick, [this](const QString &text) { m_hint->setText(text); });
    root->addWidget(m_console, 0, Qt::AlignHCenter);
    m_strip = new StripWidget(session, onPick);
    root->addWidget(m_strip, 0, Qt::AlignHCenter);

    /* Gamepads: one row per pad, rebuilt as they come and go, in room for a
     * fixed number so the window does not change size when one is plugged in */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    auto *padLayout = new QVBoxLayout(pads);
    m_padArea = new QWidget;
    m_padBox = new QVBoxLayout(m_padArea);
    m_padBox->setContentsMargins(0, 0, 0, 0);
    m_padBox->setSpacing(2);
    const int rowHeight = QComboBox().sizeHint().height() + 2;
    m_padArea->setFixedHeight(rowHeight * kMaxPadRows + m_padBox->spacing() * (kMaxPadRows - 1));
    m_padBox->setAlignment(Qt::AlignTop);
    padLayout->addWidget(m_padArea);
    root->addWidget(pads);

    auto *maprow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    m_mapButton->setToolTip(QStringLiteral("Pick a part, then press a key or a gamepad button to give it"));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    m_clearButton = new QPushButton(QStringLiteral("Clear"));
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setToolTip(QStringLiteral("Unbind the picked part's key and gamepad button"));
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (m_mapState < 0) return;
        const int target = m_mapState;
        smssession_binding_set_key(m_session, target, 0, nullptr, 0);
        smssession_binding_set_button(m_session, target, SMS_PAD_BTN_NONE, nullptr, 0);
        setMapState(-1);
        m_hint->setText(QStringLiteral("%1 is now unbound").arg(local(sms_target_name(target))));
    });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    defaults->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    connect(defaults, &QPushButton::clicked, this, [this] {
        smssession_bindings_reset(m_session);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every control is back to its default."));
    });
    auto *bindings = new QCheckBox(QStringLiteral("Show bindings"));
    bindings->setFocusPolicy(Qt::NoFocus);
    bindings->setToolTip(QStringLiteral("Show the key each part has"));
    bindings->setChecked(smssession_get_int(session, "controllers_show_bindings", 0) > 0);
    connect(bindings, &QCheckBox::toggled, this, [this](bool show) {
        smssession_set_int(m_session, "controllers_show_bindings", show ? 1 : 0);
        m_console->setShowBindings(show);
        m_strip->setShowBindings(show);
    });
    m_console->setShowBindings(bindings->isChecked());
    m_strip->setShowBindings(bindings->isChecked());
    maprow->addWidget(m_mapButton);
    maprow->addWidget(m_clearButton);
    maprow->addWidget(defaults);
    maprow->addWidget(bindings);
    maprow->addStretch(1);
    maprow->addWidget(smsHelpButton(QStringLiteral("controllers")));
    root->addLayout(maprow);
    m_hint = new ElidedLabel(true, 2);
    root->addWidget(m_hint);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] { pollHeld(); });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
    letGo();
    pollHeld();
}

/* Lets go of what the mouse holds in this window. */
void ControllersWindow::letGo()
{
    m_console->releaseAll();
    m_strip->letGo();
}

/* ---- Map mode -------------------------------------------------------------------------- */

void ControllersWindow::setMapState(int state)
{
    m_mapState = state;
    m_clearButton->setEnabled(state >= 0);
    if (state == -2) {
        m_captureTimer.stop();
        smssession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_mapButton->setStyleSheet(QString());
        m_hint->setText(QStringLiteral("Press a part with the mouse to hold it. Press and drag on the "
                                       "D-pad's disc for diagonals."));
    } else if (state == -1) {
        m_captureTimer.stop();
        smssession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_mapButton->setStyleSheet(QStringLiteral("background: %1; color: white;").arg(smsAccentColor().name()));
        m_hint->setText(QStringLiteral("Click a part to remap it. Escape leaves Map."));
    } else {
        letGo();
        m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Escape cancels).")
                            .arg(local(sms_target_name(state))));
        smssession_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    m_console->setMap(m_mapState);
    m_strip->setMap(m_mapState);
    pollHeld();
}

/* A binding was made: stay armed, since remapping several in a row is normal. */
void ControllersWindow::bound(int target, const QString &what, const char *stolen)
{
    setMapState(-1);
    m_hint->setText(stolen[0] ? QStringLiteral("%1 is now %2 (taken from %3)")
                                    .arg(local(sms_target_name(target)), what, local(stolen))
                              : QStringLiteral("%1 is now %2").arg(local(sms_target_name(target)), what));
}

void ControllersWindow::pollCapture()
{
    int button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (!smssession_gamepad_capture_poll(m_session, &button)) return;
    char stolen[128] = "";
    const int target = m_mapState;
    smssession_binding_set_button(m_session, target, button, stolen, sizeof stolen);
    bound(target, local(sms_pad_button_name(button)), stolen);
}

/* ---- polling ---------------------------------------------------------------------------- */

/* What the console sees held, from every source, lit (25 Hz while visible). */
void ControllersWindow::pollHeld()
{
    m_console->pollHeld();
}

/* Hot-plugged pads, auto-assignment changes, and a console change. */
void ControllersWindow::pollSlow()
{
    m_console->refreshLayout();
    const unsigned gen = smssession_gamepad_generation(m_session);
    if (gen != m_padGen) {
        m_padGen = gen;
        rebuildPads();
    } else {
        refreshPadPorts();
    }
}

/* ---- gamepads --------------------------------------------------------------------------- */

void ControllersWindow::refreshPadPorts()
{
    for (int idx = 0; idx < (int)m_padEff.size(); ++idx) {
        const int eff = smssession_gamepad_effective_port(m_session, idx);
        const QString text = eff >= 0 ? QStringLiteral("drives player %1").arg(eff + 1)
                                      : QStringLiteral("drives nothing");
        if (m_padEff[(size_t)idx]->text() != text) m_padEff[(size_t)idx]->setText(text);
    }
}

/* One row per pad: its name, the player it drives and the one it is set to. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *row : m_padRows) delete row;
    m_padRows.clear();
    m_padEff.clear();
    const int count = smssession_gamepad_count(m_session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected"));
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int idx = 0; idx < count && idx < kMaxPadRows; ++idx) {
        char name[128];
        smssession_gamepad_name(m_session, idx, name, sizeof name);
        auto *row = new QWidget;
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto *label = new ElidedLabel(false);
        label->setText(local(name));
        label->setToolTip(local(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(QStringLiteral("color: gray;"));
        /* -1 = automatic (connection order), 0-1 = that player */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Player 1"), QStringLiteral("Player 2") });
        const int assign = smssession_gamepad_assignment(m_session, idx);
        box->setCurrentIndex(assign < 0 || assign >= SMS_PLAYERS ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, idx](int sel) {
            smssession_gamepad_assign(m_session, idx, sel <= 0 ? -1 : sel - 1);
            refreshPadPorts();
        });
        line->addWidget(label, 1);
        line->addWidget(eff);
        line->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
        m_padEff.push_back(eff);
    }
    if (count > kMaxPadRows) {
        /* the rest are in Preferences */
        m_padEff.back()->setToolTip(QStringLiteral("%1 more gamepads are listed in Preferences")
                                        .arg(count - kMaxPadRows));
    }
    refreshPadPorts();
}

/* ---- the window ------------------------------------------------------------------------- */

void ControllersWindow::present()
{
    m_padGen = smssession_gamepad_generation(m_session);
    rebuildPads();
    m_console->refreshLayout();
    show();
    raise();
}

void ControllersWindow::refresh()
{
    if (isVisible()) {
        letGo();
        m_console->refreshLayout();
        pollHeld();
    }
}

void ControllersWindow::showEvent(QShowEvent *evt)
{
    pollHeld();
    m_heldTimer.start(40);
    m_slowTimer.start(250);
    QWidget::showEvent(evt);
}

void ControllersWindow::hideEvent(QHideEvent *evt)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    letGo();
    QWidget::hideEvent(evt);
}

/* Only what this window holds: the main window releases its own keys. */
void ControllersWindow::changeEvent(QEvent *evt)
{
    if (evt->type() == QEvent::ActivationChange && !isActiveWindow()) {
        letGo();
        smssession_release_all(m_session);
        m_swallow = 0;
    }
    QWidget::changeEvent(evt);
}

void ControllersWindow::keyPressEvent(QKeyEvent *evt)
{
    if (evt->key() == Qt::Key_F9) {
        if (!evt->isAutoRepeat()) hide();
        return;
    }
    if (evt->key() == Qt::Key_F12) {
        if (!evt->isAutoRepeat()) DebuggerWindow::toggleFor(parentWidget(), m_session);
        return;
    }
    const uint32_t keysym = smsKeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) return;

    if (m_mapState != -2) {
        if (evt->isAutoRepeat()) return;
        /* Escape backs out one step: a pending target, then Map itself */
        if (evt->key() == Qt::Key_Escape) {
            m_swallow = keysym;
            setMapState(m_mapState >= 0 ? -1 : -2);
            return;
        }
        if (m_mapState < 0 || !keysym) return;      /* armed, or nothing to bind: nothing is typed */
        if (smsIsAppKey(evt)) {
            m_hint->setText(QStringLiteral("F9, F11 and F12 are the app's and cannot be bound."));
            return;
        }
        char stolen[128] = "", name[48] = "";
        const int target = m_mapState;
        smssession_binding_set_key(m_session, target, keysym, stolen, sizeof stolen);
        smssession_keysym_name(keysym, name, sizeof name);
        m_swallow = keysym;
        bound(target, local(name), stolen);
        return;
    }
    /* Otherwise what the main window does with it; a system action goes to
     * the main window's queue, as a gamepad's does. */
    if (evt->isAutoRepeat()) return;
    if (!keysym || smsIsAppKey(evt)) { QWidget::keyPressEvent(evt); return; }
    const int sysact = smssession_key_sysaction(m_session, keysym);
    if (sysact >= 0) {
        smssession_sysaction_post(m_session, sysact);
        return;
    }
    if (!smssession_key(m_session, keysym, 1)) QWidget::keyPressEvent(evt);
}

void ControllersWindow::keyReleaseEvent(QKeyEvent *evt)
{
    if (evt->isAutoRepeat()) return;
    const uint32_t keysym = smsKeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) {
        m_swallow = 0;
        return;
    }
    if (!keysym) return;
    if (m_mapState != -2) {
        /* a key held before Map armed must still let go */
        smssession_key(m_session, keysym, 0);
        return;
    }
    if (!smssession_key(m_session, keysym, 0)) QWidget::keyReleaseEvent(evt);
}

QPointer<ControllersWindow> g_window;

} // namespace

void smsControllersToggle(QWidget *parent, smssession *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (g_window->isVisible()) g_window->hide();
    else g_window->present();
}

void smsControllersShow(QWidget *parent, smssession *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (g_window->isVisible()) {
        g_window->raise();
        g_window->activateWindow();
    } else {
        g_window->present();
    }
}

bool smsControllersVisible()
{
    return g_window && g_window->isVisible();
}

void smsControllersRefresh()
{
    if (g_window) g_window->refresh();
}
