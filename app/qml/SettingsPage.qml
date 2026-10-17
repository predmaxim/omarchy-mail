import QtQuick

// The mailboxes page (spec «Ящики и вход»; rules §9: no tabs, Esc — back): the mailboxes with their state, then
// "Add a mailbox". ↑/↓ walk the mailboxes and the form; on a mailbox d / Del removes it (twice), Enter logs in
// again after a refused login, else edits its sender name. Addresses, names and service words are plain text.
FocusScope {
    id: page
    property string provider: "gmail"
    property string renaming: ""   // the mailbox whose sender name is being edited
    readonly property var providers: [{id: "gmail", label: "Gmail"}, {id: "yandex", label: "Yandex"},
                                      {id: "imap", label: i18n.t("Other IMAP")}]
    readonly property string helpUrl: ({gmail: "https://myaccount.google.com/apppasswords",
                                        yandex: "https://id.yandex.ru/security/app-passwords"})[provider] || ""
    readonly property var fields: provider === "imap" ? [email, name, password, login, imapHost, imapPort, smtpHost, smtpPort]
                                                      : [email, name, password]
    readonly property Item focused: Window.activeFocusItem
    readonly property int labelWidth: theme.space(170)

    function focusFirst() { (rows.count > 0 ? rows : providerRow).forceActiveFocus() }
    function submit() {
        app.addAccount({provider: provider, email: email.text, name: name.text, password: password.text, login: login.text,
                        imapHost: imapHost.text, imapPort: imapPort.text, smtpHost: smtpHost.text, smtpPort: smtpPort.text})
        password.text = ""   // after every attempt
    }
    function enter(field) {   // Enter: the next field; in the last one, Add
        const i = fields.indexOf(field)
        if (i === fields.length - 1)
            submit()
        else
            fields[i + 1].forceActiveFocus()
    }
    function reveal(item) {   // the item with the keys stays in sight
        let inside = false
        for (let p = item; p && !inside; p = p.parent)
            inside = p === content
        if (!inside)
            return
        const top = item.mapToItem(flick.contentItem, 0, 0).y, bottom = top + item.height
        if (top < flick.contentY)
            flick.contentY = Math.max(0, top - theme.space(8))
        else if (bottom > flick.contentY + flick.height)
            flick.contentY = Math.min(flick.contentHeight - flick.height, bottom - flick.height + theme.space(8))
    }
    onFocusedChanged: reveal(focused === rows ? rows.currentItem : focused)
    Keys.onEscapePressed: app.closeSettings()
    onVisibleChanged: if (!visible) password.text = ""   // a typed password doesn't wait in a hidden page

    Connections {
        target: app
        function onAccountAdded() {
            for (const f of [email, name, login, imapHost, smtpHost])
                f.text = ""
            imapPort.text = "993"
            smtpPort.text = "465"
        }
    }

    component Caption: Text {   // headings and field labels (§9 p. 9)
        color: theme.dim
        font.pixelSize: theme.fontBody
        font.bold: true
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 1.2
    }

    component Frame: Rectangle {   // form fields are framed; the one with the keys like an open picker
        property bool lit: false
        radius: theme.space(8)
        color: lit ? theme.hoverFill : "transparent"
        border.width: 1
        border.color: lit ? theme.accent : theme.muted
    }

    component Field: FocusScope {   // a label on the left, the framed input on the right
        id: field
        property string label
        property alias text: input.text
        property alias echoMode: input.echoMode
        property alias input: input
        width: parent.width
        height: frame.height
        Caption {
            anchors.verticalCenter: parent.verticalCenter
            width: page.labelWidth - theme.space(12)
            elide: Text.ElideRight
            text: field.label
        }
        Frame {
            id: frame
            x: page.labelWidth
            width: parent.width - x
            height: input.implicitHeight + 2 * theme.space(6)
            lit: input.activeFocus
            TextInput {
                id: input
                anchors.fill: parent
                anchors.leftMargin: theme.space(12)
                anchors.rightMargin: theme.space(12)
                verticalAlignment: TextInput.AlignVCenter
                focus: true
                clip: true
                color: theme.foreground
                selectionColor: theme.selection
                selectedTextColor: theme.foreground
                font.pixelSize: theme.fontTitle
                inputMethodHints: echoMode === TextInput.Password ? Qt.ImhSensitiveData | Qt.ImhNoPredictiveText : Qt.ImhNone
                onAccepted: page.enter(field)
            }
        }
    }

    component Choice: Rectangle {   // a provider; the chosen one filled, with a frame while the row has the keys
        id: choice
        property string label
        property bool chosen: false
        property bool hasCursor: false
        signal clicked()
        implicitWidth: choiceText.implicitWidth + 2 * theme.space(12)
        implicitHeight: choiceText.implicitHeight + 2 * theme.space(6)
        radius: theme.space(8)
        color: chosen ? theme.currentFill : mouse.containsMouse ? theme.hoverFill : "transparent"
        border.width: 1
        border.color: hasCursor ? theme.cursorBorder : "transparent"
        Text {
            id: choiceText
            anchors.centerIn: parent
            text: choice.label
            color: theme.foreground
            font.pixelSize: theme.fontTitle
        }
        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: choice.clicked()
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentHeight: content.height + 2 * theme.space(18)
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: content
            x: (page.width - width) / 2
            y: theme.space(18)
            width: Math.min(page.width - 2 * theme.space(18), theme.space(720))
            spacing: theme.space(8)

            Caption {
                visible: rows.count > 0
                text: i18n.t("Mailboxes")
            }
            ListView {
                id: rows
                objectName: "accountRows"
                width: parent.width
                height: contentHeight
                visible: count > 0
                interactive: false
                keyNavigationEnabled: true
                model: app.accounts.length   // a count: new status texts don't rebuild the rows or move the cursor
                spacing: theme.space(2)
                onCurrentIndexChanged: if (activeFocus) page.reveal(currentItem)
                KeyNavigation.down: providerRow
                KeyNavigation.priority: KeyNavigation.AfterItem   // the list moves first, ↓ on its last row leaves it
                Keys.onPressed: e => {
                    const acc = app.accounts[currentIndex]
                    if (!acc || (e.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)))
                        return
                    const latin = e.key >= Qt.Key_A && e.key <= Qt.Key_Z
                    if (e.key === Qt.Key_Delete || (latin ? e.key === Qt.Key_D : e.nativeScanCode === 40)) {
                        if (!e.isAutoRepeat)   // a held d is not the second press
                            app.removeAccount(acc.email)
                        e.accepted = true
                    } else if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) {
                        if (acc.authFailed)
                            app.retryAccount(acc.email)
                        else
                            currentItem.rename()
                        e.accepted = true
                    }
                }

                delegate: Rectangle {
                    id: mailbox
                    required property int index
                    readonly property var acc: app.accounts[index] || ({})
                    readonly property bool editing: page.renaming !== "" && page.renaming === acc.email
                    readonly property bool hasCursor: (rows.activeFocus && ListView.isCurrentItem) || mouse.containsMouse
                    // however the edit ends: else the list, a focus scope, would hand the keys back to the hidden field
                    onEditingChanged: if (!editing) renameField.focus = false
                    function rename() {
                        renameField.text = acc.name
                        page.renaming = acc.email
                        renameField.forceActiveFocus()
                    }
                    function stopRenaming() {
                        page.renaming = ""
                        rows.forceActiveFocus()
                    }
                    width: rows.width
                    height: lines.implicitHeight + 2 * theme.space(8)
                    radius: theme.space(8)
                    color: hasCursor ? theme.hoverFill : "transparent"
                    border.width: 1
                    border.color: hasCursor ? theme.cursorBorder : "transparent"
                    Behavior on color { ColorAnimation { duration: 60 } }

                    MouseArea {   // under the name field: a click there types in it
                        id: mouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            page.renaming = ""   // a click on a row ends an edit
                            rows.currentIndex = mailbox.index
                            rows.forceActiveFocus()
                        }
                    }
                    Column {
                        id: lines
                        x: theme.space(12)
                        y: theme.space(8)
                        width: parent.width - 2 * x
                        spacing: theme.space(3)
                        Item {
                            width: parent.width
                            height: address.implicitHeight
                            Text {
                                id: address
                                anchors.left: parent.left
                                anchors.right: stateLine.left
                                anchors.rightMargin: theme.space(12)
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                text: mailbox.acc.email || ""
                                color: theme.foreground
                                font.pixelSize: theme.fontHeading
                            }
                            Text {
                                id: stateLine
                                anchors.right: parent.right
                                anchors.baseline: address.baseline
                                width: Math.min(implicitWidth, parent.width * 0.45)
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                text: mailbox.acc.statusText || ""
                                color: mailbox.acc.authFailed ? theme.urgent : theme.dim
                                font.pixelSize: theme.fontBody
                            }
                        }
                        Text {
                            width: parent.width
                            visible: !mailbox.editing
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: [mailbox.acc.name, ({gmail: "Gmail", yandex: "Yandex", imap: "IMAP"})[mailbox.acc.provider]]
                                  .filter(s => !!s).join(" · ")
                            color: theme.dim
                            font.pixelSize: theme.fontBody
                        }
                        Item {
                            width: parent.width
                            height: nameFrame.height
                            visible: mailbox.editing
                            Caption {
                                anchors.verticalCenter: parent.verticalCenter
                                width: page.labelWidth - theme.space(24)
                                elide: Text.ElideRight
                                text: i18n.t("Sender name")
                            }
                            Frame {
                                id: nameFrame
                                x: page.labelWidth - theme.space(12)
                                width: parent.width - x
                                height: renameField.implicitHeight + 2 * theme.space(6)
                                lit: renameField.activeFocus
                                TextInput {
                                    id: renameField
                                    objectName: "renameField"
                                    anchors.fill: parent
                                    anchors.leftMargin: theme.space(12)
                                    anchors.rightMargin: theme.space(12)
                                    verticalAlignment: TextInput.AlignVCenter
                                    clip: true
                                    color: theme.foreground
                                    selectionColor: theme.selection
                                    selectedTextColor: theme.foreground
                                    font.pixelSize: theme.fontTitle
                                    function save() {
                                        app.renameAccount(mailbox.acc.email, text)
                                        mailbox.stopRenaming()
                                    }
                                    Keys.onReturnPressed: save()   // accepted here: TextInput would pass Enter on to the list
                                    Keys.onEnterPressed: save()
                                    // the keys went elsewhere in the window: the edit ends; another window: it waits
                                    onActiveFocusChanged: if (!activeFocus && mailbox.editing && Window.active) page.renaming = ""
                                    Keys.onEscapePressed: mailbox.stopRenaming()
                                    Keys.onUpPressed: {}     // eaten: the list's cursor stays on the row being edited
                                    Keys.onDownPressed: {}
                                }
                            }
                        }
                    }
                }
            }

            Caption {
                topPadding: rows.count > 0 ? theme.space(14) : 0
                text: i18n.t("Add a mailbox")
            }
            Row {
                id: providerRow
                objectName: "providerRow"
                spacing: theme.space(8)
                KeyNavigation.up: rows
                KeyNavigation.down: email
                Keys.onLeftPressed: page.provider = page.providers[Math.max(0, index() - 1)].id
                Keys.onRightPressed: page.provider = page.providers[Math.min(page.providers.length - 1, index() + 1)].id
                function index() { return page.providers.findIndex(p => p.id === page.provider) }
                Repeater {
                    model: page.providers
                    Choice {
                        required property var modelData
                        label: modelData.label
                        chosen: page.provider === modelData.id
                        hasCursor: chosen && providerRow.activeFocus
                        onClicked: {
                            page.provider = modelData.id
                            providerRow.forceActiveFocus()
                        }
                    }
                }
            }
            Field {
                id: email
                objectName: "emailField"
                label: i18n.t("Email")
                KeyNavigation.up: providerRow
                KeyNavigation.down: name
            }
            Field {
                id: name
                objectName: "nameField"
                label: i18n.t("Sender name")
                KeyNavigation.up: email
                KeyNavigation.down: password
            }
            Field {
                id: password
                objectName: "passwordField"
                label: i18n.t("App password")
                echoMode: TextInput.Password
                KeyNavigation.up: name
                KeyNavigation.down: help
            }
            Rectangle {   // where the app password comes from; Enter or a click opens the page
                id: help
                objectName: "helpLink"
                visible: page.helpUrl !== ""
                x: page.labelWidth - theme.space(8)
                width: helpLine.implicitWidth + 2 * theme.space(8)
                height: helpLine.implicitHeight + 2 * theme.space(4)
                radius: theme.space(6)
                color: activeFocus || helpMouse.containsMouse ? theme.hoverFill : "transparent"
                border.width: 1
                border.color: activeFocus ? theme.cursorBorder : "transparent"
                KeyNavigation.up: password
                KeyNavigation.down: login
                Keys.onReturnPressed: letter.activateLink(page.helpUrl)
                Keys.onEnterPressed: letter.activateLink(page.helpUrl)
                Row {
                    id: helpLine
                    anchors.centerIn: parent
                    readonly property var parts: i18n.t("Create an app password: %1").split("%1")
                    Text {
                        text: helpLine.parts[0]
                        color: theme.dim
                        font.pixelSize: theme.fontBody
                    }
                    Text {
                        text: page.helpUrl
                        color: theme.accent
                        font.pixelSize: theme.fontBody
                        font.underline: true
                    }
                    Text {
                        text: helpLine.parts[1] || ""
                        color: theme.dim
                        font.pixelSize: theme.fontBody
                    }
                }
                MouseArea {
                    id: helpMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: letter.activateLink(page.helpUrl)
                }
            }
            Field {
                id: login
                objectName: "loginField"
                visible: page.provider === "imap"
                label: i18n.t("Login")
                KeyNavigation.up: password
                KeyNavigation.down: imapHost
            }
            Field {
                id: imapHost
                objectName: "imapHostField"
                visible: page.provider === "imap"
                label: i18n.t("IMAP server")
                KeyNavigation.up: login
                KeyNavigation.down: imapPort
            }
            Field {
                id: imapPort
                visible: page.provider === "imap"
                label: i18n.t("IMAP port")
                text: "993"
                input.maximumLength: 5
                KeyNavigation.up: imapHost
                KeyNavigation.down: smtpHost
            }
            Field {
                id: smtpHost
                visible: page.provider === "imap"
                label: i18n.t("SMTP server")
                KeyNavigation.up: imapPort
                KeyNavigation.down: smtpPort
            }
            Field {
                id: smtpPort
                visible: page.provider === "imap"
                label: i18n.t("SMTP port")
                text: "465"
                input.maximumLength: 5
                KeyNavigation.up: smtpHost
                KeyNavigation.down: addButton
            }
            Item {
                width: parent.width
                height: addButton.height + theme.space(6)
                Rectangle {
                    id: addButton
                    objectName: "addButton"
                    readonly property bool hasCursor: activeFocus || addMouse.containsMouse
                    x: page.labelWidth
                    y: theme.space(6)
                    implicitWidth: addText.implicitWidth + 2 * theme.space(14)
                    implicitHeight: addText.implicitHeight + 2 * theme.space(6)
                    radius: theme.space(8)
                    color: hasCursor ? theme.hoverFill : "transparent"
                    border.width: 1
                    border.color: hasCursor ? theme.cursorBorder : theme.muted
                    KeyNavigation.up: page.provider === "imap" ? smtpPort : help
                    Keys.onReturnPressed: page.submit()
                    Keys.onEnterPressed: page.submit()
                    Text {
                        id: addText
                        anchors.centerIn: parent
                        text: app.adding ? i18n.t("Checking the login…") : i18n.t("Add")
                        color: theme.foreground
                        font.pixelSize: theme.fontTitle
                    }
                    MouseArea {
                        id: addMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: page.submit()
                    }
                }
            }
            Text {
                id: addError
                objectName: "addError"
                x: page.labelWidth
                width: parent.width - x
                visible: text !== ""
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: app.addError
                color: theme.urgent
                font.pixelSize: theme.fontBody
                onTextChanged: if (addError.text !== "") page.reveal(addError)
            }
        }
    }
}
