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
    property TextEdit picked: null   // the header line selected in last: Ctrl+C copies it before the letter
    signal pressed()   // a selection starts in the header or the letter: the window takes the keys back

    component HeadText: TextEdit {   // a header line selects like the letter; the keys stay the window's
        id: line
        readOnly: true
        selectByMouse: true
        activeFocusOnPress: false
        wrapMode: TextEdit.Wrap
        textFormat: TextEdit.PlainText
        selectionColor: theme.selection
        selectedTextColor: color
        onSelectedTextChanged: {
            if (selectedText === "")
                return
            if (pane.picked && pane.picked !== line)
                pane.picked.deselect()
            pane.picked = line
            pane.pressed()
        }
    }

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

    Rectangle {   // the header band over the pane's whole width, a line under it
        objectName: "headBand"
        visible: !letter.empty
        width: parent.width
        height: head.y + head.height + theme.space(17) + 1
        color: theme.surface
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: theme.muted
        }
    }

    Column {
        id: head
        x: theme.space(27)
        y: theme.space(20)
        width: pane.width - 2 * x
        spacing: theme.space(6)

        HeadText {
            objectName: "letterSubject"
            readonly property bool none: letter.subject.trim() === ""
            width: parent.width
            visible: !letter.empty
            text: none ? i18n.t("(no subject)") : letter.subject
            color: none ? theme.dim : theme.foreground
            font.pixelSize: theme.fontDisplay
            font.bold: true
        }
        HeadText {
            width: parent.width
            visible: text !== ""
            text: letter.from
            color: theme.foreground
            font.pixelSize: theme.fontTitle
        }
        HeadText {
            objectName: "letterMeta"
            width: parent.width
            visible: text !== ""
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
            topPadding: theme.space(9)
            spacing: theme.space(9)
            Repeater {
                id: attachments
                objectName: "attachments"
                model: letter.attachments
                delegate: Plaque {
                    id: chip
                    required property var modelData
                    required property int index
                    icon: modelData.busy ? "…" : modelData.icon
                    name: modelData.name
                    size: modelData.sizeText
                    lit: (nav.row === 5 && nav.col === index) || chipMouse.containsMouse
                    maxWidth: head.width
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
        anchors.topMargin: theme.space(17) + 1 + pane.pad
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

    Connections {   // a press in the letter starts its own selection
        target: body
        function onPressed() {
            if (pane.picked)
                pane.picked.deselect()
            pane.pressed()
        }
    }

    Shortcut {   // the field copies its own text: it takes Ctrl+C before the window's shortcuts
        sequences: [StandardKey.Copy]
        onActivated: {
            if (pane.picked && pane.picked.selectedText !== "")
                pane.picked.copy()
            else
                body.copySelection()
        }
    }
}
