import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import NotionRecorder

// Always-on-top floating control bar (Notion style).
Item {
    id: bar

    property point dragOffset: Qt.point(0, 0)
    property var rootWindow: null
    property SettingsDialog settingsDialog: null
    property RegionSelector regionSelector: null
    property RecordingHistory historyDialog: null
    property bool encoderHovered: false

    layer.enabled: true
    layer.effect: MultiEffect {
        shadowEnabled: true
        shadowBlur: 0.65
        shadowColor: "#1A000000"
        shadowVerticalOffset: 2
    }

    Rectangle {
        id: surface
        anchors.fill: parent
        radius: 8
        color: "#FFFFFF"
        border.color: "#E3E2E0"
        border.width: 1

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onPressed: (mouse) => {
                bar.dragOffset = Qt.point(mouse.x, mouse.y)
            }
            onPositionChanged: (mouse) => {
                if (mouse.buttons & Qt.LeftButton && bar.rootWindow) {
                    bar.rootWindow.x += mouse.x - bar.dragOffset.x
                    bar.rootWindow.y += mouse.y - bar.dragOffset.y
                }
            }
        }
    }

    Row {
        id: contentRow
        anchors.fill: parent
        anchors.leftMargin: 18
        anchors.rightMargin: 14
        spacing: 14

        Text {
            id: statusText
            anchors.verticalCenter: parent.verticalCenter
            font.pixelSize: 13
            font.weight: app.recording ? Font.Medium : Font.Normal
            color: app.recording ? "#EB5757" : (app.paused ? "#9B9A97" : "#37352F")
            text: app.recording ? "录制中 " + app.elapsedText
                                : (app.paused ? "已暂停 " + app.elapsedText
                                              : app.statusText)
            width: 150
            horizontalAlignment: Text.AlignLeft
            elide: Text.ElideRight

            ToolTip.text: app.encoderName
            ToolTip.visible: bar.encoderHovered && app.encoderName.length > 0
            ToolTip.delay: 600

            MouseArea {
                id: hoverArea
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.NoButton
                onEntered: bar.encoderHovered = true
                onExited: bar.encoderHovered = false
            }
        }

        Item { width: 1; height: 1 }

        // Record button: hollow ring when idle, solid red dot when recording
        Rectangle {
            id: recordButton
            objectName: "recordButton"
            anchors.verticalCenter: parent.verticalCenter
            width: 38
            height: 38
            radius: 19
            color: "transparent"
            border.width: app.recording ? 0 : 2
            border.color: "#37352F"

            Rectangle {
                visible: app.recording
                anchors.centerIn: parent
                width: 18
                height: 18
                radius: 9
                color: "#EB5757"
                Behavior on width { NumberAnimation { duration: 120 } }
                Behavior on height { NumberAnimation { duration: 120 } }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: app.toggleRecording()
            }
        }

        // Pause / resume
        Item {
            id: pauseButton
            anchors.verticalCenter: parent.verticalCenter
            visible: app.recording || app.paused
            width: 28
            height: 28

            Row {
                anchors.centerIn: parent
                spacing: 4
                visible: !app.paused
                Rectangle { width: 4; height: 13; radius: 1; color: "#37352F" }
                Rectangle { width: 4; height: 13; radius: 1; color: "#37352F" }
            }

            Canvas {
                anchors.centerIn: parent
                visible: app.paused
                width: 14
                height: 14
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.fillStyle = "#37352F"
                    ctx.beginPath()
                    ctx.moveTo(0, 0)
                    ctx.lineTo(14, 7)
                    ctx.lineTo(0, 14)
                    ctx.closePath()
                    ctx.fill()
                }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: app.togglePause()
            }
        }

        // Region selection
        Rectangle {
            id: regionButton
            objectName: "regionButton"
            anchors.verticalCenter: parent.verticalCenter
            width: 30
            height: 30
            radius: 5
            color: "transparent"

            Canvas {
                anchors.centerIn: parent
                width: 16
                height: 16
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.strokeStyle = "#37352F"
                    ctx.lineWidth = 1.4
                    ctx.strokeRect(1.5, 1.5, 13, 13)
                    ctx.beginPath()
                    ctx.moveTo(6.5, 0); ctx.lineTo(9.5, 0)
                    ctx.moveTo(0, 6.5); ctx.lineTo(0, 9.5)
                    ctx.moveTo(16, 6.5); ctx.lineTo(16, 9.5)
                    ctx.moveTo(6.5, 16); ctx.lineTo(9.5, 16)
                    ctx.stroke()
                }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (bar.regionSelector !== null) {
                        bar.regionSelector.visible = true
                    }
                }
            }
        }

        // Settings
        Rectangle {
            id: settingsButton
            objectName: "settingsButton"
            anchors.verticalCenter: parent.verticalCenter
            width: 30
            height: 30
            radius: 5
            color: "transparent"

            Text {
                anchors.centerIn: parent
                text: "⚙"
                font.pixelSize: 16
                color: "#37352F"
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (bar.settingsDialog !== null) {
                        bar.settingsDialog.visible = true
                    }
                }
            }
        }

        // Recording history
        Rectangle {
            id: filesButton
            objectName: "filesButton"
            anchors.verticalCenter: parent.verticalCenter
            width: 30
            height: 30
            radius: 5
            color: "transparent"

            Text {
                anchors.centerIn: parent
                text: "≡"
                font.pixelSize: 18
                font.weight: Font.Medium
                color: "#37352F"
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (bar.historyDialog !== null) {
                        bar.historyDialog.visible = true
                    }
                }
            }
        }

        // Annotation toggle (visible while recording)
        Rectangle {
            id: annotationButton
            objectName: "annotationButton"
            anchors.verticalCenter: parent.verticalCenter
            width: 30
            height: 30
            radius: 5
            visible: app.recording
            color: app.annotationMode ? "#EFEEEA" : "transparent"

            Text {
                anchors.centerIn: parent
                text: "✎"
                font.pixelSize: 16
                color: app.annotationMode ? "#EB5757" : "#37352F"
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: app.toggleAnnotationMode()
            }
        }
    }

    // Tiny CPU load indicator while recording (PRD performance monitoring).
    Text {
        anchors.left: parent.left
        anchors.leftMargin: 20
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 2
        text: app.highLoad
              ? "CPU " + app.cpuPercent + "% · 点击降至 30FPS"
              : "CPU " + app.cpuPercent + "%"
        color: app.highLoad ? "#EB5757" : "#D3D1CA"
        font.pixelSize: 9
        visible: app.recording

        MouseArea {
            anchors.fill: parent
            cursorShape: app.highLoad ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: {
                if (app.highLoad) {
                    app.setFps(30)
                }
            }
        }
    }
}
