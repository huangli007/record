import QtQuick
import QtQuick.Effects

Window {
    id: root
    width: 640
    height: 84
    visible: true
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
    color: "transparent"

    SettingsDialog {
        id: settingsDialog
    }

    RegionSelector {
        id: regionSelector
    }

    AnnotationOverlay {
        id: annotationOverlay
        visible: app.recording && app.annotationMode
    }

    RecordingHistory {
        id: historyDialog
    }

    FloatingBar {
        anchors.fill: parent
        anchors.margins: 10
        rootWindow: root
        settingsDialog: settingsDialog
        regionSelector: regionSelector
        historyDialog: historyDialog
    }

    // Transient notification toast (auto-save result / errors).
    Window {
        id: toast
        visible: false
        flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
        width: 420
        height: 58
        color: "transparent"

        function show(message, isError) {
            toastText.text = message
            toastText.color = isError ? "#EB5757" : "#37352F"
            x = root.x + root.width - width - 8
            y = root.y + root.height + 10
            visible = true
            hideTimer.restart()
        }

        Rectangle {
            anchors.fill: parent
            radius: 10
            color: "#FFFFFF"
            border.color: "#E3E2E0"
            border.width: 1
            layer.enabled: true
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowBlur: 0.6
                shadowColor: "#1A000000"
                shadowVerticalOffset: 2
            }
        }

        Text {
            id: toastText
            anchors.fill: parent
            anchors.margins: 12
            font.pixelSize: 12
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
            elide: Text.ElideMiddle
        }

        Timer {
            id: hideTimer
            interval: 4000
            onTriggered: toast.visible = false
        }
    }

    Connections {
        target: app
        function onSessionFinished(path) {
            toast.show("已保存：" + path, false)
        }
        function onRecordingFailed(message) {
            toast.show(message, true)
        }
    }
}
