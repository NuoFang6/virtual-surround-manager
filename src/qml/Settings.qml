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
            Connections {
                target: frontendManager
                function onAutostartEnabledChanged() {
                    autostartEnabled.checked = Qt.binding(function () {
                        return frontendManager.autostartEnabled;
                    });
                }
            }
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
            description: i18nc("@info:description", "根据立体声相关性推导虚拟 7.1 分布，不能恢复原录音中不存在的独立声道。检测到非前置声道内容后切换旁路；48 kHz 下处理延迟约 128 ms，切换有 20 ms 淡化，原生 LFE 另加 2× 数字增益。")
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
            text: i18nc("@label:switch", "预设参考电平匹配")
            description: i18nc("@info:description", "以七路不相关、等功率粉红噪声为参考，积分各 HRIR 的 A 计权输出功率，匹配列表中位数（最多 ±8 dB）。只改整体增益；不是 LUFS，也不保证不同节目等响。默认关闭。")
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
            text: i18nc("@label:switch", "输出重建峰值限幅")
            description: i18nc("@info:description", "8× 多相重建峰检测、5 ms lookahead、−1.5 dBFS、100 ms release 的立体声联动限幅。音频不重采样，只有检测器过采样；瞬态仍会影响同一前瞻窗口的低电平样本。不是认证 true-peak 限幅或听力保护。默认关闭。")
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
            text: i18nc("@label:radio", "低频增强（默认）")
            description: i18nc("@info:description", "把上混后的 7 路主声道单声道求和以 −6 dB 送入扩散 LFE 通路：120 Hz BW4、扩散音色 IR、每耳对齐/全通。它增加的是非前置的扩散低频层，不是抬高前置 HRIR。只在立体声上混时生效；原生多声道直通不加。")
            onClicked: frontendManager.lfeMode = "balanced"
        }

        FormCard.FormRadioDelegate {
            id: referenceRadio
            text: i18nc("@label:radio", "无额外低频增强")
            description: i18nc("@info:description", "不从主声道生成附加低频；仍保留原生 LFE 的低通、音色与数字增益处理。不宣称对所有耳机或听者更中性、更耐听。")
            onClicked: frontendManager.lfeMode = "reference"
        }

        FormCard.FormRadioDelegate {
            id: impactRadio
            text: i18nc("@label:radio", "较强低频增强")
            description: i18nc("@info:description", "把同一 mono 主声道和的 LFE 送量提高到 0 dB，仍经 120 Hz BW4、扩散 IR 和相位对齐。实际增量取决于节目低频分布；请留意峰值和听音音量。")
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
            description: i18nc("@info:description", "低通为 120 Hz Butterworth 4 阶；每耳相位按所选音色的实际响应与等权主声道和拟合。默认是七对 HRIR 的相干平均（扩散场近似），按中心 HRIR 的 40–80 Hz 幅度归一化。δ 仅表示不加 HRIR 音色，并非耳机校准；中心选项使用中置 HRIR。")
            displayMode: FormCard.FormComboBoxDelegate.ComboBox
            editable: false
            textRole: "label"
            valueRole: "key"
            model: [
                { label: i18nc("@option", "扩散场 HRIR（默认）"), key: "diffuse" },
                { label: i18nc("@option", "δ 脉冲（不加 HRIR 音色）"), key: "delta" },
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
