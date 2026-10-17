import QtQuick

// An attachment plaque (letter and form): icon in the accent, name, size in dim. `lit` is the cursor / focus:
// hoverFill and cursorBorder go over the plaque's own fill.
// Sizes are the mockup's px (base 16) scaled to space(): space(round(px * 12 / 16)).
Rectangle {
    id: plaque
    property string icon
    property string name
    property string size
    property string tail          // after the size: the form's ✕
    property bool lit: false
    property int maxWidth: 0      // the plaque never grows past it; the name elides
    readonly property real tailWidth: tailText.visible ? tailText.width : 0
    readonly property int padX: theme.space(12)

    implicitWidth: Math.min(content.implicitWidth + 2 * padX, maxWidth > 0 ? maxWidth : 1e6)
    implicitHeight: content.implicitHeight + 2 * theme.space(8)
    width: implicitWidth
    height: implicitHeight
    radius: theme.space(6)
    color: Qt.tint(Qt.alpha(theme.foreground, 0.06), lit ? theme.hoverFill : "transparent")
    border.width: 1
    border.color: lit ? theme.cursorBorder : Qt.alpha(theme.foreground, 0.14)

    Row {
        id: content
        x: plaque.padX
        anchors.verticalCenter: parent.verticalCenter
        spacing: theme.space(8)
        Text {
            id: iconText
            anchors.verticalCenter: parent.verticalCenter
            text: plaque.icon
            color: theme.accent
            font.pixelSize: theme.iconLarge
        }
        Text {
            id: nameText
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, Math.max(0, (plaque.maxWidth > 0 ? plaque.maxWidth : 1e6) - 2 * plaque.padX - iconText.width - sizeText.width - tailText.width - 3 * content.spacing))
            elide: Text.ElideMiddle
            textFormat: Text.PlainText
            text: plaque.name
            color: theme.foreground
            font.pixelSize: theme.fontSubtitle
        }
        Text {
            id: sizeText
            anchors.verticalCenter: parent.verticalCenter
            textFormat: Text.PlainText
            text: plaque.size
            color: theme.dim
            font.pixelSize: theme.fontSubtitle
        }
        Text {
            id: tailText
            anchors.verticalCenter: parent.verticalCenter
            visible: plaque.tail !== ""
            text: plaque.tail
            color: theme.dim
            font.pixelSize: theme.fontSubtitle
        }
    }
}
