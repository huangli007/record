import QtQuick
import NotionRecorder

Window {
    id: sel
    x: 0
    y: 0
    width: Screen.width
    height: Screen.height
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "#00000000"
    modality: Qt.ApplicationModal

    property int selX: 0
    property int selY: 0
    property int selW: 0
    property int selH: 0
    property int startX: 0
    property int startY: 0
    property bool selecting: false

    function confirm() {
        if (selW > 10 && selH > 10) {
            app.setRegion(selX, selY, selW, selH)
            app.setCaptureMode(1)
        }
        close()
    }

    function cancel() {
        close()
    }

    // Dark mask (rgba(0,0,0,0.4)) with a clear rectangular hole.
    Rectangle {
        id: fullMask
        anchors.fill: parent
        color: "#66000000"
    }

    Rectangle { id: topMask; visible: selW > 0; color: "#66000000" }
    Rectangle { id: bottomMask; visible: selW > 0; color: "#66000000" }
    Rectangle { id: leftMask; visible: selW > 0; color: "#66000000" }
    Rectangle { id: rightMask; visible: selW > 0; color: "#66000000" }

    function updateMasks() {
        topMask.x = 0
        topMask.y = 0
        topMask.width = sel.width
        topMask.height = selY

        bottomMask.x = 0
        bottomMask.y = selY + selH
        bottomMask.width = sel.width
        bottomMask.height = Math.max(sel.height - (selY + selH), 0)

        leftMask.x = 0
        leftMask.y = selY
        leftMask.width = selX
        leftMask.height = selH

        rightMask.x = selX + selW
        rightMask.y = selY
        rightMask.width = Math.max(sel.width - (selX + selW), 0)
        rightMask.height = selH
    }

    onSelXChanged: updateMasks()
    onSelYChanged: updateMasks()
    onSelWChanged: updateMasks()
    onSelHChanged: updateMasks()

    // Selection border
    Rectangle {
        id: selection
        x: selX
        y: selY
        width: selW
        height: selH
        visible: selW > 0 && selH > 0
        color: "transparent"
        border.color: "#FFFFFF"
        border.width: 1.5
    }

    // Size label (monospace)
    Rectangle {
        id: dims
        visible: selection.visible
        color: "#000000"
        opacity: 0.72
        radius: 3
        width: dimsText.width + 14
        height: dimsText.height + 8
        x: Math.min(Math.max(selection.x + selection.width / 2 - width / 2, 8),
                    sel.width - width - 8)
        y: Math.max(selection.y - height - 10, 8)

        Text {
            id: dimsText
            anchors.centerIn: parent
            color: "#FFFFFF"
            font.family: "Menlo"
            font.pixelSize: 12
            text: Math.round(selection.width * Screen.devicePixelRatio) +
                  " × " +
                  Math.round(selection.height * Screen.devicePixelRatio)
        }
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.CrossCursor
        onPressed: (mouse) => {
            selecting = true
            startX = mouse.x
            startY = mouse.y
            selX = mouse.x
            selY = mouse.y
            selW = 0
            selH = 0
        }
        onPositionChanged: (mouse) => {
            if (selecting) {
                selX = Math.min(startX, mouse.x)
                selY = Math.min(startY, mouse.y)
                selW = Math.abs(mouse.x - startX)
                selH = Math.abs(mouse.y - startY)
            }
        }
        onReleased: (mouse) => {
            selecting = false
        }
    }

    // Bottom action bar
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        y: sel.height - 72
        width: 280
        height: 44
        radius: 6
        color: "#FFFFFF"

        Row {
            anchors.centerIn: parent
            spacing: 18

            Text {
                text: "取消"
                font.pixelSize: 13
                color: "#9B9A97"
                anchors.verticalCenter: parent.verticalCenter
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sel.cancel()
                }
            }

            Rectangle {
                width: 1
                height: 20
                color: "#E3E2E0"
            }

            Text {
                text: "确定区域"
                font.pixelSize: 13
                font.weight: Font.Medium
                color: "#37352F"
                anchors.verticalCenter: parent.verticalCenter
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sel.confirm()
                }
            }
        }
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: sel.height - 104
        text: "拖拽选择录制区域　·　Enter 确定　·　Esc 取消"
        font.pixelSize: 12
        color: "#FFFFFF"
        opacity: 0.85
    }

    onVisibleChanged: {
        if (visible) {
            keyHandler.forceActiveFocus()
        }
    }

    Item {
        id: keyHandler
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: cancel()
        Keys.onReturnPressed: confirm()
        Keys.onEnterPressed: confirm()
    }
}
