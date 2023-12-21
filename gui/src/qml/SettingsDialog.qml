import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15

import org.streetpea.chiaki4deck 1.0

DialogView {
    title: qsTr("Settings")
    buttonVisible: false

    Item {
        TabBar {
            id: bar
            anchors {
                top: parent.top
                left: parent.left
                right: parent.right
                topMargin: 5
            }

            TabButton {
                text: qsTr("General")
            }

            TabButton {
                text: qsTr("Video")
            }

            TabButton {
                text: qsTr("Audio")
            }

            TabButton {
                text: qsTr("Consoles")
            }

            TabButton {
                text: qsTr("Keys")
            }
        }

        StackLayout {
            anchors {
                top: bar.bottom
                left: parent.left
                right: parent.right
                bottom: parent.bottom
            }
            currentIndex: bar.currentIndex

            Item {
                // General
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
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Verbose Logging:")
                    }

                    CheckBox {
                        text: qsTr("Warning: Don't enable for regular use")
                        checked: Chiaki.settings.logVerbose
                        onToggled: Chiaki.settings.logVerbose = checked
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("PS5 Features:")
                    }

                    CheckBox {
                        text: qsTr("DualSense and Steam Deck haptics and adaptive triggers")
                        checked: Chiaki.settings.dualSense
                        onToggled: Chiaki.settings.dualSense = checked
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Buttons By Position:")
                    }

                    CheckBox {
                        text: qsTr("Use buttons by position instead of by label")
                        checked: Chiaki.settings.buttonsByPosition
                        onToggled: Chiaki.settings.buttonsByPosition = checked
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Steam Deck Vertical:")
                    }

                    CheckBox {
                        text: qsTr("Use Steam Deck in vertical orientation (motion controls)")
                        checked: Chiaki.settings.verticalDeck
                        onToggled: Chiaki.settings.verticalDeck = checked
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Action On Disconnect:")
                    }

                    ComboBox {
                        Layout.preferredWidth: 300
                        model: [qsTr("Do Nothing"), qsTr("Enter Sleep Mode"), qsTr("Ask")]
                        currentIndex: Chiaki.settings.disconnectAction
                        onActivated: (index) => Chiaki.settings.disconnectAction = index
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Log Directory:")
                    }

                    TextField {
                        Layout.preferredWidth: 600
                        readOnly: true
                        selectByMouse: true
                        text: Chiaki.settings.logDirectory
                    }
                }

                Button {
                    anchors {
                        bottom: parent.bottom
                        horizontalCenter: parent.horizontalCenter
                        bottomMargin: 50
                    }
                    implicitWidth: 200
                    padding: 20
                    text: qsTr("About %1").arg(Qt.application.name)
                    onClicked: aboutDialog.open()
                }
            }

            Item {
                // Video
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
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Resolution:")
                    }

                    ComboBox {
                        Layout.preferredWidth: 300
                        model: [qsTr("360p"), qsTr("540p"), qsTr("720p"), qsTr("1080p (PS5 and PS4 Pro)")]
                        currentIndex: Chiaki.settings.resolution - 1
                        onActivated: (index) => Chiaki.settings.resolution = index + 1
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("FPS:")
                    }

                    ComboBox {
                        Layout.preferredWidth: 300
                        model: [qsTr("30 fps"), qsTr("60 fps")]
                        currentIndex: (Chiaki.settings.fps / 30) - 1
                        onActivated: (index) => Chiaki.settings.fps = (index + 1) * 30
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Bitrate:")
                    }

                    TextField {
                        Layout.preferredWidth: 300
                        text: Chiaki.settings.bitrate || ""
                        placeholderText: {
                            var bitrate = 0;
                            switch (Chiaki.settings.resolution) {
                            case 1: bitrate = 2000; break; // 360p
                            case 2: bitrate = 6000; break; // 540p
                            case 3: bitrate = 10000; break; // 720p
                            case 4: bitrate = 15000; break; // 1080p
                            }
                            return qsTr("Automatic (%1)").arg(bitrate);
                        }
                        onEditingFinished: {
                            var num = parseInt(text);
                            if (num >= 2000 && num <= 50000) {
                                Chiaki.settings.bitrate = num;
                            } else {
                                Chiaki.settings.bitrate = 0;
                                text = "";
                            }
                        }
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Codec:")
                    }

                    ComboBox {
                        Layout.preferredWidth: 300
                        model: [qsTr("H264"), qsTr("H265 (PS5)"), qsTr("H265 HDR (PS5)")]
                        currentIndex: Chiaki.settings.codec
                        onActivated: (index) => Chiaki.settings.codec = index
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Hardware Decoder:")
                    }

                    ComboBox {
                        Layout.preferredWidth: 300
                        model: Chiaki.settings.availableDecoders
                        currentIndex: Math.max(0, model.indexOf(Chiaki.settings.decoder))
                        onActivated: (index) => Chiaki.settings.decoder = model[index]
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Render Preset:")
                    }

                    ComboBox {
                        Layout.preferredWidth: 300
                        model: [qsTr("Default"), qsTr("Fast"), qsTr("High Quality")]
                        currentIndex: Chiaki.settings.videoPreset
                        onActivated: (index) => Chiaki.settings.videoPreset = index
                    }
                }
            }

            Item {
                // Audio
                GridLayout {
                    anchors {
                        top: parent.top
                        horizontalCenter: parent.horizontalCenter
                        topMargin: 20
                    }
                    columns: 2
                    rowSpacing: 10
                    columnSpacing: 20
                    onVisibleChanged: if (visible) Chiaki.settings.refreshAudioDevices()

                    Label {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Output Device:")
                    }

                    ComboBox {
                        Layout.preferredWidth: 300
                        model: [qsTr("Auto")].concat(Chiaki.settings.availableAudioOutDevices)
                        currentIndex: Math.max(0, model.indexOf(Chiaki.settings.audioOutDevice))
                        onActivated: (index) => Chiaki.settings.audioOutDevice = index ? model[index] : ""
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Input Device:")
                    }

                    ComboBox {
                        Layout.preferredWidth: 300
                        model: [qsTr("Auto")].concat(Chiaki.settings.availableAudioInDevices)
                        currentIndex: Math.max(0, model.indexOf(Chiaki.settings.audioInDevice))
                        onActivated: (index) => Chiaki.settings.audioInDevice = index ? model[index] : ""
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Buffer Size:")
                    }

                    TextField {
                        Layout.preferredWidth: 300
                        text: Chiaki.settings.audioBufferSize || ""
                        placeholderText: qsTr("Default (19200)")
                        onEditingFinished: {
                            var num = parseInt(text);
                            if (num >= 1024 && num <= 0x20000) {
                                Chiaki.settings.audioBufferSize = num;
                            } else {
                                Chiaki.settings.audioBufferSize = 0;
                                text = "";
                            }
                        }
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Speech Processing:")
                    }

                    CheckBox {
                        text: qsTr("Noise suppression + echo cancellation")
                        checked: Chiaki.settings.speechProcessing
                        onToggled: Chiaki.settings.speechProcessing = checked
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Noise To Suppress:")
                        visible: Chiaki.settings.speechProcessing
                    }

                    Slider {
                        Layout.preferredWidth: 250
                        from: 0
                        to: 60
                        stepSize: 1
                        visible: Chiaki.settings.speechProcessing
                        value: Chiaki.settings.noiseSuppressLevel
                        onMoved: Chiaki.settings.noiseSuppressLevel = value

                        Label {
                            anchors {
                                left: parent.right
                                verticalCenter: parent.verticalCenter
                                leftMargin: 10
                            }
                            text: qsTr("%1 dB").arg(parent.value)
                        }
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Echo To Suppress:")
                        visible: Chiaki.settings.speechProcessing
                    }

                    Slider {
                        Layout.preferredWidth: 250
                        from: 0
                        to: 60
                        stepSize: 1
                        value: Chiaki.settings.echoSuppressLevel
                        visible: Chiaki.settings.speechProcessing
                        onMoved: Chiaki.settings.echoSuppressLevel = value

                        Label {
                            anchors {
                                left: parent.right
                                verticalCenter: parent.verticalCenter
                                leftMargin: 10
                            }
                            text: qsTr("%1 dB").arg(parent.value)
                        }
                    }
                }
            }

            Item {
                // Consoles
                Button {
                    id: registerNewButton
                    anchors {
                        top: parent.top
                        horizontalCenter: parent.horizontalCenter
                        topMargin: 30
                    }
                    padding: 20
                    text: qsTr("Register New")
                    onClicked: root.showRegistDialog("255.255.255.255")
                }

                Label {
                    id: consolesLabel
                    anchors {
                        top: registerNewButton.bottom
                        horizontalCenter: registerNewButton.horizontalCenter
                        topMargin: 50
                    }
                    text: qsTr("Registered Consoles")
                    font.bold: true
                }

                ListView {
                    anchors {
                        top: consolesLabel.bottom
                        horizontalCenter: consolesLabel.horizontalCenter
                        bottom: parent.bottom
                        topMargin: 10
                    }
                    width: 500
                    clip: true
                    model: Chiaki.settings.registeredHosts
                    delegate: ItemDelegate {
                        text: "%1 (%2, %3)".arg(modelData.mac).arg(modelData.ps5 ? "PS5" : "PS4").arg(modelData.name)
                        height: 80
                        width: parent ? parent.width : 0

                        Button {
                            anchors {
                                right: parent.right
                                verticalCenter: parent.verticalCenter
                                rightMargin: 20
                            }
                            text: qsTr("Delete")
                            onClicked: Chiaki.settings.deleteRegisteredHost(index)
                        }
                    }
                }
            }

            Item {
                // Keys
                GridLayout {
                    anchors {
                        top: parent.top
                        horizontalCenter: parent.horizontalCenter
                        topMargin: 20
                    }
                    columns: 3
                    rowSpacing: 10
                    columnSpacing: 10

                    Repeater {
                        model: Chiaki.settings.controllerMapping

                        RowLayout {
                            spacing: 20

                            Label {
                                Layout.preferredWidth: 200
                                horizontalAlignment: Text.AlignRight
                                text: modelData.buttonName
                            }

                            Button {
                                Layout.preferredWidth: 150
                                text: modelData.keyName
                                onClicked: text = Chiaki.settings.changeControllerKey(modelData.buttonValue)
                            }
                        }
                    }
                }
            }
        }


        /*
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
        */

        Dialog {
            id: aboutDialog
            anchors.centerIn: parent
            title: qsTr("About %1").arg(Qt.application.name)
            modal: true
            standardButtons: Dialog.Ok

            RowLayout {
                spacing: 50

                Image {
                    Layout.preferredWidth: 200
                    fillMode: Image.PreserveAspectFit
                    verticalAlignment: Image.AlignTop
                    source: "qrc:icons/chiaki4deck.svg"
                }

                Label {
                    Layout.preferredWidth: 400
                    verticalAlignment: Text.AlignTop
                    wrapMode: Text.Wrap
                    text: "<h1>chiaki4deck</h1> by Street Pea, version %1
                        <h2>Fork of Chiaki</h2> by Florian Markl at version 2.1.1

                        <p>This program is free software: you can redistribute it and/or modify
                        it under the terms of the GNU Affero General Public License version 3
                        as published by the Free Software Foundation.</p>

                        <p>This program is distributed in the hope that it will be useful,
                        but WITHOUT ANY WARRANTY; without even the implied warranty of
                        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
                        GNU General Public License for more details.</p>".arg(Qt.application.version)
                }
            }
        }
    }
}
