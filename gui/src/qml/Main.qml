import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15
import QtQuick.Controls.Material 2.15

import org.streetpea.chiaki4deck 1.0

Item {
    id: root

    function showMainView() {
        if (stack.depth > 1)
            stack.pop(stack.get(0));
        else
            stack.replace(stack.get(0), mainViewComponent);
    }

    function showStreamView() {
        stack.replace(stack.get(0), streamViewComponent);
    }

    function showManualHostDialog() {
        stack.push(manualHostDialogComponent);
    }

    function showRegistDialog(host) {
        stack.push(registDialogComponent, {host: host});
    }

    function showSettingsDialog() {
        stack.push(settingsDialogComponent);
    }

    Component.onCompleted: {
        if (Chiaki.session)
            stack.replace(stack.get(0), streamViewComponent, {}, StackView.Immediate);
    }

    Pane {
        anchors.fill: parent
        visible: !Chiaki.window.hasVideo || !Chiaki.window.keepVideo
    }

    StackView {
        id: stack
        anchors.fill: parent
        initialItem: mainViewComponent

        replaceEnter: Transition {
            PropertyAnimation {
                property: "opacity"
                from: 0.01
                to: 1.0
                duration: 200
            }
        }

        replaceExit: Transition {
            PropertyAnimation {
                property: "opacity"
                from: 1.0
                to: 0.0
                duration: 100
            }
        }
    }

    Rectangle {
        anchors {
            bottom: parent.bottom
            horizontalCenter: parent.horizontalCenter
            bottomMargin: 30
        }
        color: Material.accent
        width: errorLayout.width + 40
        height: errorLayout.height + 20
        opacity: errorHideTimer.running ? 0.8 : 0.0

        Behavior on opacity { NumberAnimation { duration: 500 } }

        ColumnLayout {
            id: errorLayout
            anchors.centerIn: parent

            Label {
                id: errorTitleLabel
                Layout.alignment: Qt.AlignCenter
                font.bold: true
                font.pixelSize: 24
            }

            Label {
                id: errorTextLabel
                Layout.alignment: Qt.AlignCenter
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 20
            }
        }

        Timer {
            id: errorHideTimer
            interval: 2000
        }
    }

    Connections {
        target: Chiaki

        function onSessionChanged() {
            if (Chiaki.session)
                root.showStreamView();
        }

        function onError(title, text) {
            errorTitleLabel.text = title;
            errorTextLabel.text = text;
            errorHideTimer.start();
        }

        function onRegistDialogRequested(host) {
            showRegistDialog(host);
        }
    }

    Component {
        id: mainViewComponent
        MainView { }
    }

    Component {
        id: streamViewComponent
        StreamView { }
    }

    Component {
        id: manualHostDialogComponent
        ManualHostDialog { }
    }

    Component {
        id: settingsDialogComponent
        SettingsDialog { }
    }

    Component {
        id: registDialogComponent
        RegistDialog { }
    }
}
