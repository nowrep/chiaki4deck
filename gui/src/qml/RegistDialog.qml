import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15

import org.streetpea.chiaki4deck 1.0

DialogView {
    property alias host: hostField.text
    title: qsTr("Register Console")
    buttonText: qsTr("Register")
    buttonEnabled: hostField.text.trim() && pin.acceptableInput && (!onlineId.visible || onlineId.text.trim()) && (!accountId.visible || accountId.text.trim())
    StackView.onActivated: host ? accountId.forceActiveFocus() : hostField.forceActiveFocus()
    onAccepted: {
        var psnId = onlineId.visible ? onlineId.text.trim() : accountId.text.trim();
        var registerOk = Chiaki.registerHost(hostField.text.trim(), psnId, pin.text.trim(), broadcast.checked, consoleButtons.checkedButton.target, function(msg, ok, done) {
            if (!done)
                logArea.text += msg + "\n";
            else
                logDialog.standardButtons = Dialog.Close;
            if (ok && done)
                stack.pop();
        });
        if (registerOk) {
            logArea.text = "";
            logDialog.open();
        }
    }

    Item {
        GridLayout {
            anchors {
                top: parent.top
                horizontalCenter: parent.horizontalCenter
                topMargin: 20
            }
            columns: 2
            rowSpacing: 10
            columnSpacing: 20

            Label {
                text: qsTr("Host:")
            }

            TextField {
                id: hostField
                Layout.preferredWidth: 400
            }

            Label {
                text: qsTr("PSN Online-ID:")
                visible: onlineId.visible
            }

            TextField {
                id: onlineId
                visible: ps4_7.checked
                placeholderText: qsTr("username, case-sensitive")
                Layout.preferredWidth: 400
            }

            Label {
                text: qsTr("PSN Account-ID:")
                visible: accountId.visible
            }

            TextField {
                id: accountId
                visible: !ps4_7.checked
                placeholderText: qsTr("base64")
                Layout.preferredWidth: 400
            }

            Label {
                text: qsTr("PIN:")
            }

            TextField {
                id: pin
                validator: RegularExpressionValidator { regularExpression: /[0-9]{8}/ }
                Layout.preferredWidth: 400
            }

            Label {
                text: qsTr("Broadcast:")
            }

            CheckBox {
                id: broadcast
            }

            Label {
                text: qsTr("Console:")
            }

            ColumnLayout {
                spacing: 0

                RadioButton {
                    id: ps4_7
                    property int target: 800
                    text: qsTr("PS4 Firmware < 7.0")
                }

                RadioButton {
                    id: ps4_75
                    property int target: 900
                    text: qsTr("PS4 Firmware >= 7.0, < 8.0")
                }

                RadioButton {
                    id: ps4_8
                    property int target: 1000
                    text: qsTr("PS4 Firmware >= 8.0")
                }

                RadioButton {
                    id: ps5
                    property int target: 1000100
                    text: qsTr("PS5")
                    checked: true
                }
            }
        }

        ButtonGroup {
            id: consoleButtons
            buttons: [ps4_7, ps4_75, ps4_8, ps5]
        }

        Dialog {
            id: logDialog
            anchors.centerIn: parent
            title: qsTr("Register Console")
            modal: true
            closePolicy: Popup.NoAutoClose
            standardButtons: Dialog.Cancel

            TextArea {
                id: logArea
                implicitWidth: 600
                implicitHeight: 400
                readOnly: true
            }
        }
    }
}
