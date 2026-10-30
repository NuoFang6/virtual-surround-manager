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

        FormCard.FormSwitchDelegate {
            id: presetLevelMatch
            text: i18nc("@label:switch", "统一预设响度")
            description: i18nc("@info:description", "按 A 计权能量把当前 HRIR 预设匹配到列表的中位电平（最多 ±8 dB）。只改变整体增益，不改变音色；切换预设时音量更一致。默认关闭。")
            checkable: true
            checked: frontendManager.presetLevelMatch
            onToggled: frontendManager.presetLevelMatch = checked

            Connections {
                target: frontendManager
                function onPresetLevelMatchChanged() {
                    presetLevelMatch.checked = Qt.binding(function () {
                        return frontendManager.presetLevelMatch;
                    })
                }
            }
        }

        FormCard.FormSwitchDelegate {
            id: outputLimiter
            text: i18nc("@label:switch", "输出保护限幅")
            description: i18nc("@info:description", "在最终输出前加 5 ms lookahead、−1.5 dBFS、100 ms release 的立体声联动峰值限幅；正常电平不改变声音，只增加 5 ms 延迟。默认关闭。")
            checkable: true
            checked: frontendManager.outputLimiter
            onToggled: frontendManager.outputLimiter = checked

            Connections {
                target: frontendManager
                function onOutputLimiterChanged() {
                    outputLimiter.checked = Qt.binding(function () {
                        return frontendManager.outputLimiter;
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
            description: i18nc("@info:description", "上混立体声时，把主声道的低频以 −6 dB 混入 LFE，并自动对齐 HRIR 与 LFE 的相位；低频更饱满、更自然。使用 δ/中心音色时，校正后平均残差 >30° 会自动禁用送量；默认扩散场与主声道同源，相位自动抵消。")
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
            description: i18nc("@info:description", "上混开启时，在均衡听感的基础上把 LFE 低频送量从 −6 dB 提高到 0 dB；相位对齐后低频密度、下潜和冲击力明显增强。使用 δ/中心音色时，校正后平均残差 >15° 退回 −6 dB，>30° 禁用送量；默认扩散场通常不会触发。")
            onClicked: frontendManager.lfeMode = "impact"
        }
    }

    FormCard.FormHeader {
        title: i18nc("@title:group", "LFE 音色")
    }

    FormCard.FormCard {
        FormCard.FormComboBoxDelegate {
            id: lfeTimbre
            text: i18nc("@label:combobox", "LFE 音色/空间")
            description: i18nc("@info:description", "低通固定为 Butterworth 4 阶，每耳对齐默认开启；这里只切换 LFE 的脉冲/音色。默认扩散场 HRIR（与主声道同源，相位自动抵消）；第二为 δ 无染色；第三为中心 HRIR。切换会重建效果图，请 A/B。")
            displayMode: FormCard.FormComboBoxDelegate.ComboBox
            editable: false
            textRole: "label"
            valueRole: "key"
            model: [
                { label: i18nc("@option", "扩散场 HRIR（默认，7 对平均）"), key: "diffuse" },
                { label: i18nc("@option", "δ 脉冲（无染色，第二）"), key: "delta" },
                { label: i18nc("@option", "中心 HRIR（第三）"), key: "center" }
            ]
            Component.onCompleted: currentIndex = indexOfValue(frontendManager.lfeTimbre)
            onCurrentValueChanged: frontendManager.lfeTimbre = currentValue

            Connections {
                target: frontendManager
                function onLfeTimbreChanged() {
                    lfeTimbre.currentIndex = lfeTimbre.indexOfValue(frontendManager.lfeTimbre);
                }
            }
        }
    }
}
