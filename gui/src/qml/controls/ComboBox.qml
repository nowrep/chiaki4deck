import QtQuick 2.15
import QtQuick.Controls 2.15

ComboBox {
    property bool firstInFocusChain: false
    property bool lastInFocusChain: false

    Keys.onPressed: (event) => {
        switch (event.key) {
        case Qt.Key_Up:
            if (!popup.visible) {
                var item = nextItemInFocusChain(false);
                if (!firstInFocusChain && item)
                    item.forceActiveFocus(Qt.TabFocusReason);
                event.accepted = true;
            }
            break;
        case Qt.Key_Down:
            if (!popup.visible) {
                var item = nextItemInFocusChain();
                if (!lastInFocusChain && item)
                    item.forceActiveFocus(Qt.TabFocusReason);
                event.accepted = true;
            }
            break;
        case Qt.Key_Return:
            if (popup.visible) {
                activated(highlightedIndex);
                popup.close();
            } else {
                popup.open();
            }
            event.accepted = true;
            break;
        }
    }

    Keys.onReleased: (event) => {
        if (event.key == Qt.Key_Return)
            event.accepted = true;
    }
}
