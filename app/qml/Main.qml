import QtQuick

// The mail window (spec «Окно», layout-v2): the line of commands on top; the search, the mailbox and folder
// pickers and the list on the left; the open letter on the right. Keys go to `app`: from the root item, from the
// search field as "search"; an open picker keeps its own.
Window {
    id: win
    width: 1200
    height: 800
    visible: true
    title: i18n.t("Mail")
    color: theme.background

    readonly property string page: app.page   // the mailboxes page takes the keys, the main one gives them to root
    onPageChanged: page === "settings" ? settings.focusFirst() : root.forceActiveFocus()

    Connections {
        target: app
        function onRaiseRequested() {
            win.raise()
            win.requestActivate()
        }
        function onCloseRequested() { win.close() }
        function onFocusSearch() { search.forceActiveFocus() }
        function onLeaveSearch() { root.forceActiveFocus() }
        function onOpenPicker(which) { (which === "account" ? accountPicker : folderPicker).open() }
        function onScroll(cmd, dir) {   // int(om::Cmd): ScrollLines 11, ScrollPages 12, ScrollTop 13, ScrollEnd 14
            if (cmd === 11)
                pane.view.scrollLines(dir)
            else if (cmd === 12)
                pane.view.scrollPages(dir)
            else if (cmd === 13)
                pane.view.toTop()
            else if (cmd === 14)
                pane.view.toEnd()
        }
    }
    Connections {   // a reset puts the view at the top: a reload puts it back; a cursor move keeps the cursor row in sight
        target: list
        function onModelAboutToBeReset() { mailList.keptY = mailList.contentY }
        function onRowsReplaced(sameLetter) {
            if (sameLetter)   // the same letter under the cursor: the rows the user scrolled to stay
                mailList.contentY = Math.min(mailList.keptY, Math.max(0, mailList.contentHeight - mailList.height))
        }
        function onCursorChanged() {
            if (list.cursor >= 0)
                mailList.positionViewAtIndex(list.cursor, ListView.Contain)
        }
    }
    Connections {   // a click in the letter takes the keys back from the search field, as a click on a row does
        target: pane.view
        function onPressed() { root.forceActiveFocus() }
    }
    Binding {
        target: app
        property: "letterAtTop"
        value: pane.view.atTop
    }

    Item {
        id: root
        anchors.fill: parent
        focus: true
        // only its own keys: those a field or the form let through are theirs to drop
        Keys.onPressed: e => e.accepted = root.activeFocus && app.key(e.key, e.nativeScanCode, e.modifiers, e.isAutoRepeat, "")

        ActionBar {
            id: bar
            width: parent.width
        }

        Item {
            id: column
            visible: win.page === "main"
            readonly property int pad: theme.space(8)
            readonly property int inset: pad + theme.space(12)   // the text edge of the rows, pickers and search
            anchors.top: bar.bottom
            anchors.bottom: parent.bottom
            opacity: compose.open ? 0.5 : 1   // the form takes the keys: the list is only a backdrop
            enabled: !compose.open
            width: Math.max(theme.space(300), Math.min(root.width * 0.38, theme.space(400)))

            Rectangle {
                objectName: "leftSurface"
                anchors.fill: parent
                color: theme.surface
            }

            Column {
                id: head
                width: parent.width

                Rectangle {   // the search line: no frame, the window cursor fills it while the field is left
                    width: parent.width
                    height: Math.max(theme.space(34), search.implicitHeight) + 2 * theme.space(8)
                    color: nav.row === 1 && !search.activeFocus ? theme.hoverFill : "transparent"
                    TextInput {
                        id: search
                        objectName: "searchField"
                        anchors.left: parent.left
                        anchors.right: searchKey.left
                        anchors.leftMargin: column.inset
                        anchors.rightMargin: theme.space(8)
                        anchors.verticalCenter: parent.verticalCenter
                        clip: true
                        text: app.query
                        color: theme.foreground
                        selectionColor: theme.selection
                        selectedTextColor: theme.foreground
                        font.pixelSize: theme.fontHeading
                        onTextEdited: app.setQuery(text)
                        Keys.onPressed: e => e.accepted = app.key(e.key, e.nativeScanCode, e.modifiers, e.isAutoRepeat, "search")
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: search.text === ""
                            text: "󰍉 " + i18n.t("Search all mailboxes")
                            color: theme.dim
                            font.pixelSize: theme.fontHeading
                        }
                    }
                    ActionBar.KeyCap {
                        id: searchKey
                        anchors.right: parent.right
                        anchors.rightMargin: column.inset
                        anchors.verticalCenter: parent.verticalCenter
                        text: search.activeFocus || app.query !== "" ? "Esc" : "/"
                    }
                }
                Rectangle {
                    width: parent.width
                    height: 1
                    color: Qt.tint(theme.muted, theme.hoverFill)   // a step lighter than muted: reads on the surface
                }
                Row {
                    id: pickers
                    x: column.pad
                    width: parent.width - 2 * column.pad
                    topPadding: theme.space(8)
                    bottomPadding: theme.space(4)
                    spacing: theme.space(8)
                    Picker {
                        id: accountPicker
                        objectName: "accountPicker"
                        visible: app.accountChoices.length > 0
                        width: (pickers.width - pickers.spacing) / 2
                        choices: app.accountChoices
                        current: app.accountIndex + 1   // choice 0: all mailboxes
                        hasCursor: nav.row === 2 && nav.col === 0
                        onPicked: i => app.pickAccount(i - 1)
                    }
                    Picker {
                        id: folderPicker
                        objectName: "folderPicker"
                        width: accountPicker.visible ? accountPicker.width : pickers.width
                        choices: app.folderChoices
                        current: app.folderIndex
                        hasCursor: nav.row === 2 && nav.col === (accountPicker.visible ? 1 : 0)
                        onPicked: i => app.pickFolder(i)
                    }
                }
                Text {
                    x: column.inset
                    width: parent.width - 2 * column.inset
                    visible: text !== ""
                    topPadding: theme.space(4)
                    bottomPadding: theme.space(4)
                    wrapMode: Text.Wrap
                    text: list.selectedCount > 0 ? i18n.tn("Selected: %n", list.selectedCount)
                        : app.foundText + (app.foundText !== "" && app.remotePending ? " · " + i18n.t("searching servers…") : "")
                    color: theme.dim
                    font.pixelSize: theme.fontBody
                }
            }

            ListView {
                id: mailList
                objectName: "mailList"
                anchors.top: head.bottom
                anchors.topMargin: theme.space(4)
                anchors.bottom: parent.bottom
                x: column.pad
                width: parent.width - 2 * column.pad
                clip: true
                spacing: theme.space(4)   // the rows are separate surfaces
                boundsBehavior: Flickable.StopAtBounds
                keyNavigationEnabled: false   // the cursor is MailList's, moved through app.key / app.clickRow only
                property real keptY: 0        // contentY before a reset
                model: list
                delegate: MailRow {
                    onClicked: modifiers => {
                        root.forceActiveFocus()   // a click on a letter takes the keys back from the search field
                        app.clickRow(index, modifiers)
                    }
                }
                section.property: "group"
                section.delegate: Item {
                    required property string section
                    width: mailList.width
                    height: section === "remote" ? caption.implicitHeight + 2 * theme.space(8) : 0
                    Text {
                        id: caption
                        x: theme.space(12)
                        anchors.verticalCenter: parent.verticalCenter
                        visible: parent.section === "remote"
                        text: i18n.t("On servers, before 2026")
                        color: theme.dim
                        font.pixelSize: theme.fontBody
                    }
                }
            }
            Text {
                anchors.centerIn: mailList
                visible: mailList.count === 0
                text: i18n.t("No mail here")
                color: theme.dim
                font.pixelSize: theme.fontTitle
            }
        }

        Rectangle {
            id: divider
            visible: win.page === "main"
            anchors.left: column.right
            anchors.top: bar.bottom
            anchors.bottom: parent.bottom
            width: 1
            color: theme.muted
        }

        ComposePane {
            id: form
            anchors.left: divider.right
            anchors.right: parent.right
            anchors.top: bar.bottom
            anchors.bottom: parent.bottom
            onClosed: root.forceActiveFocus()
        }
        LetterPane {
            id: pane
            visible: win.page === "main" && !compose.open
            anchors.left: divider.right
            anchors.right: parent.right
            anchors.top: bar.bottom
            anchors.bottom: parent.bottom
        }

        SettingsPage {
            id: settings
            objectName: "settingsPage"
            visible: win.page === "settings"
            anchors.top: bar.bottom
            anchors.bottom: parent.bottom
            width: parent.width
        }
    }
}
