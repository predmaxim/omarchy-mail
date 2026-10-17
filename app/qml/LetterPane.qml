import QtQuick
import QtQuick.Templates as T
import OmMail

// The open letter (spec «Окно», «Письмо внутри»): subject, sender, recipients and date, an unsent mail's state,
// the attachments right under the header, then the body that LetterView paints in the theme's colours.
// Mail strings are plain text. `nav` rows here: Outbox 4, Attachments 5, Links 6 (om::Row).
Item {
    id: pane
    readonly property alias view: body
    readonly property int pad: theme.space(18)
    readonly property QtObject shown: letter   // inside LetterView `letter` and `theme` are its own properties
    readonly property QtObject colours: theme

    component PaneButton: Rectangle {   // "Send again" / "Discard" under a mail that wasn't sent
        id: button
        required property int col
        property string label
        signal pressed()
        readonly property bool hasCursor: (nav.row === 4 && nav.col === col) || mouse.containsMouse
        implicitWidth: buttonText.implicitWidth + 2 * theme.space(10)
        implicitHeight: buttonText.implicitHeight + 2 * theme.space(4)
        radius: theme.space(8)
        color: hasCursor ? theme.hoverFill : "transparent"
        border.width: 1
        border.color: hasCursor ? theme.cursorBorder : theme.muted
        Text {
            id: buttonText
            anchors.centerIn: parent
            text: button.label
            color: theme.foreground
            font.pixelSize: theme.fontBody
        }
        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: button.pressed()
        }
    }

    Column {
        id: head
        x: pane.pad
        y: pane.pad
        width: pane.width - 2 * pane.pad
        spacing: theme.space(6)

        Text {
            objectName: "letterSubject"
            readonly property bool none: letter.subject.trim() === ""
            width: parent.width
            visible: !letter.empty
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: none ? i18n.t("(no subject)") : letter.subject
            color: none ? theme.dim : theme.foreground
            font.pixelSize: theme.fontDisplay
            font.bold: true
        }
        Text {
            width: parent.width
            visible: text !== ""
            elide: Text.ElideRight
            textFormat: Text.PlainText
            text: letter.from
            color: theme.foreground
            font.pixelSize: theme.fontTitle
        }
        Text {
            objectName: "letterMeta"
            width: parent.width
            visible: text !== ""
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: [letter.to !== "" ? i18n.t("to: %1").arg(letter.to) : "", letter.date].filter(s => s !== "").join(" · ")
            color: theme.dim
            font.pixelSize: theme.fontSubtitle
        }
        Text {
            visible: letter.outbox === 1
            text: i18n.t("Queued — will be sent when online")
            color: theme.dim
            font.pixelSize: theme.fontBody
        }
        Flow {
            width: parent.width
            visible: letter.outbox === 2
            spacing: theme.space(8)
            Text {
                width: Math.min(implicitWidth, parent.width)
                height: Math.max(implicitHeight, retry.height)
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: i18n.t("Not sent: %1").arg(letter.sendError)
                color: theme.urgent
                font.pixelSize: theme.fontBody
            }
            PaneButton { id: retry; col: 0; label: i18n.t("Send again"); onPressed: app.retryOutbox() }
            PaneButton { col: 1; label: i18n.t("Discard"); onPressed: app.discardOutbox() }
        }
        Flow {
            width: parent.width
            visible: attachments.count > 0
            topPadding: theme.space(6)
            spacing: theme.space(8)
            Repeater {
                id: attachments
                objectName: "attachments"
                model: letter.attachments
                delegate: Rectangle {
                    id: chip
                    required property var modelData
                    required property int index
                    readonly property bool hasCursor: (nav.row === 5 && nav.col === index) || chipMouse.containsMouse
                    width: Math.min(chipText.implicitWidth + 2 * theme.space(10), head.width)
                    height: chipText.implicitHeight + 2 * theme.space(4)
                    radius: theme.space(8)
                    color: hasCursor ? theme.hoverFill : "transparent"
                    border.width: 1
                    border.color: hasCursor ? theme.cursorBorder : theme.muted
                    Text {
                        id: chipText
                        anchors.centerIn: parent
                        width: parent.width - 2 * theme.space(10)
                        elide: Text.ElideMiddle
                        textFormat: Text.PlainText
                        text: (chip.modelData.busy ? "…" : chip.modelData.icon) + " " + chip.modelData.name + " · " + chip.modelData.sizeText
                        color: theme.foreground
                        font.pixelSize: theme.fontBody
                    }
                    MouseArea {
                        id: chipMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: letter.openAttachment(chip.index)
                    }
                }
            }
        }
        Text {
            visible: letter.state !== ""
            text: letter.state === "remote" ? i18n.t("Downloading from the server…") : i18n.t("Loading…")
            color: theme.dim
            font.pixelSize: theme.fontBody
        }
        Text {
            width: parent.width
            visible: letter.error !== ""
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: letter.error
            color: theme.urgent
            font.pixelSize: theme.fontBody
        }
    }

    LetterView {
        id: body
        objectName: "letterView"
        anchors.top: head.bottom
        anchors.topMargin: pane.pad
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        letter: pane.shown
        theme: pane.colours
        currentLink: nav.row === 6 ? nav.col : -1
    }

    Text {
        objectName: "noText"
        anchors.centerIn: body
        visible: letter.blank
        text: i18n.t("No content")
        color: theme.dim
        font.pixelSize: theme.fontBody
    }

    T.ScrollBar {
        anchors.top: body.top
        anchors.bottom: body.bottom
        anchors.right: body.right
        orientation: Qt.Vertical
        padding: theme.space(2)
        implicitWidth: implicitContentWidth + leftPadding + rightPadding
        size: body.contentHeight > 0 ? Math.min(1, body.height / body.contentHeight) : 1
        position: body.contentHeight > 0 ? body.scrollY / body.contentHeight : 0
        visible: size < 1
        onPositionChanged: if (pressed) body.scrollY = position * body.contentHeight
        contentItem: Rectangle {
            implicitWidth: theme.space(4)
            radius: width / 2
            color: theme.muted
        }
    }

    Shortcut {   // the field copies its own text: it takes Ctrl+C before the window's shortcuts
        sequences: [StandardKey.Copy]
        onActivated: body.copySelection()
    }
}
