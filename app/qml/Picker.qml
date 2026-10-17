import QtQuick
import QtQuick.Templates as T

// A drop-down (spec «Окно»: the mailbox, the folder): a framed line with the choice and ▾; opened, its list in a
// Popup that keeps the keys itself (↑/↓, Enter, Esc) and follows the mouse. `choices`: strings, or
// {label, unread, separatorBefore}. Names come from the servers: plain text.
Rectangle {
    id: picker
    property var choices: []
    property int current: -1
    property bool hasCursor: false   // the window cursor (`nav`) is on it
    signal picked(int index)

    function open() { popup.open() }
    function pick(i) {
        popup.close()
        picker.picked(i)
    }
    function labelOf(c) { return c === undefined ? "" : typeof c === "string" ? c : c.label }

    implicitHeight: label.implicitHeight + 2 * theme.space(6)
    radius: theme.space(8)
    color: hasCursor || mouse.containsMouse ? theme.hoverFill : "transparent"
    border.width: 1
    border.color: popup.visible ? theme.accent : theme.muted

    Text {
        id: label
        anchors.left: parent.left
        anchors.right: arrow.left
        anchors.leftMargin: theme.space(12)
        anchors.rightMargin: theme.space(6)
        anchors.verticalCenter: parent.verticalCenter
        elide: Text.ElideRight
        textFormat: Text.PlainText
        text: picker.labelOf(picker.choices[picker.current])
        color: theme.foreground
        font.pixelSize: theme.fontBody
    }
    Text {
        id: arrow
        anchors.right: parent.right
        anchors.rightMargin: theme.space(12)
        anchors.verticalCenter: parent.verticalCenter
        text: "▾"
        color: theme.dim
        font.pixelSize: theme.fontBody
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: popup.open()
    }

    T.Popup {
        id: popup
        y: picker.height + theme.space(4)
        width: picker.width
        implicitHeight: view.contentHeight + topPadding + bottomPadding
        padding: theme.space(4)
        margins: theme.space(8)
        focus: true
        onAboutToShow: view.currentIndex = Math.max(0, picker.current)

        background: Rectangle {
            color: theme.background
            radius: theme.space(8)
            border.width: 1
            border.color: theme.accent
        }
        contentItem: ListView {
            id: view
            clip: true
            focus: true
            boundsBehavior: Flickable.StopAtBounds
            model: picker.choices
            Keys.onReturnPressed: picker.pick(currentIndex)
            Keys.onEnterPressed: picker.pick(currentIndex)

            delegate: Item {
                id: entry
                required property var modelData
                required property int index
                readonly property bool separated: typeof modelData !== "string" && modelData.separatorBefore === true
                width: ListView.view.width
                height: choice.height + choice.y

                Rectangle {
                    visible: entry.separated
                    y: theme.space(4)
                    x: theme.space(2)
                    width: parent.width - 2 * x
                    height: 1
                    color: theme.muted
                }
                Rectangle {
                    id: choice
                    readonly property bool current: entry.index === picker.current
                    readonly property bool hasCursor: entry.index === view.currentIndex && !current
                    y: entry.separated ? theme.space(9) : 0
                    width: parent.width
                    height: name.implicitHeight + 2 * theme.space(5)
                    radius: theme.space(6)
                    color: current ? theme.currentFill : hasCursor ? theme.hoverFill : "transparent"
                    border.width: 1
                    border.color: hasCursor ? theme.cursorBorder : "transparent"

                    Text {
                        id: name
                        anchors.left: parent.left
                        anchors.right: unread.left
                        anchors.leftMargin: theme.space(8)
                        anchors.rightMargin: theme.space(8)
                        anchors.verticalCenter: parent.verticalCenter
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        text: picker.labelOf(entry.modelData)
                        color: theme.foreground
                        font.pixelSize: theme.fontBody
                    }
                    Text {
                        id: unread
                        anchors.right: parent.right
                        anchors.rightMargin: theme.space(8)
                        anchors.verticalCenter: parent.verticalCenter
                        text: entry.modelData.unread > 0 ? entry.modelData.unread : ""
                        color: theme.dim
                        font.pixelSize: theme.fontBody
                    }
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: view.currentIndex = entry.index
                        onClicked: picker.pick(entry.index)
                    }
                }
            }
        }
    }
}
