import QtQuick

// The line of commands on top (spec «Окно»): "Mail", Compose / Reply / Delete with their keys; on the right the
// last note or the mailboxes' status, the Unread filter and the mailboxes page. The window cursor on the row
// Actions lights the button `nav.col`; a button's `col` is App::pressAction's. On the mailboxes page: "Mail",
// the note or status and the back arrow in the gear's place (rules §9).
Item {
    id: bar
    implicitHeight: Math.max(commands.implicitHeight, toggles.implicitHeight) + 2 * theme.space(8) + 1
    readonly property bool main: app.page === "main"

    component KeyCap: Rectangle {   // a key in a frame; Main uses it for the search field's key
        property alias text: keyText.text
        implicitWidth: keyText.implicitWidth + 2 * theme.space(4)
        implicitHeight: keyText.implicitHeight
        radius: theme.space(4)
        color: "transparent"
        border.width: 1
        border.color: theme.muted
        Text {
            id: keyText
            anchors.centerIn: parent
            color: theme.dim
            font.pixelSize: theme.fontBody
        }
    }

    component ActionButton: Rectangle {
        id: button
        required property int col
        property string label
        property string key
        property bool current: false
        property int labelSize: theme.fontTitle
        readonly property bool hasCursor: (bar.main && nav.row === 0 && nav.col === col) || mouse.containsMouse
        anchors.verticalCenter: parent.verticalCenter   // in its Row: the gear's icon is taller than the labels
        implicitWidth: content.implicitWidth + 2 * theme.space(8)
        implicitHeight: content.implicitHeight + 2 * theme.space(4)
        radius: theme.space(8)
        color: current ? theme.currentFill : hasCursor ? theme.hoverFill : "transparent"
        border.width: 1
        border.color: hasCursor ? theme.cursorBorder : "transparent"
        Row {
            id: content
            anchors.centerIn: parent
            spacing: theme.space(6)
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: button.label
                color: theme.foreground
                font.pixelSize: button.labelSize
            }
            KeyCap {
                anchors.verticalCenter: parent.verticalCenter
                visible: button.key !== ""
                text: button.key
            }
        }
        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: app.pressAction(button.col)
        }
    }

    Row {
        id: commands
        x: theme.space(18)
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: -1
        spacing: theme.space(8)
        Text {
            anchors.verticalCenter: parent.verticalCenter
            rightPadding: theme.space(10)
            text: i18n.t("Mail")
            color: theme.foreground
            font.pixelSize: theme.fontTitle
            font.bold: true
        }
        ActionButton { col: 0; label: i18n.t("Compose"); key: "c"; visible: bar.main }
        ActionButton { col: 1; label: i18n.t("Reply"); key: "r"; visible: bar.main }
        ActionButton { col: 2; label: i18n.t("Delete"); key: "d"; visible: bar.main }
    }

    Text {   // the service's words or ours: plain, they may quote a server
        anchors.left: commands.right
        anchors.right: toggles.left
        anchors.margins: theme.space(18)
        anchors.verticalCenter: commands.verticalCenter
        horizontalAlignment: Text.AlignRight
        elide: Text.ElideRight
        textFormat: Text.PlainText
        text: app.note !== "" ? app.note : app.statusText
        color: app.note !== "" ? (app.noteError ? theme.urgent : theme.foreground)
                               : (app.statusError ? theme.urgent : theme.dim)
        font.pixelSize: theme.fontBody
    }

    Row {
        id: toggles
        anchors.right: parent.right
        anchors.rightMargin: theme.space(18)
        anchors.verticalCenter: commands.verticalCenter
        spacing: theme.space(8)
        ActionButton { col: 3; label: i18n.t("Unread"); key: "Shift+U"; current: app.unreadOnly; visible: bar.main }
        ActionButton { col: 4; label: bar.main ? "󰒓" : "󰁍"; labelSize: theme.iconLarge }   // the gear, the back arrow
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: theme.muted
    }
}
