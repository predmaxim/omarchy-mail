import QtQuick
import QtQuick.Dialogs

// The compose form (spec «Написание»): From (only with several mailboxes), To with address suggestions, Subject,
// the attachments right under them, the text, a note and the key hints. Every field hands its keys to `app` first
// (Ctrl+Enter, Ctrl+V with a picture, Ctrl+O, Ctrl+D, Esc); the rest is typing. Suggestion and file names are plain text.
Item {
    id: pane
    objectName: "composePane"
    visible: compose.open
    readonly property int pad: theme.space(18)
    property int suggestion: -1   // the highlighted suggestion; -1: none
    signal closed()               // the window takes its keys back

    function appKey(e) { return app.key(e.key, e.nativeScanCode, e.modifiers, e.isAutoRepeat, "compose") }
    function lastAddress(text) { return text.split(/[,;]/).pop().trim() }

    onVisibleChanged: {
        if (!visible) {
            closed()
            return
        }
        Qt.callLater(() => {
            if (compose.isReply) {
                body.forceActiveFocus()
                body.cursorPosition = Math.min(compose.bodyCursor, body.length)
            } else {
                toField.forceActiveFocus()
            }
        })
    }
    Connections {
        target: compose
        function onChanged() {
            if (compose.suggestions.length === 0 || pane.suggestion >= compose.suggestions.length)
                pane.suggestion = -1
        }
    }
    Connections {
        target: app
        function onPickFiles() { files.open() }
    }
    FileDialog {
        id: files
        fileMode: FileDialog.OpenFiles
        onAccepted: compose.addFiles(selectedFiles)
    }
    DropArea {
        anchors.fill: parent
        onDropped: drop => compose.addFiles(drop.urls)
    }

    component Field: Rectangle {   // a framed line: caption on the left, the input inside
        id: field
        property string caption
        default property alias input: slot.data
        width: parent.width
        height: Math.max(theme.space(40), slot.childrenRect.height + 2 * theme.space(8))
        radius: theme.space(8)
        color: "transparent"
        border.width: 1
        border.color: slot.children.length > 0 && slot.children[0].activeFocus ? theme.accent : theme.muted
        Text {
            id: label
            x: theme.space(12)
            width: theme.space(80)
            anchors.verticalCenter: parent.verticalCenter
            text: field.caption.toUpperCase()
            color: theme.dim
            font.pixelSize: theme.fontBody
            font.bold: true
            font.letterSpacing: 1.2
        }
        Item {
            id: slot
            x: label.x + label.width
            width: parent.width - x - theme.space(12)
            anchors.verticalCenter: parent.verticalCenter
            height: childrenRect.height
        }
    }

    Column {
        id: head
        x: pane.pad
        y: pane.pad
        width: pane.width - 2 * pane.pad
        spacing: theme.space(8)

        Text {
            text: compose.isReply ? i18n.t("Replying") : i18n.t("New message")
            color: theme.foreground
            font.pixelSize: theme.fontTitle
            font.bold: true
            bottomPadding: theme.space(4)
        }
        Row {
            visible: app.accounts.length > 1
            width: parent.width
            Text {
                width: theme.space(92)
                anchors.verticalCenter: parent.verticalCenter
                text: i18n.t("From").toUpperCase()
                color: theme.dim
                font.pixelSize: theme.fontBody
                font.bold: true
                font.letterSpacing: 1.2
            }
            Picker {
                objectName: "composeFrom"
                width: parent.width - theme.space(92)
                choices: app.accounts.map(a => a.email)
                current: choices.indexOf(compose.account)
                onPicked: i => compose.account = choices[i]
            }
        }
        Field {
            caption: i18n.t("To")
            TextInput {
                id: toField
                objectName: "composeTo"
                width: parent.width
                clip: true
                text: compose.to
                color: theme.foreground
                selectionColor: theme.selection
                selectedTextColor: theme.foreground
                font.pixelSize: theme.fontTitle
                KeyNavigation.tab: subject
                onTextEdited: {
                    compose.to = text
                    app.suggest(pane.lastAddress(text))
                }
                Keys.onPressed: e => {
                    const n = compose.suggestions.length
                    if (n > 0 && (e.key === Qt.Key_Down || e.key === Qt.Key_Up)) {
                        pane.suggestion = e.key === Qt.Key_Down ? Math.min(n - 1, pane.suggestion + 1) : Math.max(0, pane.suggestion - 1)
                        e.accepted = true
                    } else if (n > 0 && e.modifiers === Qt.NoModifier && (e.key === Qt.Key_Return || e.key === Qt.Key_Enter || e.key === Qt.Key_Tab)) {
                        compose.pickSuggestion(Math.max(0, pane.suggestion))
                        cursorPosition = length
                        e.accepted = true
                    } else {
                        e.accepted = pane.appKey(e)
                    }
                }
            }
        }
        Item {   // the suggestions, over what follows (they take no room)
            width: parent.width
            height: 0
            z: 1
            Column {
                visible: compose.suggestions.length > 0
                width: parent.width
                Repeater {
                    model: compose.suggestions
                    delegate: Rectangle {
                        id: sug
                        required property var modelData
                        required property int index
                        width: parent.width
                        height: sugText.implicitHeight + 2 * theme.space(6)
                        color: index === pane.suggestion ? theme.currentFill : theme.background
                        border.width: 1
                        border.color: theme.muted
                        Text {
                            id: sugText
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.margins: theme.space(12)
                            anchors.verticalCenter: parent.verticalCenter
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: sug.modelData.name + (sug.modelData.name !== "" ? "  " : "") + sug.modelData.addr
                            color: theme.foreground
                            font.pixelSize: theme.fontTitle
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                compose.pickSuggestion(sug.index)
                                toField.forceActiveFocus()
                                toField.cursorPosition = toField.length
                            }
                        }
                    }
                }
            }
        }
        Field {
            id: subjectField
            caption: i18n.t("Subject")
            TextInput {
                id: subject
                objectName: "composeSubject"
                width: parent.width
                clip: true
                text: compose.subject
                color: theme.foreground
                selectionColor: theme.selection
                selectedTextColor: theme.foreground
                font.pixelSize: theme.fontTitle
                KeyNavigation.tab: body
                KeyNavigation.backtab: toField
                onTextEdited: compose.subject = text
                Keys.onPressed: e => e.accepted = pane.appKey(e)
            }
        }
        Flow {
            width: parent.width
            visible: attachments.count > 0
            spacing: theme.space(8)
            Repeater {
                id: attachments
                objectName: "composeAttachments"
                model: compose.attachments
                delegate: Rectangle {
                    id: chip
                    required property var modelData
                    required property int index
                    width: Math.min(chipText.implicitWidth + 2 * theme.space(10), head.width)
                    height: chipText.implicitHeight + 2 * theme.space(4)
                    radius: theme.space(8)
                    color: activeFocus ? theme.hoverFill : "transparent"
                    border.width: 1
                    border.color: activeFocus ? theme.cursorBorder : theme.muted
                    activeFocusOnTab: true
                    Keys.onPressed: e => {
                        if (e.key === Qt.Key_Delete && e.modifiers === Qt.NoModifier) {
                            compose.removeAttachment(index)
                            e.accepted = true
                        } else {
                            e.accepted = pane.appKey(e)
                        }
                    }
                    Text {
                        id: chipText
                        anchors.centerIn: parent
                        width: parent.width - 2 * theme.space(10)
                        elide: Text.ElideMiddle
                        textFormat: Text.PlainText
                        text: chip.modelData.icon + " " + chip.modelData.name + " · " + chip.modelData.sizeText + "  ✕"
                        color: theme.foreground
                        font.pixelSize: theme.fontBody
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: compose.removeAttachment(chip.index)   // ponytail: the whole plaque removes; the file is only attached, never deleted
                    }
                }
            }
        }
    }

    Flickable {
        id: flick
        anchors.top: head.bottom
        anchors.topMargin: theme.space(12)
        anchors.bottom: foot.top
        anchors.bottomMargin: theme.space(8)
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: pane.pad
        anchors.rightMargin: pane.pad
        clip: true
        contentWidth: width
        contentHeight: Math.max(height, body.implicitHeight)
        boundsBehavior: Flickable.StopAtBounds
        function follow() {
            const r = body.cursorRectangle
            if (r.y < contentY)
                contentY = r.y
            else if (r.y + r.height > contentY + height)
                contentY = r.y + r.height - height
        }
        MouseArea {
            anchors.fill: parent
            onClicked: { body.forceActiveFocus(); body.cursorPosition = body.length }
        }
        TextEdit {
            id: body
            objectName: "composeBody"
            width: flick.width
            height: flick.contentHeight
            wrapMode: TextEdit.Wrap
            text: compose.body
            color: theme.foreground
            selectionColor: theme.selection
            selectedTextColor: theme.foreground
            font.family: "monospace"
            font.pixelSize: theme.fontTitle
            KeyNavigation.backtab: subject
            onTextChanged: if (text !== compose.body) compose.body = text
            onCursorRectangleChanged: flick.follow()
            Keys.onPressed: e => e.accepted = pane.appKey(e)
        }
    }

    Column {
        id: foot
        anchors.bottom: parent.bottom
        anchors.bottomMargin: pane.pad
        x: pane.pad
        width: pane.width - 2 * pane.pad
        spacing: theme.space(4)
        Text {
            width: parent.width
            visible: text !== ""
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: compose.note
            color: compose.noteError ? theme.urgent : theme.dim
            font.pixelSize: theme.fontBody
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: i18n.t("Ctrl+Enter — send · Ctrl+V — paste screenshot · Ctrl+O — attach file · Esc — close (draft is kept)")
            color: theme.dim
            font.pixelSize: theme.fontBody
        }
    }
}
