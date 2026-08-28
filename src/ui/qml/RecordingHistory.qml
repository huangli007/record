import QtQuick

Window {
    id: historyWindow
    objectName: "historyDialogWindow"
    width: 480
    height: 420
    title: "录制历史"
    color: "#FFFFFF"
    modality: Qt.WindowModal

    function formatSize(bytes) {
        if (bytes >= 1024 * 1024 * 1024) {
            return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GB"
        }
        if (bytes >= 1024 * 1024) {
            return (bytes / (1024 * 1024)).toFixed(1) + " MB"
        }
        return Math.max(1, Math.round(bytes / 1024)) + " KB"
    }

    // Header
    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 52
        color: "#F7F7F5"
        border.color: "#E3E2E0"
        border.width: 1

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            text: "录制历史"
            font.pixelSize: 15
            font.weight: Font.Medium
            color: "#37352F"
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            HeaderButton {
                text: "刷新"
                onClicked: app.refreshHistory()
            }
            HeaderButton {
                text: "打开文件夹"
                onClicked: app.openOutputFolder()
            }
            HeaderButton {
                text: "✕"
                onClicked: historyWindow.close()
            }
        }
    }

    ListView {
        id: list
        anchors.top: parent.top
        anchors.topMargin: 52
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
        model: app.recordingHistory
        clip: true
        spacing: 4

        Text {
            visible: list.count === 0
            anchors.centerIn: parent
            text: "暂无录制文件"
            color: "#D3D1CA"
            font.pixelSize: 13
        }

        delegate: Rectangle {
            width: list.width
            height: 58
            radius: 8
            color: rowMouse.pressed ? "#EFEEEA" : "#F7F7F5"

            Column {
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3
                Text {
                    text: modelData.name
                    width: historyWindow.width - 90
                    elide: Text.ElideMiddle
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    color: "#37352F"
                }
                Text {
                    text: historyWindow.formatSize(modelData.size) + " · " + modelData.modified
                    font.pixelSize: 11
                    color: "#9B9A97"
                }
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                text: "▶"
                color: "#9B9A97"
                font.pixelSize: 12
            }

            MouseArea {
                id: rowMouse
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: app.revealRecording(modelData.path)
            }
        }
    }

    component HeaderButton: Rectangle {
        id: headerButton
        property string text: ""
        signal clicked()

        width: 58
        height: 28
        radius: 7
        color: headerMouse.pressed ? "#E8E7E3" : "transparent"

        Text {
            anchors.centerIn: parent
            text: headerButton.text
            color: "#37352F"
            font.pixelSize: 12
        }
        MouseArea {
            id: headerMouse
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: headerButton.clicked()
        }
    }
}
