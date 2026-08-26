import QtQuick

Window {
    id: root
    width: 640
    height: 84
    visible: true
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
    color: "transparent"

    SettingsDialog {
        id: settingsDialog
        visible: false
    }

    RegionSelector {
        id: regionSelector
        visible: false
    }

    FloatingBar {
        anchors.fill: parent
        anchors.margins: 10
        rootWindow: root
    }
}
