pragma ComponentBehavior: Bound

import QtQuick
import org.kde.kirigamiaddons.formcard as FormCard

FormCard.FormCardPage {
    id: root
    title: i18nc("@title:window", "Settings")

    FormCard.FormHeader {
        title: i18nc("@title:group", "General")
    }

    FormCard.FormCard {
        FormCard.FormSwitchDelegate {
            id: autostartEnabled
            text: i18nc("@label:switch", "Automatically start application on login")
            checkable: true
            checked: frontendManager.autostartEnabled
            onToggled: frontendManager.autostartEnabled = checked
        }

        FormCard.FormSwitchDelegate {
            id: virtualSurroundAutoEnabled
            text: i18nc("@label:switch", "Automatically enable virtual surround sound")
            checkable: true
            checked: frontendManager.virtualSurroundAutoEnabled
            onToggled: frontendManager.virtualSurroundAutoEnabled = checked
        }

        FormCard.FormComboBoxDelegate {
            id: startupUi
            text: i18nc("@label:combobox", "Application behavior")
            description: i18nc("@info:description", "How the application should start and run.<br>Restart the program to apply.")
            displayMode: FormCard.FormComboBoxDelegate.ComboBox
            editable: false
            textRole: "label"
            valueRole: "key"
            model: [
                {
                    label: i18nc("@option", "Show system tray icon"),
                    key: "showTray"
                },
                {
                    label: i18nc("@option", "Show system tray icon & hide window on startup"),
                    key: "showTrayHideWindow"
                },
                {
                    label: i18nc("@option", "Hide system tray icon"),
                    key: "hideTray"
                }
            ]
            Component.onCompleted: currentIndex = indexOfValue(frontendManager.startupUi)
            onCurrentValueChanged: frontendManager.startupUi = currentValue
        }
    }

    FormCard.FormHeader {
        title: i18nc("@title:group", "Audio")
    }

    FormCard.FormCard {
        FormCard.FormSwitchDelegate {
            id: upmixEnabled
            text: i18nc("@label:switch", "Upmix stereo to surround sound")
            description: i18nc("@info:description", "Upmix stereo audio to the virtual 7.1 channels using Passive Surround Decoding (bass redirection at 70–90 Hz). Native multi-channel audio is passed through unchanged. Applied instantly.")
            checkable: true
            checked: frontendManager.upmixEnabled
            onToggled: frontendManager.upmixEnabled = checked

            Connections {
                target: frontendManager
                function onUpmixEnabledChanged() {
                    // Clicking replaces this binding with a plain value; re-create it so
                    // a failed graph rebuild can show the reverted state
                    upmixEnabled.checked = Qt.binding(function () {
                        return frontendManager.upmixEnabled;
                    })
                }
            }
        }
    }
}
