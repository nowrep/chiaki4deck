import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15
import QtQuick.Controls.Material 2.15

import org.streetpea.chiaki4deck 1.0

Item {
    StackView.onActivated: forceActiveFocus()
    Keys.onUpPressed: hostsView.decrementCurrentIndex()
    Keys.onDownPressed: hostsView.incrementCurrentIndex()
    Keys.onReturnPressed: hostsView.currentItem.clicked()
    Keys.onMenuPressed: settingsButton.clicked()

    ToolBar {
        id: toolBar
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
        }
        height: 80

        RowLayout {
            anchors {
                fill: parent
                leftMargin: 10
                rightMargin: 10
            }

            Button {
                Layout.fillHeight: true
                Layout.preferredWidth: 100
                flat: true
                text: "×"
                font.pixelSize: 60
                focusPolicy: Qt.NoFocus
                onClicked: Qt.quit()
            }

            Item { Layout.fillWidth: true }

            Button {
                Layout.fillHeight: true
                Layout.preferredWidth: 100
                flat: true
                icon.source: "qrc:/icons/add-24px.svg";
                icon.width: 50
                icon.height: 50
                focusPolicy: Qt.NoFocus
                onClicked: root.showManualHostDialog()
            }

            Button {
                id: settingsButton
                Layout.fillHeight: true
                Layout.preferredWidth: 100
                flat: true
                icon.source: "qrc:/icons/settings-20px.svg";
                icon.width: 50
                icon.height: 50
                focusPolicy: Qt.NoFocus
                onClicked: root.showSettingsDialog()
            }
        }

        Label {
            anchors.centerIn: parent
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
            textFormat: Text.RichText
            text: "%1 &nbsp;<small>%2</small>".arg(Qt.application.name).arg(Qt.application.version)
            font.bold: true
            font.pixelSize: 26
        }
    }

    ListView {
        id: hostsView
        anchors {
            top: toolBar.bottom
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        clip: true
        model: Chiaki.hosts
        delegate: ItemDelegate {
            id: delegate
            width: parent ? parent.width : 0
            height: 150
            highlighted: ListView.isCurrentItem
            onClicked: Chiaki.connectToHost(index)

            RowLayout {
                anchors {
                    fill: parent
                    leftMargin: 30
                    rightMargin: 10
                    topMargin: 10
                    bottomMargin: 10
                }
                spacing: 50

                Image {
                    Layout.fillHeight: true
                    Layout.preferredWidth: 150
                    fillMode: Image.PreserveAspectFit
                    source: "qrc:/icons/console-ps" + (modelData.ps5 ? "5" : "4") + ".svg"
                    sourceSize: Qt.size(width, height)
                }

                Label {
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    text: {
                        var t = "";
                        if (modelData.name)
                            t += modelData.name + "\n";
                        t += qsTr("Address: %1").arg(modelData.address);
                        if (modelData.mac)
                            t += "\n" + qsTr("ID: %1 (%2)").arg(modelData.mac).arg(modelData.registered ? qsTr("registered") : qsTr("unregistered"));
                        t += "\n" + (modelData.discovered ? qsTr("discovered") : qsTr("manual"));
                        return t;
                    }
                }

                Label {
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    text: {
                        var t = "";
                        if (!modelData.discovered)
                            return t;
                        t += qsTr("State: %1").arg(modelData.state);
                        if (modelData.app)
                            t += "\n" + qsTr("App: %1").arg(modelData.app);
                        if (modelData.titleId)
                            t += "\n" + qsTr("Title ID: %1").arg(modelData.titleId);
                        return t;
                    }
                }

                Item { Layout.fillWidth: true }

                ColumnLayout {
                    Layout.fillHeight: true
                    spacing: 0

                    Button {
                        Layout.alignment: Qt.AlignCenter
                        text: qsTr("Delete")
                        flat: true
                        padding: 20
                        focusPolicy: Qt.NoFocus
                        visible: !modelData.discovered
                        onClicked: Chiaki.deleteHost(index)
                    }

                    Button {
                        Layout.alignment: Qt.AlignCenter
                        text: qsTr("Send WakeUp")
                        flat: true
                        padding: 20
                        focusPolicy: Qt.NoFocus
                        visible: modelData.registered
                        onClicked: Chiaki.wakeUpHost(index)
                    }
                }
            }
        }
    }

    RoundButton {
        anchors {
            left: parent.left
            bottom: parent.bottom
            margins: 10
        }
        icon.source: "qrc:/icons/discover-" + (checked ? "" : "off-") + "24px.svg"
        icon.width: 50
        icon.height: 50
        padding: 20
        focusPolicy: Qt.NoFocus
        checkable: true
        checked: Chiaki.discoveryEnabled
        onToggled: Chiaki.discoveryEnabled = !Chiaki.discoveryEnabled
    }

    Image {
        anchors.centerIn: parent
        source: "qrc:/icons/chiaki.svg"
        sourceSize: Qt.size(Math.min(parent.width, parent.height) / 2.5, Math.min(parent.width, parent.height) / 2.5)
        opacity: 0.2
    }
}
