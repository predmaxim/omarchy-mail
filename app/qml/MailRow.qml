import QtQuick

// One letter of the list (spec «Окно», «Строка письма»): sender, marks and date; subject; preview; the mailbox
// and folder. Mail strings are plain text but subjectRich / previewRich, which highlight::styled escaped. The
// mouse lights the row but never moves the list cursor: the cursor opens the letter.
Rectangle {
    id: row
    required property int index
    required property string sender
    required property string subject
    required property string subjectRich
    required property string previewRich
    required property string dateText
    required property string place
    required property bool seen
    required property bool flagged
    required property bool attachments
    required property bool queued
    required property bool failed
    required property bool selected
    required property bool isCursor
    signal clicked(int modifiers)

    readonly property bool current: isCursor || selected
    readonly property int padX: theme.space(12)
    readonly property int padY: theme.space(8)

    width: ListView.view ? ListView.view.width : 0
    height: lines.implicitHeight + 2 * padY
    radius: theme.space(8)
    color: current ? theme.currentFill : mouse.containsMouse ? theme.hoverFill : "transparent"
    border.width: 1
    border.color: (!current && mouse.containsMouse) || (isCursor && list.selectedCount > 0) ? theme.cursorBorder : "transparent"
    Behavior on color { ColorAnimation { duration: 60 } }

    Rectangle {
        objectName: "unreadBar"
        visible: !row.seen
        x: theme.space(4)
        anchors.verticalCenter: parent.verticalCenter
        width: theme.space(3)
        height: parent.height - 2 * row.padY
        radius: width / 2
        color: theme.accent
    }

    Column {
        id: lines
        x: row.padX
        y: row.padY
        width: row.width - 2 * row.padX
        spacing: theme.space(3)

        Item {
            width: parent.width
            height: senderText.implicitHeight
            Text {
                id: senderText
                anchors.left: parent.left
                anchors.right: marks.left
                anchors.rightMargin: theme.space(8)
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: row.sender
                color: theme.foreground
                font.pixelSize: theme.fontHeading
            }
            Row {
                id: marks
                anchors.right: parent.right
                y: senderText.baselineOffset - date.baselineOffset
                spacing: theme.space(6)
                Text {
                    visible: row.attachments
                    text: "󰏢"
                    color: theme.dim
                    font.pixelSize: theme.fontBody
                }
                Text {
                    visible: row.flagged
                    text: "󰈻"
                    color: theme.accent
                    font.pixelSize: theme.fontBody
                }
                Text {
                    id: date
                    text: row.failed ? i18n.t("not sent") : row.queued ? i18n.t("queued") : row.dateText
                    color: row.failed ? theme.urgent : theme.dim
                    font.pixelSize: theme.fontBody
                }
            }
        }
        Text {
            objectName: "subject"
            readonly property bool none: row.subject.trim() === ""
            width: parent.width
            elide: Text.ElideRight
            textFormat: none ? Text.PlainText : Text.StyledText
            text: none ? i18n.t("(no subject)") : row.subjectRich
            color: none ? theme.dim : theme.foreground
            font.pixelSize: theme.fontTitle
        }
        Text {
            width: parent.width
            visible: row.previewRich !== ""   // server search hits come without one
            elide: Text.ElideRight
            textFormat: Text.StyledText
            text: row.previewRich
            color: theme.dim
            font.pixelSize: theme.fontBody
        }
        Text {
            width: parent.width
            visible: row.place !== ""
            elide: Text.ElideRight
            textFormat: Text.PlainText
            text: row.place
            color: theme.dim
            font.pixelSize: theme.fontBody
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: e => row.clicked(e.modifiers)
    }
}
