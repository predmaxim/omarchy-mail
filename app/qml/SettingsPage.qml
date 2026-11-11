import QtQuick

// The mailboxes page (spec «Ящики и вход», round 2 §4; rules §9: no tabs, Esc — back): the «List» switch, the
// mailboxes with their state, then «Add a mailbox»; the form below shows the row under the cursor. ↑/↓ walk the
// switch, the rows and the form, Tab only the form's fields; Enter on a row goes into the form; on a mailbox d / Del
// removes it (twice). Esc: from the form back to the row, from the list back to the mail. Addresses, names and service
// words are plain text.
FocusScope {
    id: page
    property string provider: "gmail"   // the one chosen on «Add a mailbox»
    readonly property var acc: app.accounts[rows.currentIndex]   // the mailbox under the cursor; undefined on «Add»
    readonly property bool onMailbox: acc !== undefined
    readonly property string formProvider: onMailbox ? acc.provider : provider
    // another row: its own form, typed edits go; a status event rebuilds `acc` but keeps the address, and the typing
    readonly property string formEmail: acc ? acc.email : ""
    onFormEmailChanged: fill()
    readonly property var providers: [{id: "gmail", label: "Gmail"}, {id: "yandex", label: "Yandex"},
                                      {id: "imap", label: i18n.t("Other IMAP")}]
    readonly property string helpUrl: ({gmail: "https://myaccount.google.com/apppasswords",
                                        yandex: "https://id.yandex.ru/security/app-passwords"})[formProvider] || ""
    readonly property var fields: (onMailbox ? [name, password] : [email, name, password])
                                  .concat(formProvider === "imap" ? [login, imapHost, imapPort, smtpHost, smtpPort] : [])
    readonly property Item focused: Window.activeFocusItem
    readonly property int labelWidth: theme.space(170)

    function focusFirst() { (app.accounts.length > 0 ? rows : providerRow).forceActiveFocus() }
    function fill() {   // the form from the row under the cursor: a mailbox's settings, or empty to add one
        for (const f of [email, name, password, login, imapHost, imapPort, smtpHost, smtpPort])
            f.reset()
    }
    function submit() {
        app.addAccount({provider: provider, email: email.text, name: name.text, password: password.text, login: login.text,
                        imapHost: imapHost.text, imapPort: imapPort.text, smtpHost: smtpHost.text, smtpPort: smtpPort.text})
        password.text = ""   // after every attempt
    }
    function save() {   // an empty password: unchanged
        app.updateAccount({email: acc.email, name: name.text, password: password.text, login: login.text,
                           imapHost: imapHost.text, imapPort: imapPort.text, smtpHost: smtpHost.text, smtpPort: smtpPort.text})
        password.text = ""   // after every attempt
    }
    function enter(field) {   // Enter: the next field; in the last one, Save or Add
        const i = fields.indexOf(field)
        if (i === fields.length - 1)
            onMailbox ? save() : submit()
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
    // the list's keys are on its current row, so `activeFocus`, not `focused === rows`
    Keys.onEscapePressed: rows.activeFocus || previewRow.activeFocus ? app.closeSettings() : rows.forceActiveFocus()
    // hidden: typed edits and a password don't wait; shown: a save that ended behind the page is in the form, not the
    // name from before it (the cursor row, so `formEmail`, may be the same)
    onVisibleChanged: fill()

    Connections {
        target: app
        // the list grows by one: an int model puts its cursor on the first row, and the add form goes
        function onAccountAdded() {
            page.fill()
            rows.forceActiveFocus()
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
        // the row's value, which fill() puts in. A save that lands while the form shows the value from before it (the
        // cursor left and came back) changes it: a field not typed over since takes it, a typed one keeps the typing.
        property string base
        property string held   // the base the text was last given
        property alias text: input.text
        property alias echoMode: input.echoMode
        property alias input: input
        width: parent.width
        height: frame.height
        function reset() {
            text = base
            held = base
        }
        onBaseChanged: {
            if (text === held)
                text = base
            held = base
        }
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
                activeFocusOnTab: true   // Tab and Shift+Tab walk the visible fields, as ↑/↓
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

    component Info: Item {   // a read-only line of the form: a label and plain text, no keys
        id: info
        property string label
        property string value
        width: parent.width
        height: infoText.implicitHeight + 2 * theme.space(6)
        Caption {
            anchors.verticalCenter: parent.verticalCenter
            width: page.labelWidth - theme.space(12)
            elide: Text.ElideRight
            text: info.label
        }
        Text {
            id: infoText
            x: page.labelWidth + theme.space(12)
            width: parent.width - x
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
            textFormat: Text.PlainText
            text: info.value
            color: theme.foreground
            font.pixelSize: theme.fontTitle
        }
    }

    component Button: Rectangle {   // a framed command: Enter or a click
        id: button
        property string text
        signal clicked()
        readonly property bool hasCursor: activeFocus || buttonMouse.containsMouse
        implicitWidth: buttonText.implicitWidth + 2 * theme.space(14)
        implicitHeight: buttonText.implicitHeight + 2 * theme.space(6)
        radius: theme.space(8)
        color: hasCursor ? theme.hoverFill : "transparent"
        border.width: 1
        border.color: hasCursor ? theme.cursorBorder : theme.muted
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        Text {
            id: buttonText
            anchors.centerIn: parent
            text: button.text
            color: theme.foreground
            font.pixelSize: theme.fontTitle
        }
        MouseArea {
            id: buttonMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: button.clicked()
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

    component ToggleSwitch: Rectangle {   // the stock shell ToggleSwitch's look: that one needs Quickshell
        id: toggle
        property bool checked: false
        width: theme.space(42)
        height: theme.space(22)
        radius: height / 2
        color: checked ? theme.currentFill : "transparent"
        border.width: 1
        border.color: checked ? theme.accent : theme.muted
        Behavior on color { ColorAnimation { duration: 120 } }
        Rectangle {
            width: theme.space(16)
            height: width
            radius: height / 2
            x: toggle.checked ? toggle.width - width - theme.space(3) : theme.space(3)
            anchors.verticalCenter: parent.verticalCenter
            color: toggle.checked ? theme.accent : Qt.darker(theme.foreground, 1.25)
            Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
            Behavior on color { ColorAnimation { duration: 120 } }
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
                text: i18n.t("List")
            }
            Rectangle {   // the letter text in the list: Enter, Space, ←/→ or a click flip it
                id: previewRow
                objectName: "previewRow"
                readonly property bool hasCursor: activeFocus || previewMouse.containsMouse
                width: parent.width
                height: Math.max(previewLabel.implicitHeight, previewSwitch.height) + 2 * theme.space(8)
                radius: theme.space(8)
                color: hasCursor ? theme.hoverFill : "transparent"
                border.width: 1
                border.color: hasCursor ? theme.cursorBorder : "transparent"
                Behavior on color { ColorAnimation { duration: 60 } }
                KeyNavigation.down: rows
                onActiveFocusChanged: if (activeFocus) rows.currentIndex = 0   // the form shows the first row
                Keys.onPressed: e => {
                    if (e.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
                        return
                    if ([Qt.Key_Return, Qt.Key_Enter, Qt.Key_Space, Qt.Key_Left, Qt.Key_Right].includes(e.key)) {
                        if (!e.isAutoRepeat)   // a held key doesn't flicker the switch
                            app.listPreview = !app.listPreview
                        e.accepted = true
                    }
                }
                MouseArea {
                    id: previewMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        app.listPreview = !app.listPreview
                        previewRow.forceActiveFocus()
                    }
                }
                Text {
                    id: previewLabel
                    anchors.left: parent.left
                    anchors.right: previewSwitch.left
                    anchors.leftMargin: theme.space(12)
                    anchors.rightMargin: theme.space(12)
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: i18n.t("Letter text in the list")
                    color: theme.foreground
                    font.pixelSize: theme.fontHeading
                }
                ToggleSwitch {
                    id: previewSwitch
                    objectName: "previewSwitch"
                    anchors.right: parent.right
                    anchors.rightMargin: theme.space(12)
                    anchors.verticalCenter: parent.verticalCenter
                    checked: app.listPreview
                }
            }

            Caption {
                topPadding: theme.space(14)
                text: i18n.t("Mailboxes")
            }
            ListView {
                id: rows
                objectName: "accountRows"
                width: parent.width
                height: contentHeight
                interactive: false
                keyNavigationEnabled: true
                // a count: new status texts don't rebuild the rows or move the cursor; the last row is «Add a mailbox»
                model: app.accounts.length + 1
                spacing: theme.space(2)
                onCurrentIndexChanged: if (activeFocus) page.reveal(currentItem)
                KeyNavigation.up: previewRow
                KeyNavigation.down: providerRow   // only from the last row, «Add a mailbox»
                KeyNavigation.priority: KeyNavigation.AfterItem   // the list moves first, ↑/↓ at its ends leave it
                Keys.onPressed: e => {
                    if (e.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
                        return
                    const latin = e.key >= Qt.Key_A && e.key <= Qt.Key_Z
                    if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) {
                        (page.onMailbox ? name : providerRow).forceActiveFocus()
                        e.accepted = true
                    } else if (page.onMailbox && (e.key === Qt.Key_Delete || (latin ? e.key === Qt.Key_D : e.nativeScanCode === 40))) {
                        if (!e.isAutoRepeat)   // a held d is not the second press
                            app.removeAccount(page.acc.email)
                        e.accepted = true
                    }
                }

                delegate: Rectangle {
                    id: mailbox
                    required property int index
                    readonly property bool adder: index === app.accounts.length   // the last row: «Add a mailbox»
                    readonly property var acc: app.accounts[index] || ({})
                    readonly property bool hasCursor: (rows.activeFocus && ListView.isCurrentItem) || mouse.containsMouse
                    width: rows.width
                    height: lines.implicitHeight + 2 * theme.space(8)
                    radius: theme.space(8)
                    color: hasCursor ? theme.hoverFill : "transparent"
                    border.width: 1
                    border.color: hasCursor ? theme.cursorBorder : "transparent"
                    Behavior on color { ColorAnimation { duration: 60 } }

                    MouseArea {
                        id: mouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
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
                                text: mailbox.adder ? i18n.t("Add a mailbox") : mailbox.acc.email || ""
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
                            visible: !mailbox.adder
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: [mailbox.acc.name, ({gmail: "Gmail", yandex: "Yandex", imap: "IMAP"})[mailbox.acc.provider]]
                                  .filter(s => !!s).join(" · ")
                            color: theme.dim
                            font.pixelSize: theme.fontBody
                        }
                    }
                }
            }

            Item {   // the list, then its form: a gap as above a caption
                width: 1
                height: theme.space(6)
            }
            Info {
                objectName: "infoEmail"
                visible: page.onMailbox
                label: i18n.t("Email")
                value: page.formEmail
            }
            Info {
                objectName: "infoType"
                visible: page.onMailbox
                label: i18n.t("Type")
                value: page.providers.find(p => p.id === page.formProvider)?.label ?? ""
            }
            Text {   // why the last login failed; «Retry login» stands next to Save
                objectName: "loginState"
                x: page.labelWidth + theme.space(12)
                width: parent.width - x
                visible: page.onMailbox && page.acc.authFailed
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: page.acc?.statusText ?? ""
                color: theme.urgent
                font.pixelSize: theme.fontBody
            }
            Row {
                id: providerRow
                objectName: "providerRow"
                visible: !page.onMailbox
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
                visible: !page.onMailbox
                label: i18n.t("Email")
                KeyNavigation.up: providerRow
                KeyNavigation.down: name
            }
            Field {
                id: name
                objectName: "nameField"
                label: i18n.t("Sender name")
                base: page.acc?.name ?? ""
                KeyNavigation.up: email
                KeyNavigation.down: password
            }
            Field {
                id: password
                objectName: "passwordField"
                label: page.onMailbox ? i18n.t("New app password") : i18n.t("App password")
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
                visible: page.formProvider === "imap"
                label: i18n.t("Login")
                base: page.acc?.login ?? ""
                KeyNavigation.up: password
                KeyNavigation.down: imapHost
            }
            Field {
                id: imapHost
                objectName: "imapHostField"
                visible: page.formProvider === "imap"
                label: i18n.t("IMAP server")
                base: page.acc?.imapHost ?? ""
                KeyNavigation.up: login
                KeyNavigation.down: imapPort
            }
            Field {
                id: imapPort
                visible: page.formProvider === "imap"
                label: i18n.t("IMAP port")
                base: String(page.acc?.imapPort || 993)
                text: "993"
                input.maximumLength: 5
                KeyNavigation.up: imapHost
                KeyNavigation.down: smtpHost
            }
            Field {
                id: smtpHost
                visible: page.formProvider === "imap"
                label: i18n.t("SMTP server")
                base: page.acc?.smtpHost ?? ""
                KeyNavigation.up: imapPort
                KeyNavigation.down: smtpPort
            }
            Field {
                id: smtpPort
                visible: page.formProvider === "imap"
                label: i18n.t("SMTP port")
                base: String(page.acc?.smtpPort || 465)
                text: "465"
                input.maximumLength: 5
                KeyNavigation.up: smtpHost
                KeyNavigation.down: page.onMailbox ? saveButton : addButton
            }
            Item {
                width: parent.width
                height: buttons.height + theme.space(6)
                Row {   // ←/→ between Save and Retry login
                    id: buttons
                    x: page.labelWidth
                    y: theme.space(6)
                    spacing: theme.space(8)
                    Button {
                        id: addButton
                        objectName: "addButton"
                        visible: !page.onMailbox
                        text: app.adding ? i18n.t("Checking the login…") : i18n.t("Add")
                        KeyNavigation.up: page.formProvider === "imap" ? smtpPort : help
                        onClicked: page.submit()
                    }
                    Button {
                        id: saveButton
                        objectName: "saveButton"
                        visible: page.onMailbox
                        text: page.acc?.saving ? i18n.t("Checking the login…") : i18n.t("Save")
                        KeyNavigation.up: page.formProvider === "imap" ? smtpPort : help
                        KeyNavigation.right: retryButton
                        onClicked: page.save()
                    }
                    Button {
                        id: retryButton
                        objectName: "retryButton"
                        visible: page.onMailbox && page.acc.authFailed
                        text: i18n.t("Retry login")
                        KeyNavigation.up: page.formProvider === "imap" ? smtpPort : help
                        KeyNavigation.left: saveButton
                        onClicked: app.retryAccount(page.acc.email)
                        // the login works again: Qt would leave the keys on the hidden button
                        onVisibleChanged: if (!visible && activeFocus) saveButton.forceActiveFocus()
                    }
                }
            }
            Text {
                id: formError
                objectName: "formError"
                x: page.labelWidth
                width: parent.width - x
                visible: text !== ""
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: page.onMailbox ? page.acc.saveError || "" : app.addError
                color: theme.urgent
                font.pixelSize: theme.fontBody
                onTextChanged: if (formError.text !== "") page.reveal(formError)
            }
        }
    }
}
