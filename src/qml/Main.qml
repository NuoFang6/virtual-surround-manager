pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

import de.berny23.virtual_surround_manager

// Provides basic features needed for all kirigami applications
Kirigami.ApplicationWindow {
    id: root
    width: 550
    height: 570
    title: i18nc("@title:window", "Manager")

    // Set right before quitting so the confirmation is not shown twice
    property bool forceQuit: false

    // Intercept close event to minimize to tray instead of quitting
    onClosing: event => {
        // Only intercept if tray icon is visible, to prevent being unable to close app after changing tray icon setting from hidden to visible
        if (trayIcon.is_visible() && !forceQuit) {
            event.accepted = false;
            if (trayIcon !== null && trayIcon.hide_main_window !== undefined) {
                trayIcon.hide_main_window();
            }
        } else if (!forceQuit && frontendManager.virtualSurroundEnabled && frontendManager.is_audio_playing()) {
            // Closing really quits here (no tray icon); ask before cutting the audio
            event.accepted = false;
            quitConfirmDialog.open();
        }
    }

    Connections {
        target: frontendManager
        function onVirtualSurroundEnabledChanged() {
            // Keep the switch in sync with toggles from the tray icon
            toggleVirtualSurroundAction.checked = frontendManager.virtualSurroundEnabled;
        }
    }

    Kirigami.PromptDialog {
        id: disableConfirmDialog
        title: i18nc("@title:dialog", "Audio is playing")
        subtitle: i18nc("@info:dialog", "Disabling virtual surround will move the playing audio to your default output device. Do you want to continue?")
        standardButtons: Controls.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Keep enabled")
                onTriggered: disableConfirmDialog.close()
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Disable")
                icon.name: "dialog-warning-symbolic"
                onTriggered: {
                    frontendManager.virtualSurroundEnabled = false;
                    disableConfirmDialog.close();
                }
            }
        ]
    }

    Kirigami.PromptDialog {
        id: quitConfirmDialog
        title: i18nc("@title:dialog", "Audio is playing")
        subtitle: i18nc("@info:dialog", "Closing the application will move the playing audio back to your default output device. Do you want to quit?")
        standardButtons: Controls.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Cancel")
                onTriggered: quitConfirmDialog.close()
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Quit")
                icon.name: "application-exit-symbolic"
                onTriggered: {
                    root.forceQuit = true;
                    quitConfirmDialog.close();
                    Qt.quit();
                }
            }
        ]
    }

    // Load the additional pages
    Component {
        id: settingsPage
        Settings {}
    }
    Component {
        id: aboutPage
        Kirigami.AboutPage {
            aboutData: About
        }
    }

    globalDrawer: Kirigami.GlobalDrawer {
        isMenu: true
        actions: [
            Kirigami.Action {
                text: i18nc("@title:window", "Settings")
                icon.name: "preferences-other"
                onTriggered: pageStack.layers.push(settingsPage)
            },
            Kirigami.Action {
                text: i18nc("@action", "About Virtual Surround")
                icon.name: "help-about"
                onTriggered: pageStack.layers.push(aboutPage)
            }
        ]
    }

    pageStack.initialPage: Kirigami.Page {
        id: mainPage
        title: i18nc("@page:title", "Quick Settings")
        actions: [
            // Global toggle for enabling or disabling the virtual surround routing
            Kirigami.Action {
                id: toggleVirtualSurroundAction
                text: i18nc("@action:switch", "Enabled")
                checkable: true
                checked: frontendManager.virtualSurroundEnabled
                enabled: frontendManager.hrirWavFileNames.length > 0
                onToggled: {
                    // Ignore the echo when the switch was synced from the manager state
                    if (checked === frontendManager.virtualSurroundEnabled) {
                        return;
                    }
                    if (!checked && frontendManager.is_audio_playing()) {
                        // Do not cut the audio without asking; revert and confirm
                        checked = frontendManager.virtualSurroundEnabled;
                        disableConfirmDialog.open();
                        return;
                    }
                    frontendManager.virtualSurroundEnabled = checked;
                }
                displayComponent: Controls.Switch {
                    action: toggleVirtualSurroundAction
                }
            }
        ]

        ColumnLayout {
            anchors.fill: parent

            // This shows error messages, normally hidden
            Kirigami.InlineMessage {
                id: errorMessage
                Layout.fillWidth: true
                type: Kirigami.MessageType.Error
                text: i18nc("@label", "An error occurred: %1", frontendManager.errorMessage)
                visible: frontendManager.errorMessage.length > 0
                showCloseButton: true
            }

            Kirigami.Card {
                actions: [
                    Kirigami.Action {
                        text: i18nc("@action", "Open folder")
                        icon.name: "folder-open"
                        onTriggered: frontendManager.open_hrir_wav_folder()
                    },
                    Kirigami.Action {
                        text: i18nc("@action", "Reload")
                        icon.name: "view-refresh"
                        onTriggered: frontendManager.load_hrir_wav_files()
                    }
                ]
                Layout.fillHeight: true
                Layout.fillWidth: true
                header: Kirigami.Heading {
                    text: i18nc("@header", "Virtualization Profiles")
                    level: 2
                }
                contentItem: ListView {
                    model: frontendManager.hrirWavFileNames
                    delegate: Controls.RadioDelegate {
                        id: profileDelegate
                        required property string modelData
                        required property int index

                        width: parent ? parent.width : 0
                        text: modelData
                        checked: index == frontendManager.hrirWavFileNameIndex
                        onClicked: frontendManager.hrirWavFileNameIndex = index

                        Connections {
                            target: frontendManager
                            function onHrirWavFileNameIndexChanged() {
                                // Clicking replaces this binding with a plain value, so re-create
                                // it on every change to keep tray-driven switches highlighted
                                profileDelegate.checked = Qt.binding(function () {
                                    return profileDelegate.index === frontendManager.hrirWavFileNameIndex;
                                })
                            }
                        }
                    }
                }
            }

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                text: i18nc("@message", "<b>Adding more profiles</b><br>Just open the folder and place your WAV files there. You can find hundreds of these files in the <a href=\"https://airtable.com/appayGNkn3nSuXkaz/shruimhjdSakUPg2m/tbloLjoZKWJDnLtTc\">HRTF Database</a>.")
                onLinkActivated: function (link) {
                    Qt.openUrlExternally(link);
                }
                visible: true
                showCloseButton: true
            }
            Kirigami.InlineMessage {
                Layout.fillWidth: true
                text: i18nc("@message", "<b>Using with EasyEffects</b><br>Exclude applications in EasyEffects that use surround sound. Apply your effects on this program instead.")
                visible: true
                showCloseButton: true
            }
        }
    }
}
