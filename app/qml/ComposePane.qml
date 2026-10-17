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

    // The Tab chain: From (when shown), To, Subject, every plaque, the text; it wraps. Tab in the text moves on, no tab characters.
    function chain() {
        const out = fromRow.visible ? [from] : []
        out.push(toField, subject)
        for (let i = 0; i < attachments.count; ++i)
            out.push(attachments.itemAt(i))
        out.push(body)
        return out
    }
    function step(dir) {
        const c = chain()
        let i = c.findIndex(x => x.activeFocus)
        c[(i + dir + c.length) % c.length].forceActiveFocus()
    }
    function removeAt(i) {   // the plaque dies with the model: the focus is placed from here
        compose.removeAttachment(i)
        Qt.callLater(focusPlaque, i)
    }
    function focusPlaque(i) {   // after a removal: the next plaque, else the previous, else the text
        const n = attachments.count
        const next = n > 0 ? attachments.itemAt(Math.min(i, n - 1)) : body
        next.forceActiveFocus()
    }
    function appKey(e) {
        if (e.modifiers === Qt.NoModifier && e.key === Qt.Key_Tab || e.modifiers === Qt.ShiftModifier && e.key === Qt.Key_Backtab) {
            step(e.key === Qt.Key_Tab ? 1 : -1)
            return true
        }
        return app.key(e.key, e.nativeScanCode, e.modifiers, e.isAutoRepeat, "compose")
    }
    function lastAddress(text) { return text.split(/[,;]/).pop().trim() }
    function pick(i) {   // here, not in the row: the rows go with the pick
        compose.pickSuggestion(i)
        toField.forceActiveFocus()
        toField.cursorPosition = toField.length
    }

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
            id: fromRow
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
                id: from
                objectName: "composeFrom"
                beforeKeys: e => pane.appKey(e)
                width: parent.width - theme.space(92)
                choices: app.accounts.map(a => a.email)
                current: choices.indexOf(compose.account)
                onPicked: i => compose.account = choices[i]
            }
        }
        Field {
            id: toFrame
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
                onTextEdited: {
                    pane.suggestion = -1   // new rows are coming: nothing is highlighted until ↓ / ↑
                    compose.to = text
                    app.suggest(pane.lastAddress(text))
                }
                Keys.onPressed: e => {   // only a highlighted row is taken: contacts match inside an address too
                    const n = compose.suggestions.length
                    if (n > 0 && (e.key === Qt.Key_Down || e.key === Qt.Key_Up)) {
                        pane.suggestion = e.key === Qt.Key_Down ? Math.min(n - 1, pane.suggestion + 1) : Math.max(0, pane.suggestion - 1)
                        e.accepted = true
                    } else if (pane.suggestion >= 0 && e.modifiers === Qt.NoModifier && (e.key === Qt.Key_Return || e.key === Qt.Key_Enter || e.key === Qt.Key_Tab)) {
                        pane.pick(pane.suggestion)
                        e.accepted = true
                    } else {
                        e.accepted = pane.appKey(e)
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
                onTextEdited: compose.subject = text
                Keys.onPressed: e => e.accepted = pane.appKey(e)
            }
        }
        Flow {
            width: parent.width
            visible: attachments.count > 0
            spacing: theme.space(12)
            Repeater {
                id: attachments
                objectName: "composeAttachments"
                model: compose.attachments
                delegate: Plaque {
                    id: chip
                    required property var modelData
                    required property int index
                    icon: modelData.icon
                    name: modelData.name
                    size: modelData.sizeText
                    tail: "✕"
                    lit: activeFocus
                    maxWidth: head.width
                    Keys.onPressed: e => {
                        if ((e.key === Qt.Key_Delete || e.key === Qt.Key_Backspace) && e.modifiers === Qt.NoModifier) {
                            pane.removeAt(index)
                            e.accepted = true
                        } else {
                            e.accepted = pane.appKey(e)
                        }
                    }
                    MouseArea {   // a click focuses the plaque; only the ✕ removes
                        anchors.fill: parent
                        onClicked: m => {
                            if (m.x > parent.width - chip.padX - cross.implicitWidth - theme.space(4))
                                pane.removeAt(chip.index)
                            else
                                chip.forceActiveFocus()
                        }
                    }
                    TextMetrics { id: cross; font.pixelSize: theme.fontBody; text: "✕" }
                }
            }
        }
    }

    Column {   // the suggestions under To, over what follows; outside `head`: a positioner skips a 0-high item
        x: head.x
        y: head.y + toFrame.y + toFrame.height
        z: 1
        width: head.width
        visible: compose.suggestions.length > 0 && toField.activeFocus   // over Subject only while To is typed in
        Repeater {
            objectName: "composeSuggestions"
            model: compose.suggestions
            delegate: Rectangle {
                id: sug
                required property var modelData
                required property int index
                width: parent.width
                height: sugText.implicitHeight + 2 * theme.space(6)
                color: index === pane.suggestion ? Qt.tint(theme.background, theme.currentFill) : theme.background   // opaque: over the fields
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
                    onClicked: pane.pick(sug.index)
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
