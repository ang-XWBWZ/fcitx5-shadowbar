#pragma once

#include <QPoint>
#include <QPointF>
#include <QWidget>

class QLabel;
class QMoveEvent;
class QShowEvent;

namespace shadowbar::floating_ui {

/** Compact draggable floating orb ("✦") offering one-click popup & quick actions. */
class FloatingOrb final : public QWidget {
    Q_OBJECT

public:
    explicit FloatingOrb(QWidget *parent = nullptr);

    void applyTheme();
    void setRememberedPosition(bool has_pos, const QPoint &pos);

signals:
    void openCardRequested();
    void readClipboardRequested();
    void openSettingsRequested();
    void resetWindowRequested();
    void hideOrbRequested();
    void quitRequested();
    void positionChanged(const QPoint &pos);
    void dragCompleted();

protected:
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QWidget *circle_ = nullptr;
    QLabel *glyph_ = nullptr;
    QPointF press_global_pos_;
    QPoint remembered_pos_;
    bool has_remembered_pos_ = false;
    bool restoring_pos_ = false;
    bool dragging_ = false;
    bool hovered_ = false;
};

} // namespace shadowbar::floating_ui
