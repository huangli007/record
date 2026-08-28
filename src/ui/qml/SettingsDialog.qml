import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import NotionRecorder

Window {
    id: dlg
    width: 700
    height: 460
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: "#FFFFFF"
    title: "设置"

    property string page: "general"

    component SwitchRow: RowLayout {
        id: switchRow
        property string label: ""
        property bool checked: false
        signal toggled(bool value)

        spacing: 12

        Text {
            text: switchRow.label
            font.pixelSize: 13
            color: "#37352F"
        }
        Item { Layout.fillWidth: true }
        Rectangle {
            width: 44
            height: 26
            radius: 13
            color: switchRow.checked ? "#37352F" : "#E3E2E0"
            Behavior on color { ColorAnimation { duration: 120 } }

            Rectangle {
                width: 22
                height: 22
                radius: 11
                color: "#FFFFFF"
                x: switchRow.checked ? 20 : 2
                y: 2
                Behavior on x { NumberAnimation { duration: 120 } }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    switchRow.checked = !switchRow.checked
                    switchRow.toggled(switchRow.checked)
                }
            }
        }
    }

    component SliderRow: RowLayout {
        id: sliderRow
        property string label: ""
        property int value: 100
        property int from: 0
        property int to: 100
        property real stepSize: 1
        signal changed(int value)

        spacing: 12

        Text {
            text: sliderRow.label
            font.pixelSize: 13
            color: "#37352F"
            width: 70
        }
        Slider {
            Layout.fillWidth: true
            from: sliderRow.from
            to: sliderRow.to
            value: sliderRow.value
            stepSize: sliderRow.stepSize
            onMoved: {
                sliderRow.value = value
                sliderRow.changed(value)
            }
        }
        Text {
            text: sliderRow.value
            font.pixelSize: 12
            color: "#9B9A97"
            width: 36
            horizontalAlignment: Text.AlignRight
        }
    }

    component SectionTitle: Text {
        font.pixelSize: 13
        font.weight: Font.Medium
        color: "#37352F"
    }

    // ---------- Header ----------
    Rectangle {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 52
        color: "#F7F7F5"
        border.color: "#E3E2E0"
        border.width: 1

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            text: "设置"
            font.pixelSize: 15
            font.weight: Font.Medium
            color: "#37352F"
        }

        Text {
            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            text: "×"
            font.pixelSize: 18
            color: "#9B9A97"
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: dlg.close()
            }
        }
    }

    // ---------- Body ----------
    RowLayout {
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0

        // Left navigation
        ColumnLayout {
            Layout.preferredWidth: 132
            Layout.fillHeight: true
            Layout.leftMargin: 12
            Layout.topMargin: 18
            spacing: 2

            Repeater {
                model: [
                    { key: "general", label: "通用" },
                    { key: "video", label: "视频" },
                    { key: "audio", label: "音频" },
                    { key: "hotkeys", label: "热键" }
                ]

                Rectangle {
                    width: 112
                    height: 32
                    radius: 5
                    color: dlg.page === modelData.key ? "#EFEFEC" : "transparent"

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.label
                        font.pixelSize: 13
                        color: dlg.page === modelData.key ? "#37352F" : "#9B9A97"
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: dlg.page = modelData.key
                    }
                }
            }

            Item { Layout.fillHeight: true }
        }

        Rectangle {
            Layout.fillHeight: true
            width: 1
            color: "#E3E2E0"
        }

        // Right content
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 24
            Layout.topMargin: 20
            Layout.rightMargin: 24

            // ---------- 通用 ----------
            ColumnLayout {
                visible: dlg.page === "general"
                anchors.fill: parent
                spacing: 20

                SectionTitle { text: "保存位置" }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Rectangle {
                        Layout.fillWidth: true
                        height: 36
                        color: "transparent"
                        border.color: "#E3E2E0"
                        border.width: 1
                        radius: 4

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.right: parent.right
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: outputDirField
                            font.pixelSize: 12
                            color: "#37352F"
                            elide: Text.ElideMiddle
                        }
                    }

                    Rectangle {
                        width: 72
                        height: 36
                        radius: 4
                        color: "#F1F1EF"

                        Text {
                            anchors.centerIn: parent
                            text: "选择…"
                            font.pixelSize: 12
                            color: "#37352F"
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: folderDialog.open()
                        }
                    }
                }

                SectionTitle { text: "文件格式" }
                ComboBox {
                    Layout.preferredWidth: 200
                    model: ["mp4", "mkv"]
                    onActivated: (index) => app.setFormat(currentText)
                }

                SwitchRow {
                    label: "保存后弹出通知"
                    checked: true
                }

                Item { Layout.fillHeight: true }
            }

            // ---------- 视频 ----------
            ColumnLayout {
                visible: dlg.page === "video"
                anchors.fill: parent
                spacing: 18

                SectionTitle { text: "录制模式" }
                ComboBox {
                    Layout.preferredWidth: 220
                    model: ["全屏录制", "区域录制", "窗口录制"]
                    onActivated: (index) => app.setCaptureMode(index)
                }

                SectionTitle { text: "帧率" }
                ComboBox {
                    Layout.preferredWidth: 220
                    model: ["自动（跟随屏幕刷新率）", "24 FPS", "30 FPS", "60 FPS", "120 FPS"]
                    currentIndex: 3
                    onActivated: (index) => {
                        const fps = [0, 24, 30, 60, 120][index]
                        app.setFps(fps)
                    }
                }

                SwitchRow {
                    label: "录制光标"
                    checked: true
                    onToggled: (value) => app.setCaptureCursor(value)
                }

                SwitchRow {
                    label: "点击特效"
                    checked: false
                    onToggled: (value) => app.setClickEffects(value)
                }

                SectionTitle { text: "编码器" }
                ComboBox {
                    Layout.preferredWidth: 220
                    model: ["自动（优先硬件编码）", "H.264", "H.265"]
                    onActivated: (index) => {
                        const codec = ["auto", "h264", "h265"][index]
                        app.setCodec(codec)
                    }
                }

                SectionTitle { text: "码率控制" }
                ComboBox {
                    Layout.preferredWidth: 220
                    model: ["质量优先（CRF）", "文件大小优先（CBR）"]
                    currentIndex: 1
                    onActivated: (index) => app.setBitrateMode(index)
                }

                SliderRow {
                    id: bitrateSlider
                    label: "码率"
                    value: 6000
                    from: 1000
                    to: 20000
                    stepSize: 500
                    onChanged: (value) => app.setBitrateKbps(value)
                    visible: true
                }

                SliderRow {
                    id: crfSlider
                    label: "CRF"
                    value: 18
                    from: 0
                    to: 51
                    stepSize: 1
                    onChanged: (value) => app.setCrf(value)
                }

                Item { Layout.fillHeight: true }
            }

            // ---------- 音频 ----------
            ColumnLayout {
                visible: dlg.page === "audio"
                anchors.fill: parent
                spacing: 18

                SwitchRow {
                    label: "录制系统声音"
                    checked: true
                    onToggled: (value) => app.setSystemAudio(value)
                }

                SliderRow {
                    label: "系统音量"
                    value: 100
                    onChanged: (value) => app.setSystemVolume(value)
                }

                SwitchRow {
                    label: "录制麦克风"
                    checked: false
                    onToggled: (value) => app.setMicrophone(value)
                }

                SliderRow {
                    label: "麦克风音量"
                    value: 100
                    onChanged: (value) => app.setMicVolume(value)
                }

                SwitchRow {
                    label: "降噪（WebRTC）"
                    checked: false
                    onToggled: (value) => app.setDenoise(value)
                }

                Text {
                    text: "降噪将在后续版本中启用"
                    font.pixelSize: 11
                    color: "#D3D1CA"
                }

                Item { Layout.fillHeight: true }
            }

            // ---------- 热键 ----------
            ColumnLayout {
                visible: dlg.page === "hotkeys"
                anchors.fill: parent
                spacing: 20

                SectionTitle { text: "开始 / 停止录制" }
                Rectangle {
                    Layout.preferredWidth: 220
                    height: 34
                    radius: 4
                    color: "#F7F7F5"
                    border.color: "#E3E2E0"
                    Text {
                        anchors.centerIn: parent
                        text: "⌘⇧R"
                        font.pixelSize: 13
                        color: "#37352F"
                    }
                }

                SectionTitle { text: "暂停 / 继续" }
                Rectangle {
                    Layout.preferredWidth: 220
                    height: 34
                    radius: 4
                    color: "#F7F7F5"
                    border.color: "#E3E2E0"
                    Text {
                        anchors.centerIn: parent
                        text: "⌘⇧P"
                        font.pixelSize: 13
                        color: "#37352F"
                    }
                }

                Text {
                    text: "全局热键注册将在后续版本启用"
                    font.pixelSize: 11
                    color: "#D3D1CA"
                }

                Item { Layout.fillHeight: true }
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: "选择保存目录"
        onAccepted: {
            const url = folderDialog.selectedFolder.toString()
            const path = decodeURIComponent(url.replace(/^file:\/\//, ""))
            app.setOutputDir(path)
            outputDirField = path
        }
    }

    property string outputDirField: ""
}
