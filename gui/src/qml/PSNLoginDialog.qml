import QtQuick 2.15
import QtWebEngine 1.10
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15

import org.streetpea.chiaki4deck 1.0

DialogView {
    id: dialog
    property var callback: null
    title: qsTr("PSN Login")
    buttonVisible: false

    Item {
        WebEngineView {
            id: webView
            anchors.fill: parent
            url: Chiaki.psnLoginUrl()
            profile: WebEngineProfile { offTheRecord: true }
            onContextMenuRequested: (request) => request.accepted = true
            onNavigationRequested: (request) => {
                if (Chiaki.handlePsnLoginRedirect(request.url)) {
                    overlay.opacity = 0.8;
                }
            }
        }

        Rectangle {
            id: overlay
            anchors.fill: webView
            color: "grey"
            opacity: 0.0
            visible: opacity

            MouseArea {
                anchors.fill: parent
            }

            BusyIndicator {
                anchors.centerIn: parent
                layer.enabled: true
                width: 80
                height: width
            }
        }

        Connections {
            target: Chiaki

            function onPsnLoginAccountIdDone(accountId) {
                dialog.callback(accountId);
                dialog.close();
            }
        }
    }
}
