import QtQuick
import NotionRecorder

Window {
    id: sel
    objectName: "regionSelectorWindow"
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
    property int mode: 0   // 0 = region drag, 1 = window picker

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

    Rectangle { id: topMask; visible: sel.mode === 0 && selW > 0; color: "#66000000" }
    Rectangle { id: bottomMask; visible: sel.mode === 0 && selW > 0; color: "#66000000" }
    Rectangle { id: leftMask; visible: sel.mode === 0 && selW > 0; color: "#66000000" }
    Rectangle { id: rightMask; visible: sel.mode === 0 && selW > 0; color: "#66000000" }

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
        visible: sel.mode === 0 && selW > 0 && selH > 0
        color: "transparent"
        border.color: "#FFFFFF"
        border.width: 1.5
    }

    // Size label (monospace)
    Rectangle {
        id: dims
        visible: sel.mode === 0 && selection.visible
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
        visible: sel.mode === 0
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
        visible: sel.mode === 0
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
        visible: sel.mode === 0
        anchors.horizontalCenter: parent.horizontalCenter
        y: sel.height - 104
        text: "拖拽选择录制区域　·　Enter 确定　·　Esc 取消"
        font.pixelSize: 12
        color: "#FFFFFF"
        opacity: 0.85
    }

    onVisibleChanged: {
        if (visible) {
            app.refreshWindows()
            keyHandler.forceActiveFocus()
        }
    }

    // Mode toggle: 区域 / 窗口
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 24
        width: 180
        height: 34
        radius: 8
        color: "#CCFFFFFF"
        border.color: "#33FFFFFF"

        Row {
            anchors.fill: parent
            Repeater {
                model: ["区域", "窗口"]
                delegate: Rectangle {
                    width: parent.width / 2
                    height: parent.height
                    radius: 7
                    color: sel.mode === index ? "#37352F" : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: modelData
                        color: sel.mode === index ? "#FFFFFF" : "#E9E8E6"
                        font.pixelSize: 12
                        font.weight: Font.Medium
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: sel.mode = index
                    }
                }
            }
        }
    }

    // Window picker panel
    Rectangle {
        id: windowPanel
        visible: sel.mode === 1
        anchors.centerIn: parent
        width: 480
        height: Math.min(sel.height - 180, 420)
        radius: 12
        color: "#FFFFFF"
        clip: true

        Text {
            anchors.top: parent.top
            anchors.topMargin: 16
            anchors.horizontalCenter: parent.horizontalCenter
            text: "选择要录制的窗口"
            font.pixelSize: 14
            font.weight: Font.Medium
            color: "#37352F"
        }

        Text {
            visible: app.windowList.length === 0
            anchors.centerIn: parent
            width: parent.width - 60
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: "未检测到可录制的窗口。\n请先授予“屏幕录制”权限（系统设置 → 隐私与安全性）后重试。"
            font.pixelSize: 12
            color: "#9B9A97"
            lineHeight: 1.5
        }

        ListView {
            anchors.top: parent.top
            anchors.topMargin: 52
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 14
            model: app.windowList
            spacing: 2
            clip: true

            delegate: Rectangle {
                width: ListView.view.width
                height: 48
                radius: 6
                color: winMouse.pressed ? "#EFEEEA" : "transparent"

                Column {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    Text {
                        text: modelData.title
                        elide: Text.ElideRight
                        width: 410
                        font.pixelSize: 13
                        font.weight: Font.Medium
                        color: "#37352F"
                    }
                    Text {
                        text: modelData.application + " · " +
                              modelData.width + " × " + modelData.height
                        font.pixelSize: 11
                        color: "#9B9A97"
                    }
                }

                MouseArea {
                    id: winMouse
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        app.pickWindow(modelData.id)
                        sel.close()
                    }
                }
            }
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
