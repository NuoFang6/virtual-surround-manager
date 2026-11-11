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
            text: i18nc("@label:switch", "跨预设使用列表中位参考")
            description: i18nc("@info:description", "关闭时，每个 HRIR 都以七路不相关等功率粉红噪声的 A 计权参考静态校准到比直出低 15.5 dB，为 −1.5 dBFS 限幅门槛保留 14 dB 固定峰值余量；开启时可改用更低的当前 HRIR 列表中位参考，但绝不抬高该上限。两种模式都只在建图时确定增益，不跟随播放内容变化。")
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

        FormCard.FormButtonDelegate {
            enabled: false
            text: i18nc("@label", "输出安全与响度")
            description: i18nc("@info:description", "使用 14 dB 固定峰值余量；HRIR 和设备 EQ 都采用建图时确定的静态校准。最终始终经过 8× 多相重建峰检测、5 ms lookahead、−1.5 dBFS、100 ms release 的立体声联动限幅。基准增益不会随节目变化；只有可能过载时限幅器才会降低增益。限幅统计仅在主窗口可见时启用，隐藏到托盘时停止统计。不是认证 true-peak 限幅或听力保护。")
            trailingLogo.visible: false
        }
    }

    FormCard.FormHeader {
        title: i18nc("@title:group", "输出设备 EQ 校准")
    }

    FormCard.FormCard {
        FormCard.FormSwitchDelegate {
            id: outputEqEnabled
            text: i18nc("@label:switch", "为当前输出设备启用 EQ")
            description: i18nc("@info:description", "当前物理输出：%1。每个设备独立保存开关和配置，默认关闭；切换设备时自动应用该设备的选择。", frontendManager.outputDeviceName)
            checkable: true
            enabled: frontendManager.outputEqFileName !== "未选择"
            checked: frontendManager.outputEqEnabled
            onToggled: frontendManager.outputEqEnabled = checked

            Connections {
                target: frontendManager
                function onOutputEqChanged() {
                    outputEqEnabled.checked = Qt.binding(function () {
                        return frontendManager.outputEqEnabled;
                    })
                }
            }
        }

        FormCard.FormButtonDelegate {
            text: i18nc("@action:button", "选择 Equalizer APO 参数 EQ")
            description: i18nc("@info:description", "当前文件：%1。接受 Preamp、PK、LSC 和 HSC；选择文件不会自动启用。", frontendManager.outputEqFileName)
            onClicked: frontendManager.select_output_eq_file()
        }

        FormCard.FormButtonDelegate {
            enabled: false
            text: i18nc("@label", "EQ 增益状态")
            description: frontendManager.outputEqStatus
            trailingLogo.visible: false
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
