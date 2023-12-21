#include "sdinputcontext.h"

#include <QRectF>
#include <QGuiApplication>
#include <QDesktopServices>

#include <QInputMethodQueryEvent>
#include <QQuickItem>
#include <QWindow>

QPlatformInputContext *SDInputContextPlugin::create(const QString &key, const QStringList &paramList)
{
    if (key.compare(QStringLiteral("sdinput"), Qt::CaseInsensitive) == 0)
        return new SDInputContext;
    return nullptr;
}

SDInputContext::SDInputContext()
{
}

bool SDInputContext::isValid() const
{
    return true;
}

#include <QDebug>
void SDInputContext::showInputPanel()
{
    const QRect r = QGuiApplication::inputMethod()->inputItemClipRectangle().toRect();
    const QString url = QStringLiteral("steam://open/keyboard?XPosition=%1&YPosition=%2&Width=%3&Height=%4&Mode=1");
    if (!QDesktopServices::openUrl(QUrl(url.arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height())))) {
        qWarning("Failed to open keyboard");
        return;
    }
    is_visible = true;
    emitInputPanelVisibleChanged();
}

void SDInputContext::hideInputPanel()
{
    const QString url = QStringLiteral("steam://close/keyboard");
    if (!QDesktopServices::openUrl(QUrl(url))) {
        qWarning("Failed to close keyboard");
        return;
    }
    is_visible = false;
    emitInputPanelVisibleChanged();
}

bool SDInputContext::isInputPanelVisible() const
{
    return is_visible;
}
