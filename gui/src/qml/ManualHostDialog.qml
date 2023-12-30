import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15

import org.streetpea.chiaki4deck 1.0

import "controls" as C

DialogView {
    title: qsTr("Add Manual Console")
    buttonText: qsTr("✓ Add")
    buttonEnabled: hostField.text.trim()
    onAccepted: {
        Chiaki.addManualHost(consoleCombo.model[consoleCombo.currentIndex].index, hostField.text);
        close();
    }

    Item {
        GridLayout {
            anchors {
                top: parent.top
                horizontalCenter: parent.horizontalCenter
                topMargin: 20
            }
            columns: 2
            rowSpacing: 20
            columnSpacing: 20

            Label {
                Layout.alignment: Qt.AlignRight
                text: qsTr("Host:")
            }

            C.TextField {
                id: hostField
                Layout.preferredWidth: 400
                firstInFocusChain: true
            }

            Label {
                Layout.alignment: Qt.AlignRight
                text: qsTr("Registered Consoles:")
            }

            C.ComboBox {
                id: consoleCombo
                Layout.preferredWidth: 400
                lastInFocusChain: true
                textRole: "name"
                model: {
                    var m = [];
                    m.push({
                        name: qsTr("Register on first Connection"),
                        index: -1,
                    });
                    for (var i = 0; i < Chiaki.hosts.length; ++i) {
                        var host = Chiaki.hosts[i];
                        if (!host.registered)
                            continue;
                        m.push({
                            name: "%1 (%2)".arg(host.mac).arg(host.name),
                            index: i,
                        });
                    }
                    return m;
                }
            }
        }
    }
}
