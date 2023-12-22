import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15
import QtQuick.Controls.Material 2.15
import QtGraphicalEffects 1.0

import org.streetpea.chiaki4deck 1.0

Item {
    property bool sessionError: false
    property bool sessionLoading: true

    StackView.onActivating: Chiaki.window.keepVideo = true
    StackView.onDeactivated: Chiaki.window.keepVideo = false

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
                visible: text
                onVisibleChanged: if (visible) forceActiveFocus()
                Keys.onReturnPressed: root.showMainView()
                Keys.onEscapePressed: root.showMainView()
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
                visible: text
            }
        }
    }

    RoundButton {
        anchors {
            right: parent.right
            top: parent.top
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
            interval: 400
        }
    }

    Item {
        id: menuView
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        height: 200
        opacity: 0.0
        visible: opacity
        onVisibleChanged: {
            if (visible) {
                Chiaki.window.grabInput();
                closeButton.forceActiveFocus();
            } else {
                Chiaki.window.releaseInput();
            }
        }

        Behavior on opacity { NumberAnimation { duration: 250 } }

        function toggle() {
            opacity = visible ? 0.0 : 1.0;
        }

        function close() {
            opacity = 0.0;
        }

        LinearGradient {
            id: bgMask
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent"}
                GradientStop { position: 0.8; color: "#aa808080" }
                GradientStop { position: 1.0; color: "#cc808080" }
            }
        }

        OpacityMask {
            anchors.fill: parent
            source: parent
            maskSource: bgMask
        }

        RowLayout {
            anchors {
                left: parent.left
                bottom: parent.bottom
                leftMargin: 30
                bottomMargin: 40
            }
            spacing: 0

            ToolButton {
                id: closeButton
                Layout.rightMargin: 80
                text: "×"
                padding: 20
                font.pixelSize: 50
                down: activeFocus
                onClicked: Chiaki.window.close()
                KeyNavigation.right: zoomButton
                Keys.onReturnPressed: clicked()
                Keys.onEscapePressed: menuView.close()
            }

            ToolButton {
                id: zoomButton
                text: qsTr("Zoom")
                padding: 20
                checkable: true
                checked: Chiaki.window.videoMode == ChiakiWindow.VideoMode.Zoom
                onToggled: Chiaki.window.videoMode = Chiaki.window.videoMode == ChiakiWindow.VideoMode.Zoom ? ChiakiWindow.VideoMode.Normal : ChiakiWindow.VideoMode.Zoom
                KeyNavigation.left: closeButton
                KeyNavigation.right: stretchButton
                Keys.onReturnPressed: toggled()
                Keys.onEscapePressed: menuView.close()
            }

            ToolSeparator {
                Layout.leftMargin: -10
                Layout.rightMargin: -10
            }

            ToolButton {
                id: stretchButton
                Layout.rightMargin: 50
                text: qsTr("Stretch")
                padding: 20
                checkable: true
                checked: Chiaki.window.videoMode == ChiakiWindow.VideoMode.Stretch
                onToggled: Chiaki.window.videoMode = Chiaki.window.videoMode == ChiakiWindow.VideoMode.Stretch ? ChiakiWindow.VideoMode.Normal : ChiakiWindow.VideoMode.Stretch
                KeyNavigation.left: zoomButton
                KeyNavigation.right: defaultButton
                Keys.onReturnPressed: toggled()
                Keys.onEscapePressed: menuView.close()
            }

            ToolButton {
                id: defaultButton
                text: qsTr("Default")
                padding: 20
                checkable: true
                checked: Chiaki.window.videoPreset == ChiakiWindow.VideoPreset.Default
                onToggled: Chiaki.window.videoPreset = ChiakiWindow.VideoPreset.Default
                KeyNavigation.left: stretchButton
                KeyNavigation.right: highQualityButton
                Keys.onReturnPressed: toggled()
                Keys.onEscapePressed: menuView.close()
            }

            ToolSeparator {
                Layout.leftMargin: -10
                Layout.rightMargin: -10
            }

            ToolButton {
                id: highQualityButton
                text: qsTr("High Quality")
                padding: 20
                checkable: true
                checked: Chiaki.window.videoPreset == ChiakiWindow.VideoPreset.HighQuality
                onToggled: Chiaki.window.videoPreset = ChiakiWindow.VideoPreset.HighQuality
                KeyNavigation.left: defaultButton
                Keys.onReturnPressed: toggled()
                Keys.onEscapePressed: menuView.close()
            }
        }

        Label {
            anchors {
                right: consoleNameLabel.right
                bottom: consoleNameLabel.top
                bottomMargin: 5

            }
            text: "Mbps"
            font.pixelSize: 18

            Label {
                anchors {
                    right: parent.left
                    baseline: parent.baseline
                    rightMargin: 5
                }
                text: visible && Chiaki.session ? Chiaki.session.measuredBitrate.toFixed(1) : ""
                color: Material.accent
                font.bold: true
                font.pixelSize: 28
            }
        }

        Label {
            id: consoleNameLabel
            anchors {
                right: parent.right
                bottom: parent.bottom
                margins: 50
            }
            text: qsTr("Connected to <b>%1</b>").arg(Chiaki.session ? Chiaki.session.host : "")
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
            Chiaki.window.grabInput();
        }
        onClosed: {
            Chiaki.window.releaseInput();
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
                    Keys.onReturnPressed: clicked()
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
                    Keys.onReturnPressed: clicked()
                    Keys.onEscapePressed: sessionStopDialog.close()
                    onClicked: {
                        sessionStopDialog.closeAction = 2;
                        sessionStopDialog.close();
                    }
                }
            }
        }
    }

    Dialog {
        id: sessionPinDialog
        anchors.centerIn: parent
        title: qsTr("Console Login PIN")
        modal: true
        closePolicy: Popup.NoAutoClose
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: {
            Chiaki.window.grabInput();
            standardButton(Dialog.Ok).enabled = Qt.binding(function() {
                return pinField.acceptableInput;
            });
            pinField.forceActiveFocus();
        }
        onClosed: Chiaki.window.releaseInput()
        onAccepted: Chiaki.enterPin(pinField.text)
        onRejected: Chiaki.stopSession(false)

        TextField {
            id: pinField
            implicitWidth: 200
            validator: RegularExpressionValidator { regularExpression: /[0-9]{4}/ }
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

        function onSessionPinDialogRequested() {
            sessionPinDialog.open();
        }

        function onSessionStopDialogRequested() {
            menuView.close();
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

        function onMenuRequested() {
            if (!Chiaki.window.hasVideo)
                return;
            if (sessionPinDialog.opened || sessionStopDialog.opened)
                return;
            menuView.toggle();
        }
    }
}
