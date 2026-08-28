import QtQuick

// Full-screen annotation layer shown while recording with annotation mode on.
// Drawings are rendered on the actual screen, so ScreenCaptureKit captures
// them (the capturer keeps this window in the stream by its title).
Window {
    id: overlay
    title: "NotionRecorder-Annotation"
    x: 0
    y: 0
    width: Screen.width
    height: Screen.height
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"

    property string tool: "pen"          // pen | arrow | highlight | text
    property color strokeColor: "#EB5757"
    property int strokeWidth: 4
    property var shapes: []
    property point draftStart: Qt.point(0, 0)
    property point draftEnd: Qt.point(0, 0)
    property var draftPoints: []

    function commitShape(shape) {
        shapes.push(shape)
        draftPoints = []
        canvas.requestPaint()
    }

    function undo() {
        if (shapes.length > 0) {
            shapes.pop()
            canvas.requestPaint()
        }
    }

    function clearAll() {
        shapes = []
        canvas.requestPaint()
    }

    function drawShape(ctx, s) {
        ctx.strokeStyle = s.color
        ctx.fillStyle = s.color
        ctx.lineWidth = s.width
        ctx.lineCap = "round"
        ctx.lineJoin = "round"

        if (s.type === "pen") {
            ctx.beginPath()
            ctx.moveTo(s.points[0].x, s.points[0].y)
            for (let i = 1; i < s.points.length; ++i) {
                ctx.lineTo(s.points[i].x, s.points[i].y)
            }
            ctx.stroke()
        } else if (s.type === "arrow") {
            const x1 = s.x1, y1 = s.y1, x2 = s.x2, y2 = s.y2
            const angle = Math.atan2(y2 - y1, x2 - x1)
            const head = Math.max(10, s.width * 2.5)
            ctx.beginPath()
            ctx.moveTo(x1, y1)
            ctx.lineTo(x2, y2)
            ctx.stroke()
            ctx.beginPath()
            ctx.moveTo(x2, y2)
            ctx.lineTo(x2 - head * Math.cos(angle - 0.42), y2 - head * Math.sin(angle - 0.42))
            ctx.moveTo(x2, y2)
            ctx.lineTo(x2 - head * Math.cos(angle + 0.42), y2 - head * Math.sin(angle + 0.42))
            ctx.stroke()
        } else if (s.type === "highlight") {
            ctx.globalAlpha = 0.22
            ctx.fillRect(s.x, s.y, s.width, s.height)
            ctx.globalAlpha = 1.0
            ctx.lineWidth = 2
            ctx.strokeRect(s.x, s.y, s.width, s.height)
        } else if (s.type === "text") {
            ctx.font = "bold " + s.size + "px 'PingFang SC', 'Helvetica Neue', sans-serif"
            ctx.textBaseline = "top"
            ctx.fillText(s.text, s.x, s.y)
        }
    }

    // Toolbar
    Rectangle {
        z: 20
        anchors.horizontalCenter: parent.horizontalCenter
        y: 24
        height: 44
        width: 320
        radius: 12
        color: "#F2FFFFFF"
        border.color: "#33FFFFFF"

        Row {
            anchors.centerIn: parent
            spacing: 6

            ToolButton {
                label: "✏"
                active: overlay.tool === "pen"
                onClicked: overlay.tool = "pen"
            }
            ToolButton {
                label: "➜"
                active: overlay.tool === "arrow"
                onClicked: overlay.tool = "arrow"
            }
            ToolButton {
                label: "▭"
                active: overlay.tool === "highlight"
                onClicked: overlay.tool = "highlight"
            }
            ToolButton {
                label: "T"
                active: overlay.tool === "text"
                onClicked: overlay.tool = "text"
            }

            Rectangle { width: 1; height: 22; color: "#33FFFFFF"; anchors.verticalCenter: parent.verticalCenter }

            ToolButton { label: "↶"; onClicked: overlay.undo() }
            ToolButton { label: "✕"; onClicked: overlay.clearAll() }
            ToolButton { label: "◼"; onClicked: app.toggleAnnotationMode() }
        }
    }

    // Color picker
    Rectangle {
        z: 19
        anchors.horizontalCenter: parent.horizontalCenter
        y: 78
        height: 34
        width: 176
        radius: 10
        color: "#F2FFFFFF"
        border.color: "#33FFFFFF"

        Row {
            anchors.centerIn: parent
            spacing: 10
            Repeater {
                model: ["#EB5757", "#37352F", "#2D7FF9", "#299E4C", "#F5B93B", "#FFFFFF"]
                delegate: Rectangle {
                    width: 18
                    height: 18
                    radius: 9
                    color: modelData
                    border.color: modelData === "#FFFFFF" ? "#B0B0AE" : "transparent"
                    border.width: 1
                    scale: overlay.strokeColor.toString() === modelData ? 1.2 : 1.0
                    Behavior on scale { NumberAnimation { duration: 100 } }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: overlay.strokeColor = modelData
                    }
                }
            }
        }
    }

    Canvas {
        id: canvas
        z: 1
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            for (let i = 0; i < overlay.shapes.length; ++i) {
                overlay.drawShape(ctx, overlay.shapes[i])
            }
            // Draft in progress
            if (overlay.tool === "pen" && overlay.draftPoints.length > 1) {
                overlay.drawShape(ctx, {
                    type: "pen",
                    color: overlay.strokeColor,
                    width: overlay.strokeWidth,
                    points: overlay.draftPoints
                })
            } else if (overlay.tool === "arrow" && overlay.draftEnd.x !== overlay.draftStart.x) {
                overlay.drawShape(ctx, {
                    type: "arrow",
                    color: overlay.strokeColor,
                    width: overlay.strokeWidth,
                    x1: overlay.draftStart.x, y1: overlay.draftStart.y,
                    x2: overlay.draftEnd.x, y2: overlay.draftEnd.y
                })
            } else if (overlay.tool === "highlight" && overlay.draftEnd.x !== overlay.draftStart.x) {
                overlay.drawShape(ctx, {
                    type: "highlight",
                    color: overlay.strokeColor,
                    width: 1,
                    x: Math.min(overlay.draftStart.x, overlay.draftEnd.x),
                    y: Math.min(overlay.draftStart.y, overlay.draftEnd.y),
                    width: Math.abs(overlay.draftEnd.x - overlay.draftStart.x),
                    height: Math.abs(overlay.draftEnd.y - overlay.draftStart.y)
                })
            }
        }
    }

    MouseArea {
        id: drawArea
        z: 10
        anchors.fill: parent
        cursorShape: overlay.tool === "text" ? Qt.IBeamCursor : Qt.CrossCursor

        onPressed: (mouse) => {
            if (overlay.tool === "text") {
                textEditor.x = mouse.x
                textEditor.y = mouse.y
                textEditor.visible = true
                textEditor.forceActiveFocus()
                return
            }
            overlay.draftStart = Qt.point(mouse.x, mouse.y)
            overlay.draftEnd = Qt.point(mouse.x, mouse.y)
            overlay.draftPoints = [Qt.point(mouse.x, mouse.y)]
        }
        onPositionChanged: (mouse) => {
            if (!pressed) {
                return
            }
            overlay.draftEnd = Qt.point(mouse.x, mouse.y)
            if (overlay.tool === "pen") {
                overlay.draftPoints.push(Qt.point(mouse.x, mouse.y))
            }
            canvas.requestPaint()
        }
        onReleased: (mouse) => {
            if (overlay.tool === "pen" && overlay.draftPoints.length > 1) {
                overlay.commitShape({
                    type: "pen",
                    color: overlay.strokeColor,
                    width: overlay.strokeWidth,
                    points: overlay.draftPoints
                })
            } else if (overlay.tool === "arrow") {
                overlay.commitShape({
                    type: "arrow",
                    color: overlay.strokeColor,
                    width: overlay.strokeWidth,
                    x1: overlay.draftStart.x, y1: overlay.draftStart.y,
                    x2: overlay.draftEnd.x, y2: overlay.draftEnd.y
                })
                overlay.draftEnd = Qt.point(0, 0)
            } else if (overlay.tool === "highlight") {
                overlay.commitShape({
                    type: "highlight",
                    color: overlay.strokeColor,
                    width: 1,
                    x: Math.min(overlay.draftStart.x, overlay.draftEnd.x),
                    y: Math.min(overlay.draftStart.y, overlay.draftEnd.y),
                    width: Math.abs(overlay.draftEnd.x - overlay.draftStart.x),
                    height: Math.abs(overlay.draftEnd.y - overlay.draftStart.y)
                })
                overlay.draftEnd = Qt.point(0, 0)
            }
            canvas.requestPaint()
        }
    }

    TextInput {
        id: textEditor
        z: 30
        visible: false
        width: 320
        height: 36
        font.pixelSize: 22
        font.bold: true
        color: overlay.strokeColor
        onAccepted: {
            if (text.length > 0) {
                overlay.commitShape({
                    type: "text",
                    color: overlay.strokeColor,
                    text: text,
                    x: x, y: y,
                    size: 24
                })
            }
            text = ""
            visible = false
        }
    }

    Text {
        z: 15
        anchors.horizontalCenter: parent.horizontalCenter
        y: Screen.height - 36
        text: "拖拽绘制 · 再次点击标注按钮（◼）关闭"
        color: "#CCFFFFFF"
        font.pixelSize: 12
        style: Text.Outline
        styleColor: "#66000000"
    }

    Item {
        id: keyHandler
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: app.toggleAnnotationMode()
    }

    component ToolButton: Rectangle {
        id: toolButton
        property string label: ""
        property bool active: false
        signal clicked()

        width: 34
        height: 34
        radius: 8
        color: toolButton.active ? "#3337352F" : "transparent"
        border.color: toolButton.active ? "transparent" : "#00FFFFFF"

        Text {
            anchors.centerIn: parent
            text: toolButton.label
            color: toolButton.active ? "#FFFFFF" : "#EAE9E7"
            font.pixelSize: 15
            font.bold: toolButton.label === "T" || toolButton.label === "✕"
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: toolButton.clicked()
        }
    }
}
