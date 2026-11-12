#include "floating_orb.hpp"
#include "theme_util.hpp"
#include "window_pin_util.hpp"

#include <QContextMenuEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QShowEvent>
#include <QWindow>

namespace shadowbar::floating_ui {

FloatingOrb::FloatingOrb(QWidget *parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint |
                          Qt::WindowStaysOnTopHint) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setWindowTitle("ShadowBar 悬浮球");
    setToolTip("ShadowBar 翻译悬浮窗\n• 左键点击展开 / 收起\n• 按住拖动位置\n• 右键打开快捷菜单");
    setFixedSize(50, 50);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(3, 3, 3, 3);

    circle_ = new QWidget(this);
    circle_->setObjectName("OrbCircle");
    circle_->setFixedSize(44, 44);
    circle_->setCursor(Qt::PointingHandCursor);

    auto *inner = new QHBoxLayout(circle_);
    inner->setContentsMargins(0, 0, 0, 0);

    glyph_ = new QLabel("✦", circle_);
    glyph_->setAlignment(Qt::AlignCenter);
    QFont f = glyph_->font();
    f.setPointSize(14);
    f.setBold(true);
    glyph_->setFont(f);
    inner->addWidget(glyph_);

    root->addWidget(circle_);
    applyTheme();
}

void FloatingOrb::applyTheme() {
    const auto t = resolveThemeTokens();
    const QColor bg = hovered_ ? t.surface_elevated : t.orb_bg;
    const QColor border = hovered_ ? t.accent : t.orb_border;
    circle_->setStyleSheet(QString(
        "#OrbCircle {"
        "  background-color: %1;"
        "  border: 1.5px solid %2;"
        "  border-radius: 22px;"
        "}"
        "#OrbCircle QLabel { color: %3; background: transparent; }")
                               .arg(cssRgba(bg), cssRgba(border),
                                    t.accent.name()));
}

void FloatingOrb::setRememberedPosition(bool has_pos, const QPoint &pos) {
    has_remembered_pos_ = has_pos;
    remembered_pos_ = pos;
    if (has_pos && !isWaylandPlatform()) {
        restoring_pos_ = true;
        move(pos);
        restoring_pos_ = false;
    }
}

void FloatingOrb::changeEvent(QEvent *event) {
    if (event->type() == QEvent::ApplicationPaletteChange ||
        event->type() == QEvent::PaletteChange) {
        applyTheme();
    }
    QWidget::changeEvent(event);
}

void FloatingOrb::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (!event->spontaneous()) {
        PinWindowOptions opts;
        opts.keep_above = true;
        opts.on_all_desktops = true;
        opts.skip_taskbar = true;
        opts.skip_switcher = true;
        if (isWaylandPlatform() && has_remembered_pos_) {
            opts.apply_position = true;
            opts.target_pos = remembered_pos_;
        }
        scheduleNativeWindowPinSync(this, opts);
    }
}

void FloatingOrb::moveEvent(QMoveEvent *event) {
    QWidget::moveEvent(event);
    if (!restoring_pos_ && !isWaylandPlatform() && isVisible()) {
        remembered_pos_ = pos();
        has_remembered_pos_ = true;
        emit positionChanged(remembered_pos_);
    }
}

void FloatingOrb::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        press_global_pos_ = event->globalPosition();
        dragging_ = false;
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void FloatingOrb::mouseMoveEvent(QMouseEvent *event) {
    if ((event->buttons() & Qt::LeftButton) && !dragging_) {
        if ((event->globalPosition() - press_global_pos_).manhattanLength() > 6.0) {
            dragging_ = true;
            if (QWindow *win = windowHandle()) {
                win->startSystemMove();
            }
        }
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void FloatingOrb::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        const bool was_dragging = dragging_;
        dragging_ = false;
        if (!was_dragging) {
            emit openCardRequested();
        } else {
            emit dragCompleted();
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void FloatingOrb::contextMenuEvent(QContextMenuEvent *event) {
    QMenu menu(this);
    menu.addAction("打开翻译悬浮窗", this, &FloatingOrb::openCardRequested);
    menu.addAction("读取剪贴板并翻译", this, &FloatingOrb::readClipboardRequested);
    menu.addAction("重置悬浮窗位置与大小", this, &FloatingOrb::resetWindowRequested);
    menu.addAction("设置与 API 管理...", this, &FloatingOrb::openSettingsRequested);
    menu.addSeparator();
    menu.addAction("隐藏桌面悬浮球", this, &FloatingOrb::hideOrbRequested);
    menu.addAction("退出后台悬浮窗服务", this, &FloatingOrb::quitRequested);
    menu.exec(event->globalPos());
}

void FloatingOrb::enterEvent(QEnterEvent *event) {
    hovered_ = true;
    applyTheme();
    QWidget::enterEvent(event);
}

void FloatingOrb::leaveEvent(QEvent *event) {
    hovered_ = false;
    if (dragging_) {
        dragging_ = false;
        emit dragCompleted();
    }
    applyTheme();
    QWidget::leaveEvent(event);
}

} // namespace shadowbar::floating_ui
