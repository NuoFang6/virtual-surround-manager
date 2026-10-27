pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigamiaddons.formcard as FormCard

FormCard.FormCardPage {
    id: root
    title: i18nc("@title:window", "设置")

    function update_lfe_checks() {
        balancedRadio.checked = frontendManager.lfeMode === "balanced";
        referenceRadio.checked = frontendManager.lfeMode === "reference";
        impactRadio.checked = frontendManager.lfeMode === "impact";
    }

    Component.onCompleted: update_lfe_checks()

    Connections {
        target: frontendManager
        function onLfeModeChanged() {
            root.update_lfe_checks();
        }
    }

    FormCard.FormHeader {
        title: i18nc("@title:group", "常规")
    }

    FormCard.FormCard {
        FormCard.FormSwitchDelegate {
            id: autostartEnabled
            text: i18nc("@label:switch", "登录时自动启动应用")
            checkable: true
            checked: frontendManager.autostartEnabled
            onToggled: frontendManager.autostartEnabled = checked
        }

        FormCard.FormSwitchDelegate {
            id: virtualSurroundAutoEnabled
            text: i18nc("@label:switch", "自动启用虚拟环绕声")
            checkable: true
            checked: frontendManager.virtualSurroundAutoEnabled
            onToggled: frontendManager.virtualSurroundAutoEnabled = checked
        }

        FormCard.FormComboBoxDelegate {
            id: startupUi
            text: i18nc("@label:combobox", "应用行为")
            description: i18nc("@info:description", "应用启动和运行的方式。<br>重启程序后生效。")
            displayMode: FormCard.FormComboBoxDelegate.ComboBox
            editable: false
            textRole: "label"
            valueRole: "key"
            model: [
                {
                    label: i18nc("@option", "显示系统托盘图标"),
                    key: "showTray"
                },
                {
                    label: i18nc("@option", "显示系统托盘图标，并在启动时隐藏窗口"),
                    key: "showTrayHideWindow"
                },
                {
                    label: i18nc("@option", "隐藏系统托盘图标"),
                    key: "hideTray"
                }
            ]
            Component.onCompleted: currentIndex = indexOfValue(frontendManager.startupUi)
            onCurrentValueChanged: frontendManager.startupUi = currentValue
        }
    }

    FormCard.FormHeader {
        title: i18nc("@title:group", "音频")
    }

    FormCard.FormCard {
        FormCard.FormSwitchDelegate {
            id: upmixEnabled
            text: i18nc("@label:switch", "将立体声上混为环绕声")
            description: i18nc("@info:description", "使用被动环绕解码把立体声音频上混为虚拟 7.1 声道。原生多声道音频会原样通过。立即生效。")
            checkable: true
            checked: frontendManager.upmixEnabled
            onToggled: frontendManager.upmixEnabled = checked

            Connections {
                target: frontendManager
                function onUpmixEnabledChanged() {
                    // The toggle above replaced this binding with a plain value, so
                    // re-create it on every change - a switch the user refused has to
                    // show the old state again
                    upmixEnabled.checked = Qt.binding(function () {
                        return frontendManager.upmixEnabled;
                    })
                }
            }
        }
    }

    FormCard.FormHeader {
        title: i18nc("@title:group", "低频处理")
    }

    FormCard.FormCard {
        Controls.ButtonGroup {
            id: lfeModeGroup
            buttons: [balancedRadio, referenceRadio, impactRadio]
        }

        FormCard.FormRadioDelegate {
            id: balancedRadio
            text: i18nc("@label:radio", "均衡听感（推荐）")
            description: i18nc("@info:description", "上混立体声时，把主声道的低频以 −6 dB 混入 LFE，并自动对齐 HRIR 与 LFE 的相位；低频更饱满、更自然。")
            onClicked: frontendManager.lfeMode = "balanced"
        }

        FormCard.FormRadioDelegate {
            id: referenceRadio
            text: i18nc("@label:radio", "自然耐听")
            description: i18nc("@info:description", "只处理原生 LFE，不额外添加低频。最接近原始录音，适合长时间聆听。")
            onClicked: frontendManager.lfeMode = "reference"
        }

        FormCard.FormRadioDelegate {
            id: impactRadio
            text: i18nc("@label:radio", "低音劲感增强")
            description: i18nc("@info:description", "上混开启时，在均衡听感的基础上把 LFE 低频送量从 −6 dB 提高到 0 dB；相位对齐后低频密度、下潜和冲击力明显增强。")
            onClicked: frontendManager.lfeMode = "impact"
        }
    }
}
