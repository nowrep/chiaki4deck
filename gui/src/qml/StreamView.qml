import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15
import QtQuick.Controls.Material 2.15

import org.streetpea.chiaki4deck 1.0

Item {
    property bool sessionError: false
    property bool sessionLoading: true

    Component.onCompleted: Chiaki.window.keepVideo = true
    Component.onDestruction: Chiaki.window.keepVideo = false

    Rectangle {
        id: loadingView
        anchors.fill: parent
        color: "black"
        opacity: sessionError || sessionLoading ? 1.0 : 0.0
        visible: opacity

        Behavior on opacity { NumberAnimation { duration: 250 } }

        Image {
            id: chiakiLogo
            anchors.centerIn: parent
            source: "qrc:/icons/chiaki.svg"
            sourceSize: Qt.size(Math.min(parent.width, parent.height) / 2.5, Math.min(parent.width, parent.height) / 2.5)
        }

        Item {
            anchors {
                top: chiakiLogo.bottom
                left: parent.left
                right: parent.right
                bottom: parent.bottom
            }

            BusyIndicator {
                id: spinner
                anchors.centerIn: parent
                width: 70
                height: width
                visible: sessionLoading
            }

            Label {
                id: errorTitleLabel
                anchors {
                    bottom: spinner.top
                    horizontalCenter: spinner.horizontalCenter
                }
                font.pixelSize: 24
            }

            Label {
                id: errorTextLabel
                anchors {
                    top: errorTitleLabel.bottom
                    horizontalCenter: errorTitleLabel.horizontalCenter
                    topMargin: 10
                }
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 20
            }
        }
    }

    RoundButton {
        anchors {
            right: parent.right
            bottom: parent.bottom
            margins: 40
        }
        icon.source: "qrc:/icons/discover-off-24px.svg"
        icon.width: 50
        icon.height: 50
        padding: 20
        checked: true
        opacity: networkIndicatorTimer.running ? 0.7 : 0.0
        visible: opacity

        Behavior on opacity { NumberAnimation { duration: 400 } }

        Timer {
            id: networkIndicatorTimer
            interval: 1000
        }
    }

    Popup {
        id: sessionStopDialog
        property int closeAction: 0
        anchors.centerIn: parent
        modal: true
        padding: 30
        onOpened: {
            closeAction = 0;
            Chiaki.window.grabInput = true;
        }
        onClosed: {
            Chiaki.window.grabInput = false;
            if (closeAction)
                Chiaki.stopSession(closeAction == 1);
        }

        ColumnLayout {
            Label {
                Layout.alignment: Qt.AlignCenter
                text: qsTr("Disconnect Session")
                font.bold: true
                font.pixelSize: 24
            }

            Label {
                Layout.topMargin: 10
                Layout.alignment: Qt.AlignCenter
                text: qsTr("Do you want the Console to go into sleep mode?")
                font.pixelSize: 20
            }

            RowLayout {
                Layout.topMargin: 30
                Layout.alignment: Qt.AlignCenter
                spacing: 30

                Button {
                    id: sleepButton
                    Layout.preferredWidth: 200
                    Layout.minimumHeight: 80
                    Layout.maximumHeight: 80
                    text: qsTr("⏻ Sleep")
                    font.pixelSize: 24
                    Material.background: activeFocus ? parent.Material.accent : parent.Material.background
                    KeyNavigation.left: noButton
                    KeyNavigation.right: noButton
                    Keys.onEscapePressed: sessionStopDialog.close()
                    onVisibleChanged: if (visible) forceActiveFocus()
                    onClicked: {
                        sessionStopDialog.closeAction = 1;
                        sessionStopDialog.close();
                    }
                }

                Button {
                    id: noButton
                    Layout.preferredWidth: 200
                    Layout.minimumHeight: 80
                    Layout.maximumHeight: 80
                    text: qsTr("✖ No")
                    font.pixelSize: 24
                    Material.background: activeFocus ? parent.Material.accent : parent.Material.background
                    KeyNavigation.left: sleepButton
                    KeyNavigation.right: sleepButton
                    Keys.onEscapePressed: sessionStopDialog.close()
                    onClicked: {
                        sessionStopDialog.closeAction = 2;
                        sessionStopDialog.close();
                    }
                }
            }
        }
    }

    Timer {
        id: closeTimer
        interval: 2000
        onTriggered: root.showMainView()
    }

    Connections {
        target: Chiaki

        function onSessionChanged() {
            if (!Chiaki.session)
                closeTimer.start();
        }

        function onSessionError(title, text) {
            sessionError = true;
            sessionLoading = false;
            errorTitleLabel.text = title;
            errorTextLabel.text = text;
            closeTimer.start();
        }

        function onSessionStopDialogRequested() {
            Chiaki.window.grabInput = true;
            sessionStopDialog.open();
        }
    }

    Connections {
        target: Chiaki.window

        function onHasVideoChanged() {
            if (Chiaki.window.hasVideo)
                sessionLoading = false;
            else
                root.showMainView()
        }

        function onCorruptedFramesChanged() {
            if (Chiaki.window.corruptedFrames > 1)
                networkIndicatorTimer.restart();
        }
    }
}
