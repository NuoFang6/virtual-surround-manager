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
    title: i18nc("@title:window", "虚拟环绕声管理器")

    // Set right before quitting so the confirmation is not shown twice
    property bool forceQuit: false

    // Poll the auto upmixer's detector only while this window is in the
    // foreground; the text itself is owned by FrontendManager so it survives
    // page navigation and stays in sync with the tray/UI switches.
    Timer {
        interval: 250
        repeat: true
        running: root.visible
        triggeredOnStart: true
        onTriggered: frontendManager.refresh_input_detection()
    }

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
        title: i18nc("@title:dialog", "音频正在播放")
        subtitle: i18nc("@info:dialog", "关闭虚拟环绕声会把正在播放的音频移到默认输出设备。是否继续？")
        standardButtons: Controls.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "保持开启")
                onTriggered: disableConfirmDialog.close()
            },
            Kirigami.Action {
                text: i18nc("@action:button", "关闭虚拟环绕声")
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
        title: i18nc("@title:dialog", "音频正在播放")
        subtitle: i18nc("@info:dialog", "退出应用会把正在播放的音频移回默认输出设备。是否退出？")
        standardButtons: Controls.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "取消")
                onTriggered: quitConfirmDialog.close()
            },
            Kirigami.Action {
                text: i18nc("@action:button", "退出")
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
                text: i18nc("@title:window", "设置")
                icon.name: "preferences-other"
                onTriggered: pageStack.layers.push(settingsPage)
            },
            Kirigami.Action {
                text: i18nc("@action", "关于虚拟环绕声")
                icon.name: "help-about"
                onTriggered: pageStack.layers.push(aboutPage)
            }
        ]
    }

    pageStack.initialPage: Kirigami.Page {
        id: mainPage
        title: i18nc("@page:title", "快捷设置")
        actions: [
            // Global toggle for enabling or disabling the virtual surround routing
            Kirigami.Action {
                id: toggleVirtualSurroundAction
                text: i18nc("@action:switch", "已启用")
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
                text: i18nc("@label", "发生错误：%1", frontendManager.errorMessage)
                visible: frontendManager.errorMessage.length > 0
                showCloseButton: true
            }

            Kirigami.InlineMessage {
                id: detectionMessage
                Layout.fillWidth: true
                type: Kirigami.MessageType.Information
                text: frontendManager.inputDetectionText
                visible: text.length > 0
            }

            Controls.Label {
                Layout.fillWidth: true
                text: frontendManager.hrirAnalysisText
                visible: text.length > 0
                wrapMode: Text.Wrap
                font: Kirigami.Theme.smallFont
                opacity: 0.8
            }

            Kirigami.Card {
                actions: [
                    Kirigami.Action {
                        text: i18nc("@action", "打开文件夹")
                        icon.name: "folder-open"
                        onTriggered: frontendManager.open_hrir_wav_folder()
                    },
                    Kirigami.Action {
                        text: i18nc("@action", "重新加载")
                        icon.name: "view-refresh"
                        onTriggered: frontendManager.load_hrir_wav_files()
                    }
                ]
                Layout.fillHeight: true
                Layout.fillWidth: true
                header: Kirigami.Heading {
                    text: i18nc("@header", "虚拟化配置文件")
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
                text: i18nc("@message", "<b>添加更多配置文件</b><br>打开文件夹并把 WAV 文件放进去即可。你可以在 <a href=\"https://airtable.com/appayGNkn3nSuXkaz/shruimhjdSakUPg2m/tbloLjoZKWJDnLtTc\">HRTF 数据库</a> 中找到数百个此类文件。")
                onLinkActivated: function (link) {
                    Qt.openUrlExternally(link);
                }
                visible: true
                showCloseButton: true
            }
            Kirigami.InlineMessage {
                Layout.fillWidth: true
                text: i18nc("@message", "<b>与 EasyEffects 配合使用</b><br>请在 EasyEffects 中排除使用环绕声的应用，改为在本程序上应用音效。")
                visible: true
                showCloseButton: true
            }
        }
    }
}
