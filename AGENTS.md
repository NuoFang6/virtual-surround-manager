# 项目手册（AGENTS.md）

> **本文件包含项目上下文及历史记录，不是正确性证明。** 2026-11-09 用户要求：不要完全信任接手前的文档或代码；以接口规范、实际实现与可复现实测为依据，冲突时更新记录。
> 面向未来的 LLM/协作者：先阅读本文件；历史数据的适用版本必须区分。
> 最后更新：2026-11-10（深夜：FFT 4096 回滚定稿 + 文档精简，见 §0F/§0G）。

## 0. 2026-11-09～10 审查修复（优先于下方旧记录）

### 工作授权
- 明确的 bug、健壮性改进和必要重构直接实施，不必逐项询问；涉及音色/延迟等设计取舍才询问。
- **不使用子智能体**（用户本轮明确要求）。测试不得干扰宿主音频；sudo/系统安装前仍须告知，不保存密码。
- 下方“唯一真相”“已验证”等历史措辞不能代替新验证。本轮没有重新认可所有旧声学论断。

### 实现更新
- PipeWire 模块增加 destroy listener 和世代检查：模块自行销毁后清除 active/pending 引用，修复服务断开后退出 double-free。连接状态不再靠指针非空判断；断开后前端关闭启用状态，服务恢复后点击启用可重新连接。定时等待使用 PipeWire 的 deadline API。
- 换图必须确认新输出链接和桥链接；失败回滚不销毁旧控制代理、不提前改变上混状态，不再“两边均未确认仍返回成功”。延迟销毁按模块身份/世代核验。
- 新 `src/hrir_wav.{h,cpp}` 统一校验：14ch、完整 RIFF/fmt/data、帧布局、PCM16/24/32/float32、完整 EXTENSIBLE GUID；容器位深与有效位深分开，修复24有效位/32容器误读；拒绝NaN/Inf。所有音色（包括center）共用校验。10秒上限改为拒绝而非只截LFE，另设256MiB音频负载内存上限。
- δ/diffuse中间IR统一float32、QSaveFile原子写入，不再逐tap夹到±1；参考幅度不再夹到0.05～1。默认系数/目标未主动调音；合法低电平/高峰值导入文件的行为会纠正。旧未调用的 `lfe_alignment()` 已删除；低通历史枚举仍在内部分析API，UI仍固定BW4/每耳。
- DSP FIFO明确补足FFT块延迟，所有测过的块长/变块长上混与旁路均为6144样本。auto不再要求非L/R连续持续6144+crossfade样本，而是检测事件随音频延迟并保留一秒旁路迟滞；短事件不再全吞。20ms过渡仍会衰减新原生事件的开头，不能描述为任意瞬态逐样本无损。
- 大块不再写16384帧临时数组；普通stereo latency端口接线修复；限幅在延迟后的实际立体声样本上守住ceiling，运行中控制生效，默认5ms/−1.5dBFS/100ms不变。
- 换歌门控变为独立图节点 `gateL/gateR:Mult`（SPA_PROP_params），不再操作用户mute/volume，也不再源流静音回退。上混150ms、关闭上混30ms，之后100ms淡入；失效/换图/关闭时解除应用自己的门控。
- 重载HRIR列表会实际换图（包括同路径文件更新），失败保留上一选择；重建前失效响度缓存，失败结果不永久缓存。空列表也允许关闭已启用音效。
- 自启动原子写入/错误回滚，AppImage Exec转义，Flatpak portal完成回调；无可用托盘不隐藏主窗口；托盘切预设时延迟删除正在派发信号的QAction。后两种打包环境未做运行时集成验收。

### 本轮证据
- 基线原有测试通过，但新增探针复现：WAV fmt堆越界、断连double-free、NaN被接受、24-in-32参考0.25误读0.05、7ch center被接受；源流/输出用户mute被解除；列表显示B而仍运行A。
- 基线上混冲激延迟：512块5632样本、1024块5120、4096块2048，而旁路6144；修后1/256/512/1000/1024/2048/4096/8192/16385/32768及混合块长逐样本一致，6144样本。
- 基线约43ms中置burst输入峰0.25→输出0；修后峰0.25（原生LFE补偿2×）。限幅孤立脉冲输入10→1.006369、100→2.627524；修后均≤0.841395。
- ASan断连回归修后exit0、valid0/enabled0；隔离服务重启后再次启用valid1/enabled1。源流与效果输出mute前后均true。center7ch拒绝。重载后effects generation1→2。
- 图内门控用null-sink monitor录音：最大10ms窗RMS0.002331，最小0，16个静音10ms窗（量化约150ms门控）；录音Link快照留档。
- 隔离假MPRIS+mpv：7次预设/上混切换（含24-bit HRIR）+正常退出，Pause0；强杀测试实例对照Pause1。该测试未做完整切换电平/音频空洞阈值统计，不把旧数据当本轮结果。
- 仓库新增CTest：`hrir_wav`、`hrir_analysis`、`plugin_regression`、`plugin_smoke`，加原appstream共5项。命令：`cmake -B build -G Ninja -DBUILD_TESTING=ON -DBUILD_LADSPA_TEST_HOST=ON && cmake --build build && ctest --test-dir build --output-on-failure`。分析测试强制连接空的私人runtime，不连接宿主音频。
- 临时复现源码/日志：`/tmp/vsm-audit-20261109-HOJMD2/`；原审查报告 `audit-report.md` 为修复前快照。临时工具构建/环境失败曾发生并已修正，不混淆为项目测试通过。

### 待决策/未覆盖
- **center_image 保持 no-op（2026-11-10 晚决定）**：0 与 1 输出逐样本相同；上游 FreeSurround、Kodi、Real3D-VST、旧提交都只保存该参数。2026-11-10 白天曾实现后处理，晚已按用户要求还原（§0D）。
- 128ms目标延迟、上混开关重建带来的时间错位/灌注，以及延迟传播到应用端仍是设计/集成问题，不因本轮修复就宣称解决。
- 未安装系统版本、未重启宿主音频或应用、未重新打包AppImage/Flatpak；未完成真实桌面托盘/portal测试。不会宣称已消除所有可能bug。


## 0A. 2026-11-10 声学理论复核（理论解释以此及 docs/acoustics.md 为准）

用户要求重新核实理论、科学和设计合理性。**下方§2～14为历史设计/测试记录，包含本轮已撤回的结论，不能继续当作当前科学依据。** 完整公式、数值、原始资料及限制见 `docs/acoustics.md`；可复跑脚本 `tools/analyze_acoustics.py`。

### 撤回/修正的主张
- 配置`diffuse`实际为**七方向HRIR相干平均**`ΣH/7`，不是物理扩散场功率平均`Σ|H|²/7`；UI更名但配置键和音色保持兼容。七个水平方向也不足以代表完整球面扩散场。
- +5.5dB来自二手文章对特定Dolby双耳行为的转述，不是任意耳机/HRIR的通用校准；Apple官方LFE Off只表示不做距离建模。原生2×保留为产品数字增益约定，不再声称已达到普适标准。当前60Hz LFE/FC双耳功率比27_95 +6.008dB、AtmosFar +7.458dB。
- δ仅“不加HRIR音色”，不是严格中性/耳机校准；完整通路仍有低通/全通/延迟/耳机响应。全通单位幅度不等于零群延迟或不可闻；默认全通20–120Hz群延迟约0.361～0.367ms。
- 206样本是频带拟合延迟，不是每频率群延迟。BW4@120为−3.0103dB，24dB/oct是渐近斜率；相对LR4在80/100/120Hz为+1.40/+2.51/+3.01dB，不是整个频段固定+3dB。
- 等权相干模型下HRIR相位抵消，不保证任意上混分布不抵消。实际插件9种输入关系×11频点测试，impact双耳功率增量可达27_95 +8.65dB、sbx100 +12.38dB；“固定增加3～6dB”撤回。balanced/impact是附加重复低频的听感效果，不是恢复真实LFE或标准分频。
- 当前限幅是**采样峰值**，不是true-peak或听力保护；采样峰0.84可重建为1.18794。0.30固定余量也不保证任意相关多声道不削波。
- `node.latency`只是建议，不保证minimum quantum；HRIR等长不保证相位/时延相同，0-block-latency卷积不表示整链零延迟。HRIR不自动包含当前耳机的校准响应；不可把“HRTF近似最小相位”推广为“相位均不可闻/可去掉ITD”。
- 不能把reference称为普适“最耐听”，也不能以dBFS阈值保证可闻性；UI已删除这类客观化听感承诺。门控会丢失内容，也会影响共享效果图的其他流，是折中而非无损处理。

### 本轮算法修正
- 相位误差改为圆周RMS（359°等价−1°），仅斜率拟合解缠；全通目标也按圆周误差搜索，不再把正主值相位一概当作非因果。零幅度的相位未定义，不能视作0°通过；数值容差改为相对幅度，整体缩小HRIR不改变相位有效性。
- 送量额外加入独立密集网格的最大参考相位≤90°检查（`cos φ≥0`的交叉项条件）；δ/中心按IR长度细化至≤min(1Hz,1/(4T))，3200万帧频点评估预算用尽则不通过。仍是参考筛查，不是对所有内容的安全证明。100ms纯延迟会在原10Hz网格整圈混叠，新测试能发现并禁止send。
- 预设电平匹配改为**七路不相关等功率粉红噪声参考**的A计权输出功率积分，240个log中点、上限≤Nyquist；不再相干相加C/FL/FR。不是LUFS，也不保证节目等响。新默认列表27_95 trim约−2.00dB，开关仍默认关闭。
- 共享HRIR频响计算使用复指数递推减少sin/cos重复计算；新增`src/acoustics.h`集中圆周相位与A计权公式。
- 新回归：极性/相对纯延迟不变性、幅度减半−6.0206dB、低采样率Nyquist、极低电平相位有效性、零响应相位、≤180°圆周RMS及100ms延迟混叠。原五项CTest保持通过。

### 验证边界与设计待定
- 原始资料已核对：ITU-R BS.1770-5（K计权、节目门限、true-peak）、Apple Logic LFE Off、W3C EQ Cookbook、PipeWire本机官方手册/HeSuVi映射。不要扩大资料适用范围。
- 数值目录：`/tmp/vsm-acoustics-20261110-72dUeg/`，含实际LADSPA探针、全部预设结果及原始资料（未把下载资料执行为代码）。没有改动宿主音频。
- 固定grid仍可能漏窄峰，密集网格也不是连续域数学保证；正式保证需要定义输入集合或内容自适应方案。
- BW4、原生2×、平均音色、0.30、20ms淡化、128ms预算未随意更换；它们是选择，不因不是唯一标准就必然错误。真正扩散场目标、true-peak限幅、原生LFE与合成send是否分开相位处理仍需设计决定；center_image 已决定保持 no-op（§0D）。

## 0B. 2026-11-10 白天音质架构（**已被 §0C–§0G 多轮回滚，仅存架构要点**）

本轮曾把上混 FFT 改为 2048/64 ms、删除合成 LFE send、实现搁架/功率平均 IR/center_image。这些低频改动已全部回滚（见 §0F/§0G）；**仍然有效并保留的架构要点**：

- 稳定桥 sink + 可重建效果图 + 运行时 upmix_enable / lfe_send 控制（上混/LFE 模式切换不重建图）；换 HRIR/音色/电平匹配/限幅仍重建图，跨世代逐样本 crossfade 未实施。
- 常驻 auto 插件（旁路分支补原生 LFE ×2）、图内 gateL/gateR 应用门控节点、输出重建峰限幅（8×/48-tap）、七路粉噪电平匹配、`hrir_wav` 统一校验、断连/世代处理。
- 已知残留：图重建短 IR 空洞（现为 ~128–150 ms 灌注）、延迟传播到应用端仍是集成问题。
- 性能：限幅检测器 ~1.8%（1 GHz 单核）。

## 0C. 2026-11-10 晚 低频回归修正（已回滚，仅存教训）

教训（对后续工作有效）：搁架/功率平均等"理论上更正确"的低频方案被用户听感否决——稳态 RMS 相同不代表听感相同，LFE 层的**立体声/时间结构**（相干平均 IR 时域重心 ~14 样本 vs 功率版 ~3.9）决定"劲感"。测量数据原记录已精简；方法：隔离会话同录音窗 A/B + IR 时频分析。

## 0D. 2026-11-10 晚 还原 FreeSurround center_image/归一化（**现行**）

用户要求：不要改动 FreeSurround，还原原版，`center_image` 保持未使用，同时尽量保证音质。

### 上游核对

- `tygyh/FreeSurround` `source/FreeSurroundDecoder.cpp`：`set_center_image()` 只写成员；`buffered_decode()` 不读取。
- `kodi-adsp/adsp.freesurround` `src/FreeSurroundDecoder.cpp`：`SetCenterImage()` 只写 `m_CenterImage`；解码不读。
- `NuoFang6/Real3D-VST` `Source/dsp/freesurround_decoder.cpp`：同样只写；头文件 `freesurround_decoder.h:155-161` 虽描述 1.0/0.0/0.7 语义，但实现是 no-op。
- 本仓库旧提交 `f615312` 与上游一致：只保存，不使用。

结论：**中心重分配和全局功率归一化是 2026-11-10 白天本 fork 新增的，不是 FreeSurround 原版行为。** 头文件的语义描述是未实现的意图。

### 已还原

- 删除 `buffered_decode()` 中的 C→FL/FR phantom 分配和“7 主声道总功率归一化”，恢复上游的
  `signal[c][f] = polar(amp_total * 插值权重, phase)` 与原始声像/电平律。
- graph 不再设置 `"center_image"`；LADSPA 端口保留兼容，但只存储、不参与 DSP。
- 保留 fork 中与 FreeSurround 音质无关的适配：`set_sample_rate`、`flush`、LFE bin 清零、48k/44.1k 图；这些不改变上混语义。
- LFE 路径为 **mono LFE send + 相干 diffuse IR + 每耳对齐/allpass**（§0F），不是 FreeSurround 解码器的一部分。

### 验证

- 上游解码器 + `lfe_send=0.501` + 相干 diffuse IR 与旧版 `f615312` 行为一致（§0F/§0G 实测：IR 218 帧、能量重心 ~14、balanced/reference 形状一致）。
- `plugin_regression` 改为断言 `center_image=0` 与 `=1` 输出**逐样本相同**；`plugin_smoke` 的 440 Hz FC 期望回到上游的 **0.66**（不再是中心重分配后的 0.476）；CTest **5/5**。
- 应用侧 sink 延迟随 FFT 回到 128 ms 量级（filter-graph 报主/LFE 分支平均；未重测具体值，需要时用 pw-dump 实测）。
- 原生多声道 bypass 不加 send、无搁架。

### 本机安装（2026-11-10 09:01）

- 旧 `/usr/bin` 实例 PID 2990901 已 SIGTERM 正常退出；旧安装备份在 `/tmp/vsm-sys-backup-20261110-090118/`（旧 app `74288736…` 485096 B、旧 plugin `29e84a7a…` 379096 B）。
- 安装新构建：`/usr/bin/virtual-surround-manager` = `21a21dbe…`（485368 B）、`/usr/lib/ladspa/freesurround_ladspa.so` = `268a33f2…`（366808 B）；`/usr/lib64` 是 `/usr/lib` 的符号链接，`/usr/lib64/ladspa` 读到同一哈希。
- 启动安装版 PID 3033920：日志确认 `Using FreeSurround plugin '/usr/lib/ladspa/freesurround_ladspa.so'`、`LFE send balanced 0.501`、`main 3.92 ms / LFE 0.00/0.02 ms`、allpass 6.28 kHz Q 0.127、`Enabled routing`；HRIR 使用用户 `~/.local/share/virtual-surround-manager/hrir_wav/27_95_142.wav`。
- 备份在 `/tmp`，非持久；长期回退需重新构建或保留该目录。

### 影响与限制

- 旧版的中心/声像律回来；不再宣称 `center_image` 已实现。若要按头文件语义真正实现，应在**通道分配表/网格**层面调整 C 权重，而不是事后 phantom；本轮不做。
- `lfe_send` 的 0.501/1.0 与旧版一致，相干 diffuse IR 下与旧版电平匹配；若以后改上混 FFT 或 IR，需要重测（§0G 证据：FFT 尺寸对低频不是中性的）。
- 旧 shelf 的主 HRIR 前置低频提升不再存在；用户听到的低频层来自独立 mono diffuse LFE，而不是前置 HRIR 抬高。

## 0E. 2026-11-10 晚 LFE IR 结构修正（已整节回滚，见 §0F）

曾实现 stereo hybrid IR（65% 相干 + 35% 方向功率，4096 帧）。用户判断低频被改废，全部回滚。教训：在相干平均上混幅度域只会损失旧版冲击；LFE IR 结构改动必须先用时频/包络证据而非纯 RMS 评估。

## 0F. 2026-11-10 深夜 低频全部回滚（**低频行为现行；FFT 尺寸见 §0G**）

用户判断 2026-11-10 的低频改动（白天功率平均 IR → 晚间 hybrid IR）全部失败，要求**低频全部回滚到大改前 `f615312` 的行为，只保留 bug 修复和错误纠正**。

### 删除（低频专属）
- `binaural_bass` LADSPA label（BassInstance/descriptorBass/kDescriptors 第四项）；`quality_dsp.h` 整个文件；`dsp_config.h` 的 `bass_taps/bass_latency/balanced_boost_db/impact_boost_db`。插件现在只有三个 label：`freesurround71`、`freesurround71_auto`、`output_limiter`。
- `src/ir_processing.{h,cpp}`（energy_average_ir / diffuse_ir_pair / ir_response 模板化后保留在 pipewire_manager.cpp 内）；`src/CMakeLists.txt`、`tests/CMakeLists.txt`、两个测试的引用同步删除。
- `plugin_regression_test` 的全部 bass 搁架测试（对称/非负频响/DC/HF/相位/latency）；`hrir_analysis_test` 的 energy_average 与 diffuse pair 测试。
- 图中搁架节点（此前已不插入）及其残留注释。

### 恢复（= f615312 低频行为）
- `write_lfe_diffuse_ir`：**七对 HRIR 相干平均**（每耳 Σ/7），40–80 Hz（40/50/60/70/80 五点）平均幅度按中心 HRIR 参考增益归一化，2 声道 IR（convLFE_L/R 用 ch0/1）。实现改用保留的 `HrirWav` 统一校验 + float32/QSaveFile 原子写入（bug 修复保留，不改数值）。
- `lfe_alignment_configured` 的 diffuse 分支恢复相干 timbre 响应（`Σ channel_resp/7 × scale`），因此默认 diffuse 下 HRIR 相位精确抵消、所有预设通过门控（27_95 实测 main 4.29 ms、残差 ~11.8°，见 §6 历史数据）。
- **30°/15° send 门控**：`lfe_effective_residual_degrees`/`lfe_send_safe`/`lfe_impact_allowed` 恢复；balanced = 上混分支 mono 主声道和 ×0.501，impact ×1.0（>15° 退回 −6 dB，>30° 完全禁用）。
- UI：diffuse 标签恢复"扩散场 HRIR"；Settings.qml/README/插件 README/docs/acoustics.md 同步回退为相干平均描述。

### 相对 f615312 的小改进（有意保留）
- 门控值在建图时持久化为 `lfe_send_balanced_`/`lfe_send_impact_`，运行时 `update_processing` 只能在两者间选择，不会把未门控的 send 写进未对齐的图（旧版运行时切换模式直接写常量）。
- send/门控/对齐仍工作在现行图架构上（常驻 auto 插件 + 运行时 upmix_enable/lfe_send 控制、gate 门控节点、稳定桥），而不是旧图结构。

### 保留的 bug 修复/错误纠正（不属于低频）
auto 检测 crossfade/迟滞、`hrir_wav` 统一校验、断连/世代处理、输出门控、限幅（8× 重建峰）、电平匹配、`center_image` 上游 no-op 还原（§0D）、CTest 其余断言。（FFT 尺寸最初保留 2048，后经 §0G 证据回滚为 4096。）

### 验证状态（2026-11-10 深夜回滚版）
- Release CTest **5/5**（bass 搁架/energy-average 测试随删除；`hrir_analysis` 恢复相干 diffuse 后主 4.29 ms、残差 11.73°，与 f615312 历史值一致）。
- 隔离 PipeWire/D-Bus/null sink e2e（27_95、diffuse、per-ear、立体声正弦 0.2）：balanced 相对 reference 双耳幅度 **40/60/80/100/120 Hz = +3.67/+3.67/+3.62/+3.36/+2.67 dB**（0.501 send 生效；形状与 §6 历史 f615312 的 +3.57/+3.18/+2.73/+2.07 一致，绝对值差异来自测量方法：本轮为单音正交解调，旧数据为多音 RMS）。日志 `LFE mode balanced (send 0.501, residual within 30 deg)`、all-pass `5790.06 Hz Q 0.143 (res 5.81°)`（旧版 5.8°）。这些测量在 FFT 2048 下进行；4096 下的全链路复测见 §0G。
- 生成的 `lfe_diffuse_ir.wav`：**218 帧、2ch、float32**，L/R distinct，能量重心 **14.13** 样本（f615312 历史 ~14；hybrid 版为 4096 帧 mono 化结构）。
- 极低电平（1e-12）相位有效性测试恢复通过：diffuse 增益下限由绝对 `1e-9` 改为 `>0` 相对判断（bug 修复保留语义）。

### 本机安装（2026-11-10 深夜回滚版）
- hybrid 安装版（PID 3050174 已不在运行）备份到 `/tmp/vsm-sys-backup-20261110-prerollback/`。
- 安装：`/usr/bin/virtual-surround-manager` = `a4c3385c…`（485368 B）；`/usr/lib/ladspa` 与 `/usr/lib64/ladspa` 的 `freesurround_ladspa.so` = `3f144f40…`（366320 B），与 `build/bin` 一致。
- 重启 `/usr/bin/virtual-surround-manager`：日志确认 send/对齐/路由正常；用户数据 `lfe_diffuse_ir.wav` 重新生成为 218 帧 2ch（相干平均）。（此轮安装为 FFT 2048 版 `a4c3385c…`，已被 §0G 的 4096 版取代。）
- 测试残留：隔离会话守护进程（`PIPEWIRE_RUNTIME_DIR=/tmp/vsm_rb/run`）已按 environ 识别后精确清除，宿主 PipeWire 未动；宿主应用曾在此过程中被误杀一次并已重启（重启后日志核验正常）。

## 0G. 2026-11-10 深夜 FFT 4096 回滚（**现行定稿**）

用户 A/B 后判定：`v20261110-shelf`/`v20261110-power` 低频最差，`v20261110-rollback`（FFT 2048）仍不如 `v20261030` 的低频音色/细节。调查发现**唯一残留的低频相关差异是解码器 FFT 4096→2048**（当初为把上混延迟从 128 ms 减半到 64 ms 而改，作为"架构"保留了下来）。

**证据**（离线 harness，同一 20–400 Hz 粉噪直接喂两版插件的 `freesurround71`，无 HRIR/无图）：
- 2048 vs 4096：各通道 RMS 均匀 −0.24 dB，但 mono 主声道和 **20–120 Hz 各带 −4.0…−5.0 dB**、120 Hz 以上 ±0.2 dB。机理：23.4 Hz bin（2048）下 20–120 Hz 只剩 2–4 个 bin，逐 bin 幅度/相位差估计变粗，低频 7.1 分配与能量改变。send 内容与主路径低频都来自解码器，故整链低频质感变化（全链路实测整体 −3.1 dB）。
- 4096 回滚后：每通道 RMS 比 0.9989（−0.01 dB 均匀），20–120 Hz 各带差 ≤±0.04 dB，latency 端口 6144。

**实现**：`dsp_config.h` `fft_size` 2048→4096（`upmix_latency`=6144）；CT 测试断言文本去数值化；UI/README/插件 README/acoustics.md 的 2048/3072/64 ms 全部改回 4096/6144/128 ms。**保留的无影响改进**：图架构（常驻插件/运行时控制/门控节点）、hrir_wav 校验、限幅、电平匹配、send 门控持久化、center_image 上游 no-op、float32 IR 中间格式。

**取舍**：上混延迟回到 **128 ms**（6144 样本），图重建短 IR 空洞相应变大（~128–150 ms）。这是音质优先的用户决策，不要为了延迟再改回 2048 而不重测低频。

**验证/部署**：CTest **5/5**；隔离冒烟 `上混FFT 4096 / 128 ms`、`LFE mode balanced (send 0.501…)`、Enabled routing。同录音 8ch 双脉冲/频域复核：关闭对齐时主−LFE−48000 = −201 样本、20–120 Hz 圆周 RMS 108.64°；启用后为 −9 样本、左右耳均 5.81°，与建图预测完全一致。安装 `/usr/bin/virtual-surround-manager` = `832df0e4…`、`/usr/lib(64)/ladspa/freesurround_ladspa.so` = `57c5123b…`（485368 B 应用 / 366320 B 插件）；应用已重启（4096 版）。版本库：`v20261110-rollback` 更新为 4096 版；原 2048 回滚版保留为 `v20261110-fft2048` 供 A/B。**文档精简**：删除 `docs/acoustics-measurements.json`、`docs/acoustics-upmix-measurements.json`、`tools/upmix_acoustics_probe.cpp`（早期审计数据，非当前测量）；§0B/0C/0E 压缩为教训摘要；`docs/acoustics.md` 全文刷新为当前架构与本轮数据。

**版本切换工具**：`~/.local/bin/vsm-version`（`list/current/stop/start/<name>`），版本库 `~/.local/share/virtual-surround-manager/versions/`：`v20261030`（旧版）、`v20261110-fft2048`、`v20261110-shelf`、`v20261110-power`、`v20261110-hybrid`、`v20261110-rollback`（4096 现行）。切换会 sudo 安装应用+插件并重启应用。注意：切换脚本只换二进制；`lfe_diffuse_ir.wav` 由建图时自动重新生成，无需手动清理。

## 0H. 2026-11-11 默认原生 LFE 系数试听后还原（**现行**）

用户先把默认 `27_95_142` 的原生LFE目标定为+5.5 dB，随后依次试听1.8941745×和1.95×，均认为低频不如原来，最终要求恢复 **2.0×**。测量口径为同幅度原生LFE与中心主声道分别通过完整默认路径后，在40/50/60/70/80 Hz五点上合并左右耳功率，再计算比值；不是单独的插件系数，也不外推为所有HRIR/频率的声学标准。

- 隔离 PipeWire/WirePlumber/pipewire-pulse/null sink：2.0×基线实测 **+5.9722 dB**；1.8941745×复测 **+5.4999 dB**；1.95×复测 **+5.7523 dB**；最终恢复2.0×后复测仍为 **+5.9722 dB**，40/50/60/70/80 Hz分别为 +6.0182/+6.0172/+6.0056/+5.9643/+5.8533 dB。
- 原生LFE旁路系数最终恢复为 **2.0**。共享常量与更严格的整数周期插件回归测试保留；系数只作用于原生多声道旁路，balanced/impact合成send、diffuse IR、BW4、对齐与主声道不变。
- 40 Hz 是历史 LFE 音色归一化参考带 40–80 Hz 的起点，不是高通截止；LFE 路径仍处理 40 Hz 以下。该参考带避开 HRIR 最低频更易受测量/窗长影响的区域，并远离 120 Hz 低通转折区。
- 系数定义于 `third_party/freesurround_ladspa/dsp_config.h`，插件及回归测试共用；系统安装和宿主应用重启仍需单独执行。

## 1. 项目概况

- **Virtual Surround Manager**：Berny23 上游项目的硬分叉（hard fork），版本从 0.0.1 重新起算。C++20、Qt6 + Kirigami、CMake/Ninja。
- **功能**：创建 PipeWire filter-chain 虚拟 8 声道 sink（7.1），可选 FreeSurround 上混，然后经 HeSuVi 14 通道 HRIR 卷积做耳机双耳渲染，输出 2 声道。
- **用户背景（重要）**：本仓库用户是 **Real3D-VST 的作者**（基于 FreeSurround 改写的插件，网上搜不到）。历史上 80/111 Hz 的分频值出自用户本人，非外部权威；现已改为 70/90（见 §3）。
- **构建**：`cmake -B build -G Ninja && cmake --build build`，安装前缀 `/usr`（`sudo cmake --install build --config Release`）。启动方式见 §9。
- **关键源码**：`src/pipewire_manager.cpp`（稳定桥 + 可重建效果图 + LFE/主路径对齐 + send 门控）、`src/frontend_manager.cpp`（配置/Q_PROPERTY/状态文本）、`src/qml/Settings.qml`、`src/hrir_wav.{h,cpp}`（14ch WAV 校验）、`src/acoustics.h`、`third_party/freesurround_ladspa/`（三个 label：`freesurround71`、`freesurround71_auto`、`output_limiter`；`dsp_config.h` 集中 FFT/延迟/send 常数）、`tests/`（CTest）。

## 2. 历史信号链与参数（2026-10；已被 §0D–§0G 取代）

> 本节及 §4、§12–14 是 2026-10-31 的历史快照。**当前实现以 §0D–§0G、代码与测试为准**（2026-11-10 定稿：4096 FFT/128 ms、mono LFE send + 30°/15° 门控、相干 diffuse IR）。

### 输入端（虚拟 sink，2026-10-28 起是永久稳定桥）
| 项 | 值 | 说明 |
|---|---|---|
| 通道 | 8ch，FL FR FC LFE RL RR SL SR | 标准 7.1 顺序 |
| `audio.rate` | 48000（capture/playback 两端都锁） | HRIR 原生 48k，避免重采样 |
| `node.latency` | `1024/48000` | 防止低延迟应用把 quantum 拉低导致 xrun |
| 路由 | 应用通过 metadata 把所有播放流强制改道到桥 sink（`effect_input.virtual-surround-manager`，名字永不变化） | 播放流从不搬家 |

**两模块拓扑（切换配置的核心）：**
```
应用流 → [桥模块：8ch 稳定 sink + 8ch 播放流] → [效果模块：上混/14 卷积/LFE/outGain 0.30] → 默认设备
                    ↑ 只换这里（metadata 重新指向新的效果 sink）              ↑ 每次切换新建，旧的 250ms 后删
```
- **桥模块**的 sink 与播放节点名字固定（`effect_input.virtual-surround-manager` / `effect_output.virtual-surround-manager`）。它的播放节点用 `target.object` 指向当前效果模块的 sink，节点属性还带 `node.dont-fallback=true` + `node.linger=true`：目标暂时不存在时等待而不是掉到默认设备（否则会把未衰减 7.1 直出到物理设备）。
- **效果模块**的 sink/播放节点每次切换用新名字（`….effects.<generation>`）。切换流程：建新效果图 → 等新 sink/播放节点出现在 registry → 等新播放节点接到设备 → metadata 把桥的播放节点指过去 → **等桥→新 sink 的 Link 真实出现** → 250 ms 后销毁旧效果图。
- 应用流始终留在桥 sink 上，**任何包含它们的 sink 都不会在切换时被删除**，因此不会触发 WirePlumber 的 MPRIS 暂停，也不会短暂回落到物理设备（+约 10.5 dB）。

### 上混（可选，FreeSurround，label `freesurround71_auto`）
| 控制 | 值 | 出处 |
|---|---|---|
| center_image | 0.7（未使用） | 上游只保存不读；graph 已不设置（§0D） |
| shift / focus | 0.0 | 同上 |
| depth / front_sep / rear_sep | 1.0 | 同上 |
| circular_wrap | 90.0 | 同上 |
| **bass_lo / bass_hi** | **70.0 / 90.0** | 仅为插件端口兼容；所有模式 `use_lfe=0`，不做 70/90 重定向 |
| use_lfe / upmix_enable | **0.0 / 1.0** | 主声道全频过 HRIR；低频由图的 LFE 通路控制 |

**低频处理三模式（2026-10-28 用户可选；legacy 已删除）**

| 模式（配置 `lfe_mode`） | 图结构 | 听感 / 实测（相对 reference） |
|---|---|---|
| **均衡听感 `balanced`（默认）** | 仅在上混开启时：插件在其**上混分支内部**把 7 路主声道输出求和 ×0.501 写入 LFE 输出；图做 120 Hz BW4 + 当前 LFE 音色 IR（默认归一化扩散场）；主声道全频过 HRIR。上混关闭/原生多声道旁路时插件只输出原生 LFE×2，不加 send | 相位对齐后增加单声道低频密度：**AppleSpatial +3.4/+3.0/+2.5/+1.9 dB，27_95_142 +3.6/+3.1/+2.6/+2.0 dB**（40/80/100/120 Hz） |
| **自然耐听 `reference`** | 无 send；只有原生 LFE → 120 Hz BW4 → 当前 LFE 音色 IR（默认扩散场） | 最忠实、最耐听，无额外低频 |
| **低音劲感增强 `impact`** | 等同 balanced，但插件内 send 从 −6 dB 提高到 **0 dB**（+6 dB，2× 电压）；无 shelf/静态 EQ。send 门控见下（残差 >15° 退回 −6 dB，>30° 完全禁用） | **AppleSpatial +5.8/+5.3/+4.5/+3.5 dB，27_95_142 +6.1/+5.5/+4.7/+3.6 dB**；低频更密、更连续，实测无 100–120 抵消 |

- **HRIR/LFE 相位对齐（2026-10-28 引入，2026-10-30 改为每耳默认）**：120 Hz 低通在 40–120 Hz 有约 4 ms 群延迟（LR4 实测 4.36 ms；当前 BW4 的 27_95 拟合 4.29 ms），各 HRIR 的低频群延迟不同（Apple ~1.3 ms、27_95 ~0.35 ms、Atmos ~11.8 ms）。按两者的相位差拟合，在**主声道路径**（低通更慢时）或 **LFE 路径**（HRIR 更慢时）插入 `delay`：
  - 默认每耳对齐：主路径共用 `Dmain = max(0, tauL, tauR)`，LFE 每耳补 `Dmain − tau`，再各自拟合**单位幅度 all-pass**；send 门控取较差耳。27_95 默认实测 main 4.29 ms、LFE L/R 0.00 ms、残差 11.73°/11.73°；AtmosNear 的 LFE L/R 各 8.50 ms。
  - LR4 时代的共耳数值（历史参考）：AppleSpatial 2.98 ms、27_95_142 3.98 ms（延迟主声道）；all-pass 27_95 11067 Hz/Q0.143（平均 5.1°→2.2°，最坏声道 22.8°→18.3°）、Apple 10206 Hz/Q0.10（8.2°→3.7°，26.0°→18.8°）。AtmosRenderer 相位非线性，跳过。
  - 未对齐时 send 与主声道在 90 Hz 以上相位差 −95…−171°，提高 send 会抵消（旧版 balanced 在 27_95 上 100/120 Hz 为 −2.6 dB）；对齐后同一 send 变为 +2.6/+2.0 dB，impact 才有意义。
  - 代价：主路径增加约 4 ms 延迟（远小于上混的 128 ms），总延迟仍可接受。
- **Send 安全门控（2026-10-30）**：只有在**校正后平均绝对相位残差 ≤30°** 时才允许任何 LFE send；impact 的 0 dB 还要求 ≤15°。该门控主要针对 **δ/中心音色**：LFE 相位与主声道不同源时，未对齐的预设（AtmosFar/dtshx）会被禁用/退回；旧行为下 −6 dB send 在 AtmosFar 40 Hz 可抵消到 −4.6 dB、dtshx 100 Hz −3.5 dB。**默认 diffuse 与主声道使用同一组 7 对 HRIR 的平均，HRIR 相位在 LFE/主和中精确抵消（数值验证误差 ~1e−14°），只剩 BW4 低通相位（27_95 校正后约 5.8°），因此所有预置都会通过门控**（包括 AtmosFar/dtshx）。分析文本与日志会显示送量状态。
- **非 48k HRIR（2026-10-30）**：LFE 对齐/全通按 48k 图设计，HRIR 相位按文件自身采样率测量；卷积器设 `resample_quality=14`。44.1k 的 `27_95_142` 实测 `fit 3.98 ms / all-pass 11066.6 Hz Q0.143`，与 48k 一致；图正常加载。
- **预设响度匹配（可选，2026-10-30）**：`preset_level_match` 默认关闭；按中心加权 A 计权电平把当前预设移到当前列表的中位数，限幅 ±8 dB，只改整体 outGain，不改音色/LFE 比例。实测 27_95 在默认列表下 trim −1.05 dB。
- **输出保护限幅（可选，2026-10-30）**：`output_limiter` 默认关闭；LADSPA label `output_limiter`，立体声联动、5 ms lookahead、−1.5 dBFS、100 ms release；在 outGain 之后，增加 5 ms 延迟。宿主测试：latency 240、below-threshold gain 1.000、hot peak 0.841、quiet linked peak 0.0841。
- **自动切换 crossfade（2026-10-30）**：检测器等到 `kLatency + crossfade` 采样的非 L/R 内容后才切旁路，并以 20 ms 逐样本交叉淡化；返回上混同样 crossfade。旧硬切换在 1 kHz 中置测试中有 85 ms 中置空洞、0.26 采样跳变和 +5.5 dB 电平跳；现在中置连续、无 click（宿主回归）。
- **固定低频默认（2026-10-30 用户定稿）**：LFE 低通固定 **120 Hz 4 阶 Butterworth**（−3 dB @120、24 dB/oct）；**每耳 LFE 对齐默认开启**（L/R 独立 delay/allpass，主路径共用 delay；send 门控取较差耳）；**LFE 音色默认扩散场**（7 对 HRIR 平均、按中心 40–80 Hz 归一化），第二为 **δ 无染色**，第三为 **中心 HRIR**。旧的 LR4/BW8 低通、共耳选项及 `lfe_lowpass`/`lfe_per_ear_alignment` 配置键/UI 已删除；`lfe_timbre=diffuse|delta|center`，默认 `diffuse`。默认路径走 `lfe_alignment_configured()`；旧 `lfe_alignment()` 保留为参考但不再调用。27_95 默认实测：BW4/diffuse/per-ear main 4.29 ms、校正前残差 11.73/11.73° / 校正后 5.8°，60 Hz RMS **0.02755**、120 Hz **0.02469**。
- **启动状态修正**：构造函数在读取 `upmix_enabled`/`lfe_mode` 等配置后再次 `refresh_hrir_analysis()`；之前初始分析在配置读取前生成，LFE 送量一直显示“上混关闭”，直到切换预置。
- **状态显示新鲜度（2026-10-30）**：任何重建/重载先 `invalidate_hrir_analysis()` 置为“刷新中…”；分析失败/无路径显示“未获取…”，未返回的每耳值显示“—”；成功后只写入本次结果。状态行使用 `analysis.timbre`/`per_ear_values_valid`，不再直接读可能未赋值的默认字段。LFE L/R=0.00 在 diffuse 下标注“（扩散场同源，L/R 相同）”，主路径承载延迟时标注“（对齐延迟加在主路径）”，避免被误认为旧值/默认值。
- **换歌输出门控（2026-10-30）**：upmix 开启时，新流路由会 mute 效果图输出 **150 ms**（覆盖 FreeSurround 128 ms 延迟线），在音量 0 处解除后用 **100 ms** 音量淡入；首批样本即使已进入图、上一首延迟线仍有残留，也不会在输出爆出。源流 30 ms mute 只在没有输出门控（upmix 关闭/代理未就绪）时回退。实测首样本爆冲 **0.354→0**，连续音解除最大跳变 **0.135→0.037**。
- 三模式都使用按当前 HRIR 归一化的 δ（中心声道 ch6/13 的 40–80 Hz 平均幅度）；旧 `VSM_BASS_MODE` / `VSM_BASS_LEGACY` / `VSM_LFE_SEND_DB` 与 70/90 legacy 路径已全部删除。
- 插件按 FFT bin（4096 点）做升余弦幅度拆分只服务插件自身的 bass redirection；图里 `use_lfe=0` 不启用它。
- **自动检测**：仅当所有非 L/R 通道静音时上混；检测到多声道内容则旁路（延迟线补偿 128 ms，见下）。检测结果由插件导出 `vsm_auto_upmix_state`（1 立体声/0 多声道/−1 未知）和 `vsm_auto_blocks`（处理块计数，作为"是否真的有音频流动"的心跳）；应用经 dlopen/dlsym 读取。主界面仅在窗口可见时每 250 ms 轮询。
- **内置扬声器自动跳过**：识别 `device.form-factor=internal`（节点完整信息，registry global 不带该字段，需绑定 sink node）；当物理默认输出是内置扬声器时，释放已路由的流、不再把跟随默认的流路由到虚拟 sink；切回耳机/USB 时自动恢复路由。判断依据是硬件属性，不是本地化名称。限制：笔记本模拟输出把内置扬声器和耳机口做成同一个 sink（form-factor 仍为 internal），插耳机到该口也会被跳过；USB/蓝牙耳机不受影响，精确区分需读取设备当前 Route/Port。
- **默认设备保护**：桥/效果 sink 的 `priority.session=0`；若 WirePlumber 仍把任一虚拟 sink 设为默认（用户手动选择或历史记录），应用立即把 `default.audio.sink` 和 `default.configured.audio.sink` 写回上一个物理默认，避免"回退默认又回到虚拟设备"。
- **内部效果 sink 防误选**：效果 sink 描述为“虚拟环绕声（内部效果）”，与桥 “虚拟环绕声” 区分；若用户把某个应用手动指到内部效果 sink，链接监听会把它改回稳定桥（同一音频路径，但不会在换图时丢流）。
- **任意导入/新 HRIR 的自动分析**：`lfe_reference_gain()` 与 `lfe_alignment_configured()` 在每次建图时对所选 14 声道 WAV 重新计算（不是按预置写死）。主界面显示“当前 HRIR：LFE 归一化 … dB；低频对齐：每耳主 … ms、LFE L/R … ms、残差 L/R …°；相位校正：每耳全通 …”。new HRIR 加 2 ms 延迟的合成测试得到延迟从约 4.0→2.0 ms，证明计算跟随文件。send 门控：较差耳残差 ≤30° 才允许 send，≤15° 才允许 impact 0 dB；未对齐的 AtmosFar/dtshx 完全不加 send。

### LFE 通路（音色可选，默认扩散场；低频增强由模式控制）
```
[插件 fs:Out LFE]
  ├─ 上混分支：7 路主声道输出和 × (0.501 / 1.0)  ← balanced / impact
  └─ 旁路分支：原生 LFE ×2（不加 send）
        → copyLFE → 低通 120 Hz BW4（两只 biquad，Q=0.5412/1.3066）→ [每耳 all-pass 相位校正 / 每耳 delay] → 增益（见下）
        → convLFE_L/R 卷积【运行时生成的等长、按 HRIR 归一化的 LFE 音色 IR（默认扩散场 7 对平均；可选 δ / 中心 HRIR；仅 δ 为严格等量 L/R）】
        → mixL/mixR（In 8）→ outGain 0.30
主声道 HRIR 路径：copy → [共用主 delay] → 14 个卷积器 → mixL/mixR
```
| 项 | 值 | 出处 |
|---|---|---|
| 低通 | 120 Hz，4 阶 Butterworth（−3 dB @120 Hz，24 dB/oct），两只 biquad **Q=0.5412/1.3066** | 2026-10-30 用户定稿：BW4 在 120 Hz 更平（−3 dB），对齐按实际低通重算；Dolby LFE 频带上限 |
| 增益 | 上混开 → 图里 1.0；上混关 → 图里 2.0 | 上混开时原生 LFE 已在插件 bypass 分支 ×2，send 已是目标电平；上混关时只有原生 LFE，由图补 2× |
| **HRIR/LFE 对齐 delay** | 每耳 20–120 Hz 拟合；主路径共用 `Dmain = max(0, tauL, tauR)`，LFE 每耳补 `Dmain − tau`；27_95 默认 main 4.29 ms、LFE L/R 0.00 ms | 未对齐时 send 在 90–120 Hz 相位差 −95…−171°，是旧版 send 越大越糟的根因；对齐后 send 才可叠加 |
| 原生 LFE 补偿（上混开时） | auto 插件在 **bypass 分支给 LFE ×2**（2026-10-28） | 图里的 LFE 增益在上混开时必须保持 1.0（提取低音不能加 2×）；原生 7.1 被插件旁路，由插件补上 2×，等效于上混关时的 2.0 |
| **LFE send**（balanced/impact） | 插件上混分支：7 路主声道输出求和 ×（balanced 0.501 / impact 1.0）写入插件 LFE 输出；原生旁路分支不加 send；图只做 120 Hz BW4 + 当前 LFE 音色 IR | 主声道保持全频/HRIR；增加单声道低频密度；reference 无 send；原生多声道不重复低频 |
| **LFE 音色 IR 归一化**（2026-10-28；默认扩散场 2026-10-30） | 扩散场 IR = 7 对 HRIR 平均；δ / 扩散场 / 中心 IR 都按所选 HRIR 中心声道（ch6/13）40–80 Hz 平均幅度归一化 | 否则 LFE 电平随 HRIR 漂移：实测原生 LFE 在 Apple/Atmos/27_95 上分别偏热 +2.8/+8.3/+4.1 dB。BW4 下 27_95 原生 LFE 2× 在 60 Hz 为 **+6.0 dB**，比 Dolby 双耳 +5.5 dB 基准高 0.5 dB（LR4 时代为 +5.5） |
| 渲染 | **LFE 使用等长音色 IR**（默认扩散场；δ 为无染色等量 L/R，diffuse/center 会带入 HRIR 音色及少量 L/R 差异） | 专利 CN107770718A 的等量 L/R 惯例仅对 δ 严格成立；diffuse/center 是外化/音色取向，不属于严格 LFE 中性渲染 |
| **impact send** | balanced 的同一插件 send 路径，`lfe_send` 改为 1.0（balanced 0.501）；无静态 EQ | 对齐后 send 与主声道同相：实测 Apple +5.8/+5.3/+4.5/+3.5 dB、27_95 +6.1/+5.5/+4.7/+3.6 dB（40/80/100/120，相对 reference） |
| **自动相位校正** | LFE 路径 BW4 后的每耳 `bq_allpass`，每个 HRIR 自动拟合；单位幅度，只旋转相位；失败/无改善则不插入 | 默认 27_95 每耳残差 11.73°/11.73°（BW4+扩散场）；LR4 共耳时代 27_95 5.1°→2.2°、Apple 8.2°→3.7°，最坏声道仍约 18–19°（单 all-pass 无法消除声道 spread） |

- **插件 bypass 的 LFE 补偿实测**：原生 7.1（仅 LFE 40 Hz 0.25）在上混开（插件 2× + 图 1.0）与上混关（图 2.0）下输出 RMS 0.07699 vs 0.07743（差 0.05 dB；归一化前为 0.10686/0.10532）。
- δ 归一化实测（LR4/δ 时代；当前默认扩散场用同一归一化逻辑）：AppleSpatial 0.7263（−2.78 dB）、AtmosRenderer-Far 0.384（−8.32 dB）、27_95_142 0.622（−4.13 dB）（60 Hz 单点；代码用 40–80 Hz 平均）。
- **新默认声学复核（2026-10-30）**：BW4 实测 60/120 Hz 幅度 **−0.02/−3.01 dB**（LR4 −0.53/−6.02），20–120 群延迟 **4.29 ms**（LR4 4.31）；27_95 原生 LFE 2× 在 60/120 为 **+6.00/+2.99 dB**（LR4 +5.49/−0.02）。因此 +5.5 dB 双耳基准在 BW4 下实际为 +6.0（高 0.5 dB）；严格校准需整条 LFE 路径 −0.5 dB trim。扩散场 IR 在 27_95 与中心 IR 的 60 Hz 幅度差 0.00 dB、L/R 差 0.43 dB、相位差 0.6°；其他预设可差 +1.45 dB（AtmosFar）或 L/R −3.81 dB（sonic），所以 **diffuse 不是严格等量/无染色，δ 才是中性选项**。相位门控使用校正后残差（27_95 5.8°），状态行同时显示校正前 11.7°。**1 Hz 密集网格复核**：diffuse 下 `arg(LFE/主和) ≡ arg(BW4)`，与 HRIR 无关（数值误差 ~1e−14°）；per-ear 因此退化为同一主延迟 4.29 ms、LFE 0/0、L/R 全通相同，所有预置门控通过；δ/中心才保留预置相关的每耳/门控。连续网格残差：校正前 12.15° RMS / 最大 15.6°，单级全通后 5.55° RMS / 最大 10.6°；11 点拟合报 5.8° 略保守。两级全通、延迟取样优化均无实质改善（≤0.05°），单级全通已接近该结构极限。**“精确抵消”指未加权 7 路主 HRIR 和**；FreeSurround 各声道输出权重随内容变化，固定 LFE 滤波器只能对该参考（单声道/等权）精确，任意上混分布的相位是近似，5.5° 不是所有内容的保证。

### 卷积与输出
- **HRIR**：HeSuVi 14 通道 WAV，首选 48 kHz，格式 16/24/32-bit PCM 或 32-bit float（`src/assets/hrir_wav/`，含用户自有的 6 个预设，构建时默认打包）。非 48k 文件由卷积器以 `resample_quality=14` 重采样；LFE 对齐/全通仍按 48k 图计算（2026-10-30）。
- **映射**：见 §5（已彻底解决）。
- **LFE 不再经过中置 HRIR**（2026-10-27 固化）：LFE 用运行时生成的等长 LFE 音色 IR 直通（默认扩散场，可选 δ / 中心；见上表）；其余 7 个虚拟扬声器各用一对 HRIR（共 14 个卷积器）。
- **求和**：每耳 8 路卷积输出 ×1.0 进 mixL/mixR（同 HeSuVi mix.txt）。
- **输出 headroom**：outGain = 0.30（对齐 HeSuVi 响度预算，实测 FC 冲激峰值 0.5×0.475×0.30=0.07125 与实测 0.0713 吻合）。启用可选 `preset_level_match` 时乘上 ±8 dB 的整体 trim；启用可选 `output_limiter` 时在其后加 5 ms lookahead、−1.5 dBFS、100 ms release 的立体声联动限幅（默认关）。
- **playback 节点**：`node.passive=true`，2ch，48k，1024 quantum；开限幅时输出总延迟再加 240 样本。

## 3. 权威依据速查表（含链接）

| 主题 | 结论 | 来源 |
|---|---|---|
| LFE 频带 | 3–120 Hz，输出端 120 Hz LPF | Dolby / Sony / Apple Compressor 文档 |
| LFE 录音惯例 | 比主声道低 10 dB 录制，播放端补回 | UA Apollo FAQ（70mm 磁轨起源） |
| +10 dB 适用范围 | **仅原生 LFE 通道**；主声道重定向的低频不加 | UA Apollo FAQ |
| 低音管理分频点 | THX 标准 80 Hz（可调 80–120） | SVS |
| 分频滤波器 | LR（级联 Butterworth，偶数阶，同相平坦）；忌单只 Butterworth（会抵消） | dbx / JRiver / TI TIPD134 |
| AVR "LPF of LFE" | 常见默认 48 dB/oct @120 Hz | JRiver |
| sub 混入主声道缩放 | 主声道 × 1/(1+sub增益) 防过载 | Microsoft Windows 音频驱动文档 |
| 双耳渲染中的 LFE | Apple/Dolby 渲染器 LFE 距离建模固定"关"；专利惯例：LFE 不与 BRIR 卷积、等量混入、需延迟对齐 | Apple Logic 文档 / CN107770718A |
| HeSuVi 立体声行为 | 原版矩阵 `SUB=0.0`，**从不从立体声生成 LFE**；LP111+×2 只对真实 LFE 生效 | 原版 hesuvi.txt + HeSuVi.exe 内嵌矩阵 |
| FreeSurround 口碑 | 被 PCSX2 等项目采用的开源上混器（非 Dolby/DTS 认证）；库默认分频 40/90 | 网络检索 |
| ffmpeg HRTF 滤波器 | 暴露 `lfe` 增益参数，默认 0 dB | ffmpeg 文档 |
| PipeWire convolver | **0-latency 两段式 FFT 卷积**（1.4 起；filter-graph 移为独立 SPA 插件亦自 1.4） | PipeWire 1.4 release notes |
| 输出 headroom 经验法则 | 预留 ~10 dB → ×0.3；HeSuVi 社区按 Peak gain≈−0.1 dB 调 Preamp | EqAPO/HeSuVi 社区文章 |
| HRTF 相位特性 | HRTF 近似最小相位，相位误差听觉不可辨 | 近场 HRTF 最小相位研究（CNKI） |
| PipeWire 官方参考实现 | `sink-virtual-surround-7.1-hesuvi.conf`：与我们的卷积图逐行一致 | /usr/share/pipewire/filter-chain/ |
| **WirePlumber 移除 sink 会暂停播放器** | `linking.pause-playback` 默认 true：只要一个（直接或经 link-group/上游链接可达的）包含播放流的 sink 节点被移除，就通过 MPRIS 调 `Pause` | WirePlumber 0.5.18：`/usr/share/wireplumber/scripts/linking/mpris-pause.lua` + `/usr/share/wireplumber/wireplumber.conf` |
| filter-chain 模块的节点在**独立 client 连接**里注册 | 主连接 `pw_core_sync` 不保证新模块节点已出现在 registry，必须按名字等待 global | 本机 `pw-dump`：同一 app PID 出现多个 client，模块节点属于新 client |
| WirePlumber filter 链接策略 | 节点属性 `target.object`（名字）优先；`node.dont-fallback=true` + `node.linger=true` = 目标暂时缺失时等待，不回落默认设备 | `/usr/share/wireplumber/scripts/linking/find-defined-target.lua` |
| **双耳渲染的 LFE 电平** | 扬声器回放 +10 dB；**双耳渲染 +5.5 dB（先 +10，再 −4.5）**，因此双耳 LFE 不是 +10 dB；Apple Logic 的 LFE 双耳模式固定 "Off"，仍包含在双耳渲染中但不做距离建模 | Dolby 工程师转述（Midifan 2024-07-28）+ Apple Logic 文档 |
| **双耳渲染的主声道低频** | 主声道保持全频并通过 HRTF；LFE 是独立效果声道，不把主低音"搬走"；HeSuVi 立体声矩阵 `SUB=0` 也不生成 LFE | Apple/Dolby 双耳文档 + 原版 HeSuVi 矩阵 |
| **无 sub 的低音管理** | 反向低音管理：把 LFE/sub 信号混入主声道并**大幅衰减**，而不是只留一个独立 LFE | Microsoft Windows 音频驱动 bass management |
| **LFE 下混** | LFE 在 stereo downmix 中被丢弃；重要低频必须留在主声道，不能只放在 LFE | Apple Compressor 空间混合选项 / Midifan |

链接：
- Microsoft 低音管理：https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/bass-management
- Midifan 双耳 LFE +5.5 dB（Dolby 工程师转述）：https://m.midifan.com/news_body.php?id=51572
- Apple Logic 双耳渲染模式（LFE Off）：https://support.apple.com/zh-cn/guide/logicpro/lgcp789f000d/11.2/mac/14.4
- Apple Compressor 空间混合选项（LFE 下混丢弃）：https://help.apple.com/compressor/mac/4.0/cn/compressor/usermanual/chapter_12_section_7.html
- UA Apollo +10 dB：https://m.midifan.com/news_body.php?id=53378
- SVS 分频指南：https://www.svsound.com/blogs/subwoofer-setup-and-tuning/tips-for-setting-the-proper-crossover-frequency-for-a-subwoofer
- JRiver 低音管理：https://wiki.jriver.com/index.php/Room_Correction
- 专利（LFE 不卷积惯例）：https://patents.google.com/patent/CN107770718A/zh
- Apple 双耳渲染 LFE 关：https://support.apple.com/zh-cn/guide/logicpro/lgcp789f000d/11.2/mac/14.4
- HeSuVi 14ch 格式：https://sourceforge.net/p/ash-toolset/wiki/Documentation/
- Dolby LFE 定义：https://professional.dolby.com/tv/dolby-digital/

## 4. 决策史（2026-10-27 ~ 2026-10-28 会话，按顺序）

1. 用户原设想"上混 LFE → 跳过 HRIR → ×2 → 直通中置"；调查发现 **HeSuVi 也从不对 LFE 跳过卷积**（它把 ×2 LFE 预混进 CVI，卷积前求和），原设想的依据不成立 → 暂不修改。
2. 线性代数确认：HeSuVi 预混方式与本项目"专用 LFE 卷积器（同 IR）再求和"**逐样本等价**。
3. 调查原版 HeSuVi 安装包（`/home/nuofang/下载/HeSuVi_2.0.0.1.exe`，`7z x` 可解）：原版立体声默认矩阵 `SUB=0.0`——HeSuVi 不生成 LFE；挂载盘上的配置是"上混关闭"预设（非默认），且其 `matrix.txt` 的 include 被注释。
4. FreeSurround 提取参数 80/111（用户 Real3D 值）→ 40/90（库默认，试听）→ **定为 70/90**（中点对齐 THX 80 Hz）。
5. LFE 增益改为分源：上混提取 1.0 / 原生 LFE 2.0（Apollo 规则）。
6. LFE 低通 111→**120 Hz LR4**（Dolby 频带 + LR 标准 + JRiver 反抵消）；2026-10-30 用户试听后改为 **BW4**（§4-29）。
7. LFE 走中置 HRIR 保持不变（相位/延迟自动对齐，等效专业 all-pass 对齐做法）。
8. 新增 **LFE 直通模式**（δ IR 等长方案）。用户试听结论：低频密度感/力度感明显改善，其他频率无差异 → **2026-10-27 晚用户决定固化：直通成为唯一行为，旧卷积路径与开关全部删除**。
9. HRIR 通道映射：确定现有值正确（见 §5）。
10. 环路实测全部通过（见 §6）。
11. **切换 HRIR/上混开关时的"变响 + 播放器暂停"修复（两轮迭代，最终以"流静音"收尾）**：根因是旧流程先 `disable_routing` 再销毁模块，流短暂回落到物理设备（无 0.30 衰减、设备音量不同）→ 电平跳变约 +12 dB，且 sink 消失触发播放器暂停。
    - 第一轮（已被证明不足）：**重叠重建** —— 先用带世代后缀的节点名建新模块 → metadata 把已路由流改指新 sink → 延迟销毁旧模块 + 短窗内"钉回"路由。实测失败：WirePlumber 在旧 sink 消失后会把流重连到默认设备**且不写 metadata**（`schedule_module_destroy: Node N target is '(default)'` 即证据），钉回逻辑拦不住；改去静音物理设备也不可靠（设备节点查找屡次落空），实测仍有约 6 s 的双电平窗口。
    - 第二轮（**已被 2026-10-28 方案取代，相关代码已全部删除**）：切换前**逐个静音已路由的流节点**（`SPA_PARAM_Props` 的 `SPA_PROP_mute`）。流的 mute 跟着流走，无论 WirePlumber 把它重连到哪个 sink 都静音 → 从电平上遮住跳变。随后立即换图（重叠重建的代码路径保留），旧模块 1200 ms 后销毁，1800 ms 后统一解除静音（只解除我们静音过的；静音前先用 `pw_node_enum_params` 读回原状态，用户自己静音的流不动）。静音是否生效**用二次读回验证**：客户端不确认 → 视为静音失败。
    - 失败兜底（当时用户要求，**2026-10-28 已删除**）：静音失败时不直接切换，置 `switchNeedsConfirm` 弹确认框；用户取消则回滚设置。这个兜底解决不了 MPRIS 暂停，只是把风险转嫁给用户。
    - 实测代价与效果（`/tmp/vsm_test/run_silence_test.sh` + `analyze.py`，2 ch 录 `vsm_test_sink.monitor`）：三次切换各产生 **约 1.9–2.0 s 静音段**，无 3× 基线以上的电平尖峰（最大偏离 +3.2 dB，属乐曲自身动态），mpv `pause=false` 全程不变；切换后两条流仍链接在新一代图上（`wpctl status` 佐证）。**mpv 没有 MPRIS 注册，这个测试看不到用户真实播放器的暂停**——见 §4-13。
12. **播放中操作的二次确认**：GUI 关闭音效开关、托盘关闭开关、退出应用（托盘 Quit / 无托盘时关窗）在检测到正在播放时弹确认框。播放检测 = 已路由流的 node state == RUNNING（自动维护的 atomic 标志，`FrontendManager::is_audio_playing()`）。GUI 侧的"取消后开关/高亮回退"依赖 `Qt.binding()` 重建绑定——用户点击会把 `checked` 的绑定替换成普通值，只 emit 信号不会恢复显示。
    - 2026-10-28 清理：预设/上混切换不再有"静音失败确认框"（`switchNeedsConfirm` / `resolve_switch_confirm` / 托盘唤起主窗口 / 回滚函数全部删除）；只保留关闭音效与退出应用的确认。失败时的设置回退改为 setter 内直接改状态+配置并 emit，Settings 页的上混开关仍保留 `Qt.binding()` 重建（失败回退时显示旧值）。
13. **"音量突然变大然后立即暂停"的真正根因（2026-10-28）**：不是播放器自己的行为，而是 **WirePlumber 的 `linking.pause-playback`（默认开）**。旧流程销毁了应用流所在的 sink，触发两件事：① WirePlumber 把流移到默认设备（绕过图里的 0.30 衰减 → 约 +10.5 dB）；② `mpris-pause.lua` 从被删 sink 沿 link-group/上游链接找到流的 client，调 MPRIS `Pause`。上一个 LLM 用 mpv 测（mpv 默认无 MPRIS 注册）所以只看到电平问题，且其"流静音 + 2 s 窗口"方案既挡不住 MPRIS 暂停，又引入了 2 s 静音。
14. **定稿方案：稳定桥 sink + 只重建效果图**（理由：MPRIS 暂停的触发条件是"包含流的 sink 被删除"，最彻底的避免方式就是让应用可见的 sink 永不删除）。
    - 桥模块（`effect_input/effect_output.virtual-surround-manager`，无世代后缀）常驻；效果模块（`….effects.<gen>`）每次切换新建。
    - 切换：新效果图 → 等其 sink/播放节点 global → 等播放节点接设备 → metadata 写桥播放节点的 `target.object` → 等桥→新 sink 的 Link 出现 → 250 ms 后销毁旧图。
    - 桥播放节点带 `node.dont-fallback=true` + `node.linger=true`；WirePlumber 重启丢 metadata 时节点属性里的旧目标名会"等"而不是回落物理设备，应用在 metadata 重新出现时把桥指回当前效果 sink。
    - 应用流从头到尾留在桥 sink；切换期间唯一变化的内部 Link 是桥播放节点→效果 sink。实测 27–44 ms 完成。
15. **24-bit HRIR 的 δ IR 头 bug（2026-10-28 修复）**：`write_lfe_bypass_delta` 以前无论源格式都写 `wFormatTag=3 (IEEE float)`。AppleSpatial/atmos 是 32-bit float，碰巧正确；`AtmosRenderer-*.wav` 是 24-bit PCM，生成的 δ 文件"头是 float、数据是 int24" → 卷积器加载失败，PipeWire 报 `can't start graph: No such file or directory`，整张效果图静音（直到下一次切换）。现在 δ 文件的 tag/位深/满刻度与源文件一致（PCM 16/24/32 int 或 float32），并支持 WAVE_FORMAT_EXTENSIBLE 的子格式。
16. **`disable_routing` 自回声 bug（2026-10-28 修复）**：清 target 后，应用自己的 metadata 回调看到"target 被移除"就把流 **重新路由回桥 sink**（EasyEffects 兼容逻辑）。结果流从未离开桥，500 ms 等待超时后销毁 sink → 正常退出也触发 MPRIS Pause（实测 SIGTERM 后 524 ms 收到 Pause）。修复：写清除前先把流从 `routed_node_ids` 摘掉，自回声不再命中重路由分支。
17. **验收实测（隔离 PipeWire/WirePlumber/D-Bus + null sink + 假 MPRIS 播放器，mpv 作为其子进程）**：6 次切换（预置/上混交替，含 24-bit 预置）0 个 >3× 基线的窗、0 个 ≥20 ms 静音段（上混开启的延迟线灌注被旧 HRIR 长尾覆盖，仅出现 <20 ms 的瞬时下陷）、MPRIS **0 次 Pause**、mpv `pause=false` 全程；正常 SIGTERM 退出 0 次 Pause；对照 `kill -9` 强杀应用（sink 在流仍链接时消失）**10 ms 内收到 Pause**，证明测试能捕获用户描述的症状。
18. **重启后流的过期 target（2026-10-28 实机发现）**：WirePlumber 会按流状态恢复上一次的 `target.object`（通常是旧 sink 的 **object.serial**，如 17494）。旧实例的 sink 已不存在时，新实例的 `resolve_sink_name()` 解析为空，原逻辑把它当成"已分配给别的设备"而永久放弃接管——实测重启后 Edge 停在物理设备直出，必须手动切回。现在把**无法解析的 target 视为跟随默认设备**并路由到桥（与"旧世代虚拟 sink 名字前缀"同一语义）；实机重启后 Edge 自动回到 `effect_input.virtual-surround-manager`。代价：若目标设备只是尚未出现在 registry，可能被提前接管；按本产品的虚拟化语义接受。
19. **auto 插件内容切换的 128 ms / 75 ms 空洞（2026-10-28 修复）**：宿主机回归测试发现，上混→旁路时旁路延迟环从未在上混期间写入，切换后输出 12 个 512 块（6144 样本≈128 ms）全零；旁路→上混时核心被 reset，输出 FIFO 重新灌注，约 7 个块（≈75 ms）全零；另外旁路 LFE 仍是 1×。修复：**两种模式都持续写旁路延迟环**，旁路期间**持续 feed 上混核心并 discard 输出**，不再 reset；旁路 LFE ×2。宿主测试现在断言切换块不为零、原生 LFE 比例 2.0、FC 比例 1.0（`OK (0 failures)`）。
20. **非 14 通道 HRIR（2026-10-28 修复）**：`write_lfe_bypass_delta` 现在拒绝 `channels != 14`；7 通道 `oal_dflt.wav` 在启动/切换时都会报 "has 7 channels, expected 14"，旧图保留、设置回滚，不再生成空间像错误的图。实测启动被拒绝时无任何 `virtual-surround` 节点。
21. **短 IR + 上混 OFF→ON 的剩余缺口（2026-10-28 实测）**：用只含 `27_95_142.wav`（218 帧≈4.5 ms）的测试列表强制先翻上混，上混 OFF→ON 出现 **90 ms 静音**（6.43–6.52 s）。原因：旧图是上混关、没有 FreeSurround 128 ms 延迟线可覆盖新图的灌注；长 HeSuVi IR（~1 s）的尾巴能覆盖，所以常规预设不受影响。这是图重建的固有限制，未用交叉淡化（会引入 128 ms 时间错位/回声）；文档如实记录，用户的短 IR 下翻上混会听到约 90 ms 空档。

22. **LFE 处理三模式 + HRIR/LFE 相位对齐（2026-10-28 重构）**：`balanced`（默认，reference + 上混时 −6 dB 主声道低频 send）、`reference`（仅原生 LFE）、`impact`（同一 send 提到 0 dB）。设计依据：
    - 120 Hz LR4 在 40–120 Hz 的群延迟约 4.4 ms；各 HRIR 的低频群延迟不同（Apple ~1.3 ms、27_95 ~0.35 ms、Atmos ~11.8 ms）。旧版 send 与主声道在 90 Hz 以上相位差 −95…−171°，因此 send 越大抵消越多（27_95 的 100/120 Hz 由 +2.7 变为 −2.6 dB）。
    - 现在按 HRIR 全部 14 通道的低频相位与 LR4 的差拟合一个 delay：LR4 更慢时延迟主声道路径，HRIR 更慢时延迟 LFE 路径；相位不线性（AtmosRenderer 残差 44°）则跳过。20–120 Hz 等权拟合实测 Apple 2.98 ms、27_95 3.98 ms（主路径延迟），随后自动 all-pass 校正平均残差。
    - 对齐后 balanced 实测 Apple +3.4/+3.0/+2.5/+1.9 dB、27_95 +3.6/+3.1/+2.6/+2.0 dB；impact（0 dB send）Apple +5.8/+5.3/+4.5/+3.5 dB、27_95 +6.1/+5.5/+4.7/+3.6 dB（40/80/100/120）。impact 不再使用 +4 dB/100 Hz 低架（用户反馈“软糊、恶心、没有劲感”），静态 EQ 已删除。
    - **send 位置（2026-10-28 修正）**：send 从图里的 `mixBass/sendGain/mixLFE` 移到 **auto 插件的 `lfe_send` 控制**，只在上混分支写入插件的 LFE 输出；旁路分支只输出原生 LFE×2。旧图结构会在原生 5.1/7.1（插件旁路）时仍把主声道和混进 LFE，等于重复低频；修正后 FC-only 与 LFE-only 的 upmix on/off 输出差均 <0.1%。
    - 所有模式 `use_lfe=0`；旧 70/90 重定向、`VSM_BASS_MODE`、`VSM_BASS_LEGACY`、`VSM_LFE_SEND_DB` 全部删除。配置键 `lfe_mode`（balanced/reference/impact），非法值回退 balanced。
    - 切换走稳定桥+效果图替换路径；模式切换实测（隔离会话，播放中）0 个 >3× 尖峰、0 个 ≥20 ms 静音、0 次 MPRIS Pause（仅上混 OFF→ON 自身仍有约 70 ms 下陷，见 §4-21）。
23. **主界面实时输入检测（2026-10-28 修复）**：auto 插件导出 `vsm_auto_upmix_state`（1 立体声/0 多声道/−1 未知）和 `vsm_auto_blocks`（处理块计数心跳）；应用 dlopen/dlsym 读取。之前只靠 `is_audio_playing()`（PipeWire 节点状态）不可靠，且 QML 只在窗口 `active` 时轮询，导致一直显示“等待音频播放…”——现改为：
    - 用块计数是否在最近 750 ms 内增长判断“真的有音频流动”；
    - QML 仅在窗口 `visible` 时每 250 ms 轮询（不要求焦点，隐藏到托盘时停止）；
    - 文案：已关闭 / 输出为内置扬声器，已自动跳过处理 / 上混关闭（直通）/ 等待音频播放 / 检测到立体声，上混已生效 / 检测到多声道，自动直通 / 正在检测输入。
24. **默认设备保护（2026-10-28）**：桥/效果 sink 设置 `priority.session=0`；同时监听 `default.configured.audio.sink` 记住用户上一次的物理偏好。若 WirePlumber 仍把虚拟 sink 设为默认（手动选择或历史恢复），应用把 `default.audio.sink` 和 `default.configured.audio.sink` 写回该物理默认（无偏好时按 `priority.session`/硬件名选择），避免“回退默认又回到虚拟设备”。隔离实测：手动把桥或内部效果 sink 设为默认后，1 s 内恢复为物理 `vsm_test_sink`；实机同样恢复到 USB DAC。
25. **内部效果 sink 防误选（2026-10-28）**：效果 sink 描述改为“虚拟环绕声（内部效果）”，桥为“虚拟环绕声”，不再出现两个同名设备；若应用被手动指到内部效果 sink（或旧世代效果 sink、其 serial），链接监听/`decide_stream` 会把它改回稳定桥；同一路径、切换时不会丢流。已实测 `pactl move-sink-input` 到效果 sink 后被改回桥（重定向节流 1.5 s，避免 link 事件抖动刷写）。
26. **内置扬声器自动跳过（2026-10-28）**：通过绑定 sink 节点读取完整 `device.form-factor`（registry global 不携带该字段），`internal` 视为内置扬声器。物理默认是内置扬声器时释放已路由流、不再路由跟随默认的新流；切回 USB/耳机时 `route_default_streams()` 自动恢复。隔离测试用 `device.form-factor=internal` 的 null sink 验证：播放器留在 internal_test，检测文案显示“输出为内置扬声器，已自动跳过处理”；切回普通 sink 后自动路由到桥。
27. **切换时旧效果图销毁的 MPRIS 暂停修复（2026-10-28）**：图创建变慢（~0.7–1.0 s）后，旧的 250 ms 销毁延迟不足以让 WirePlumber 的 `links_in` 记账更新；`mpris-pause.lua` 仍认为旧效果 sink 连着桥并暂停播放器。现在先等旧 sink 的输入 Link 消失，再延迟 1200 ms 销毁，并在销毁前复查；仍有 link 则重试。实测 6 次播放中切换 0 次 MPRIS Pause、0 个 >3× 尖峰、0 个 ≥20 ms 静音（OFF→ON 仍有约 70 ms 下陷）。
28. **2026-10-30 音质/兼容性批量优化**（用户要求“所有合理优化，包括可选项”）：
    - **Send 安全门控**：30° 允许任何 send，15° 允许 impact 0 dB；未对齐的 AtmosFar/dtshx 由“−6 dB 反而抵消”改为完全不加 send；分析文本/日志显示。
    - **自动切换 wait+crossfade**：插件等到 kLatency+crossfade 采样的非 L/R 内容后才切旁路（旧 2-block 决定的 85 ms 中置空洞/0.26 跳变/+5.5 dB 跳变），20 ms 逐样本混合；返回上混同样 crossfade；宿主回归加入中置连续性、steady block step、双向 click 与 limiter 测试。
    - **非 48k**：`lfe_alignment_configured` 分离文件率/图率（低通/全通/延时按 48k，HRIR 相位按文件率），卷积器 `resample_quality=14`；44.1k 27_95 实测与 48k 对齐结果一致，图正常。
    - **可选预设响度匹配**（默认关）：中心加权 A 计权能量，当前列表**中位**参考，±8 dB，只改 outGain；27_95 默认列表 trim −1.05 dB。
    - **可选输出限幅**（默认关）：新 LADSPA `output_limiter`，立体声联动、5 ms lookahead、−1.5 dBFS、100 ms release，outGain 之后；宿主测试 240 样本延迟、below-threshold 1.000、hot 0.841、quiet linked 0.0841。
    - **UI/配置**：`preset_level_match`、`output_limiter` 两个开关；LFE 模式描述写明门控/退回；分析行新增送量状态、源采样率、响度 trim、限幅状态。
    - **文档**：README/插件 README 增加 AVR 低频管理对照；本文件新增 §14。
    - **验收**：宿主 `OK (0 failures)`；隔离会话 27_95 balanced 440 Hz 端到端 RMS 0.01802（限幅开/关一致）；AtmosFar 日志 `LFE send disabled (249°)`；44.1k 图 `source 44100 / graph 48000` 正常；系统 LADSPA 已备份旧版并安装新版。
29. **2026-10-30 低频三轴实验 → 同日用户定稿**：
    - **定稿默认**：低通固定 **120 Hz 4 阶 Butterworth**（−3 dB @120、24 dB/oct）；**每耳 LFE 对齐默认开启**（L/R 独立 delay/allpass、主路径共用 delay、send 门控取较差耳）；**LFE 音色默认扩散场**（7 对 HRIR 平均，按中心 40–80 Hz 归一化），第二 δ 无染色，第三中心 HRIR。
    - **删除**：`lfe_lowpass` 与 `lfe_per_ear_alignment` 配置键及 UI；旧 LR4/BW8 路径不再调用（`lfe_alignment()` 保留为参考，`[[maybe_unused]]`）。仅保留 `lfe_timbre=diffuse|delta|center`，默认 `diffuse`，非法值回退 `diffuse`。
    - 默认路径统一为 `lfe_alignment_configured()`（BW4/diffuse/per-ear）；分析行显示“低频设置：Butterworth 4 阶 / <音色> / 每耳”及每耳 delay/残差。用户明确要求“移除实验并改为默认”，该函数现为唯一生产路径。
    - 实测：共耳 BW4 fit 3.96 ms / 11.26° 平均；BW8 8.08 ms / 24.50°；center 4.19 ms/5.17°；diffuse 4.31 ms/5.50°；per-ear 27_95 main 3.98 / LFE 0 / 4.81/5.31°；默认 BW4+diffuse+per-ear（48k）main 4.29 ms、11.73/11.73°，60 Hz RMS **0.02755**、120 Hz **0.02469**（44.1k q14 的对齐数值同为 4.29 ms/11.73°）；AtmosNear LFE 8.50/8.50 ms；AtmosFar 251°→send disabled（**这些 8.50/251° 是 δ/中心实验数据；默认 diffuse 下 HRIR 相位精确抵消，所有预置 main 4.29 ms、LFE 0/0、校正后约 5.8°，AtmosFar 不再被禁用**）。选型期其他组合 60 Hz：LR4 0.02704、BW8 0.02749、center/diffuse 0.02704；120 Hz：LR4 0.02292、BW4 0.02470、BW8 0.02398。
    - **启动状态 bug**：构造函数原先在读取配置前就调用 `refresh_hrir_analysis()`，分析文本按默认 `upmix=false` 生成，LFE 送量一直显示“上混关闭”，直到切换预置；现在读完全部配置后再调用一次，启动即显示正确送量。
    - **换歌爆音（2026-10-30 二次修复）**：源流 30 ms mute 太晚——实测首 5 ms 爆冲在约 128 ms 的输出处完整出现（首批样本已进入 FreeSurround 延迟线）。改为**效果图输出门控**：新流路由时 mute 效果图输出 150 ms，在音量 0 处解除后用 100 ms 音量淡入；首批样本/上一首延迟线残留都在输出端被截掉。源流 30 ms mute 仅作为 upmix 关闭或代理未就绪时的回退。隔离测试：首样本爆冲峰值 0.354→0；连续音解除最大跳变 0.135→0.037。待用户重启后实机确认。
    - 工作树未提交。

## 5. HRIR 通道映射（已解决，勿再猜）

**定义来源**：原版 HeSuVi 2.0.0.1 `hesuvi.txt`：
```
Channel: L0 R1 SL0 SR1 RL0 RR1 C0 R0 L1 SR0 SL1 RR0 RL1 C1
Copy:    L0=LVI R1=LVI SL0=SLVI SR1=SLVI RL0=RLVI RR1=RLVI C0=CVI
         R0=RVI L1=RVI SR0=SRVI SL1=SRVI RR0=RRVI RL1=RRVI C1=CVI
```
命名 = `[族前缀][耳][槽位]`。最终映射（代码 `create_virtual_surround_module`）：

| ch | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 扬声器 | FL | FL | SL | SL | RL | RL | C | FR | FR | SR | SR | RR | RR | C |
| 耳 | L | R | L | R | L | R | L | R | L | R | L | R | L | R |
| 节点 | FL_L | FL_R | SL_L | SL_R | RL_L | RL_R | FC_L | FR_R | FR_L | SR_R | SR_L | RR_R | RR_L | FC_R |

7–12 的"乱序"是因为 HeSuVi 第二块交错排列右侧扬声器的右耳 IR。

**第三方佐证**：PipeWire 官方示例 `/usr/share/pipewire/filter-chain/sink-virtual-surround-7.1-hesuvi.conf`（我们的图正源于它）使用的 14 通道分配与上表**逐行一致**（含 LFE 用通道 6/13、mix 1.0 系数）。三条独立证据链（HeSuVi 定义 / IR 数据 ITD / PipeWire 官方配置）互相印证。

**验证方法（不要用录音！）**：直接对 IR 文件做通道对互相关。物理规律：近耳领先（前 ~0.2 ms、后 ~0.3 ms、侧 ~0.5–0.7 ms）且响 6–12 dB；中置对零 ITD 等响度。全部 9 个真实预设通过；Apple/Atmos 合成预设有能量不对称但 ITD=0。

**血的教训（录音工具会骗人，本项目已两次踩坑）**：
- `pw-record --target <sink>` / `parec --device=<sink>.monitor` 匹配失败时会**静默回退到麦克风**；`pw-record` 无法按名字连接 monitor（需用 `parec`，或 pulse monitor 名）。
- 手写 WAV 头必须与数据编码一致；声明 PCM 却装 float32 会被解码成 7 倍电平 + 失真的垃圾（本项目实测 0.1 正弦变成 0.48）。测试信号建议直接写 16-bit PCM（WAVEFORMATEXTENSIBLE，subformat 0x0001，mask 0x63F）。
- 交叉验证：任何"通道路由"结论必须能在文件域或同一次录音内的相对时序上复现，不能依赖跨次录音的绝对时间。

**PipeWire 原生 API 的血泪坑（2026-10-27 排查静音功能时踩到，别再踩）**：
- 构造 Props pod 时值类型要传**字符串标签**：`SPA_POD_Prop(SPA_PROP_mute, SPA_POD_Bool(mute))`（`SPA_POD_Bool(v)` 展开为 `"b", v`）。写成 `SPA_TYPE_Bool, mute ? 1 : 0` 会让 vararg 解析器把整数当 `const char*` 读 → **SIGSEGV**（`spa_pod_builder_add` 内）。本项目曾因这个 bug 崩溃三次、并被我误判成"客户端不支持 mute"。
- `spa_node_events.param` 回调里 `param` **可能为 NULL**（表示该 param 被移除），`SPA_POD_TYPE` 也**可能不是 Object**；直接 `reinterpret_cast<spa_pod_object*>` 后遍历会崩。先判空 + 判 `SPA_TYPE_Object`。
- **param id 是 `enum spa_param_type` 的小整数**：`SPA_PARAM_Props == 2`（0.28.0 起就是顺序枚举），**不是** fourcc。`id=2` 就是 Props 回复，`next=1` 表示"还要继续枚举 PropInfo"，只有 `next == SPA_ID_INVALID` 才是结束。
- 回复**不携带我们请求时的 seq**（服务端用自己的同步号），所以"哪个回复对应哪个请求"不能靠 seq 匹配 → 改成**一次只允许一个在途读**（`awaiting` 标志），读回后再等 `pw_thread_loop_timed_wait_full`（它会临时释放锁并在本线程派发事件，`sync_with_core` 同理有效）。
- mute 能力实测：pipewire-pulse 的 sink-input 与原生 pw_stream（mpv `--ao=pulse` / `--ao=pipewire`）**都支持** `SPA_PARAM_Props` 的读与写；`wpctl set-mute <node-id> 1` 可作旁证（录到 RMS 从 0.0111 → 0.0000）。
- `pw-dump` 的参数在 `info.params`（按名字典），**不是**顶层 `params` 数组；按顶层 key 解析会得到"全部为空"的假结论。
- 应用自身的 Qt 日志走 **journald**（stdout 常为空，别以为程序没输出）：用 `journalctl _PID=<pid>` 取；`QT_LOGGING_RULES='*.debug=true'` 会连带灌入 harfbuzz 噪声，临时调试用 `qWarning` 更省事。
- 进程名截断到 15 字符（`virtual-surroun`）：清场用 `pkill -x virtual-surroun` / `pkill -x mpv`。**绝对不要**在同一行命令里既写 `pgrep -f <二进制路径>` 又跑它——模式会匹配到自己的 shell 并把自己杀掉（本会话栽过 3 次，AGENTS.md §9 的重启方法同理慎用 `pgrep -f`）。

**2026-10-28 切换架构踩坑（重要，别再踩）：**
- **filter-chain 模块的节点在独立 client 连接里注册**：应用自己只有一个 `pw_context`/主连接，但每次 `pw_context_load_module("libpipewire-module-filter-chain")` 的节点属于一个新 client（`pw-dump` 里同一 PID 多个 client）。所以模块加载后的 `pw_core_sync` **不保证**新 sink/播放节点的 global 已经到达 → 必须按名字/ID 等 registry global（本项目的 `wait_for()`），否则第一次切换可能碰巧成功、后续随机失败（"New effects sink was not linked in time"）。
- **`target.object` 对 filter-chain 播放节点有效**：WirePlumber 的 `find-defined-target` 先读节点属性、再读 metadata（metadata 覆盖属性）；值是名字时按 `node.name` 匹配。桥播放节点就是这样被指到新效果 sink 的。
- **`node.dont-fallback=true` 且目标不存在时会销毁节点**（除非 `node.linger=true`）。桥播放节点两个都设：目标暂时缺失时 WirePlumber 只等待，不会把未衰减的 8ch 直出到默认设备；metadata 重新出现后应用会重写 target。
- **MPRIS 暂停的遍历不只看直接链接**：`mpris-pause.lua` 会沿 `links_in`（上游）和 `node.link-group` 找到流所属 client。因此判断"删这个 sink 会不会暂停"不能只看谁直接链着它，要看整个上游链。稳定桥方案让应用流的上游永远不经过被删的效果 sink。
- **删除 sink 前等流真正离开**：`disable_routing` 先写 target 清除，再等"桥 sink 的输入 Link 消失"（registry Link 跟踪）。**写清除前必须先把流从 `routed_node_ids` 摘掉**，否则应用自己的 metadata 回调会把它重新路由回桥（§4-16）。
- **重启后的 `target.object` 可能是旧 serial**：WirePlumber 的 stream restore 会恢复上次的 target serial；解析不到任何当前节点时必须视为"跟随默认"，不能当成"已分配给别的设备"（§4-18）。
- **旧效果图延迟 250 ms 销毁**：WirePlumber 的 link 记录更新是异步的；新 Link 已出现后立刻删旧 sink，仍可能被 mpris-pause 的记录判成"删除时还连着"。250 ms 足够，且旧图此时已无输入，只是把 IR 尾巴放完。
- **手写 δ IR 必须匹配源编码**：`write_lfe_bypass_delta` 的 WAV 头 tag/位深必须与 HRIR 的 `fmt` 一致；24-bit PCM 写成 float 头会让整张图 `can't start graph`（§4-15）。WAVE_FORMAT_EXTENSIBLE 要从扩展区取真正的 subformat（offset 24 的 2 字节），不能只看 `0xFFFE`。

**非 14 通道 HRIR**：`oal_dflt.wav`（7 通道）选中后**不会报错**（PipeWire 容忍越界通道号），但空间像错误。README 已注明；可选改进：加载列表过滤非 14ch 文件。

## 6. 实测验证结果（2026-10-27 / 2026-10-28，隔离 PipeWire 会话）

2026-10-27：`module-null-sink` 隔离 → 8ch 测试 WAV → `pw-play → 虚拟sink → 链路 → null sink` → `parec` 录 monitor → numpy。三组配置：上混 OFF/直通 ON、OFF/OFF、ON/ON。

2026-10-28（切换架构验收）：独立的 PipeWire + WirePlumber + pipewire-pulse + D-Bus 会话（`XDG_RUNTIME_DIR=/tmp/vsm_pw/run`），null sink 做默认输出，假 MPRIS 播放器（Python dbus，mpv 作为其子进程）→ 应用自动切换（`VSM_TEST_AUTO_SWITCH_MS=4000`）→ `parec` 录 null sink monitor → numpy；6 次切换覆盖 32-bit/24-bit HRIR、上混开/关。

| 项 | 预期 | 实测 | 结论 |
|---|---|---|---|
| LR4@120 相对响应（60→120/240） | −5.49 / −24.08 dB | −5.50 / −24.09 dB | ✅ |
| 60 Hz 绝对电平 | 0.0399 RMS | 0.03992 | ✅ |
| 分源增益（ON vs OFF） | −6.02 dB | −6.02 dB | ✅ |
| 直通 L/R 等量 | 逐样本相同 | maxdiff 0.00 | ✅ |
| 对齐（δ vs HRIR 路径互相关） | 0 样本 | −1 样本（−0.02 ms） | ✅ |
| FC 路径确定性（A/B 两次） | 0 样本 | 0 | ✅ |
| Headroom 0.30 | 0.07125 峰值 | 0.0713 | ✅ |
| 插件冒烟测试（`BUILD_LADSPA_TEST_HOST=ON`） | 440Hz→FC、60Hz→LFE | 0.66 / 0.71 | ✅ |
| HRIR 在 60 Hz 相对 δ 的衰减 | — | −2.84 dB（用户"密度感"来源） | 数据 |
| 24-bit HRIR（AtmosRenderer）启动效果图 | 无 `can't start graph` | 修复前每次切换报 ENOENT、整图静音；修复后 **0 次** | ✅ §4-15 |
| 切换耗时（桥→新效果 sink 的 Link 确认，含新图创建） | 越快越好；旧图在无输入后延迟 1.2 s 销毁 | 本次实测 **646–1032 ms**（新图含 delay/14 卷积器，registry 等待占主要时间）；音频切换本身无长静音 | ✅ §4-27 |
| 切换电平尖峰（10 ms 窗，null sink monitor） | 0 个 >3× 基线 | **0 个**；最大 1.85× 基线（乐曲/预置自身动态） | ✅ |
| 切换静音段（10 ms 窗，RMS <0.05× 基线） | 越短越好 | **0 个 ≥20 ms**；上混开/关切换有 <20 ms 的瞬时下陷（旧 HRIR 1 s 长尾/旧 FreeSurround 延迟线覆盖新图 128 ms 灌注） | ✅ |
| 短 IR（`27_95_142.wav`，218 帧≈4.5 ms）切换 | 无长静音、无 >3× 尖峰 | 预置 ON→ON 无 ≥20 ms 静音（仅 10 ms 级下陷）；**ON→OFF 有约 128 ms 的旧延迟线重叠**，峰值 1.5–2.3× 基线（相位相关，<3×）；**上混 OFF→ON（旧图也是短 IR）有 90 ms 静音**（§4-21） | ✅/已知残留 |
| auto 插件宿主回归（`freesurround_ladspa_test`） | 0 failures | 440 Hz→FC 0.660、60 Hz→LFE 0.719；切换块无零；原生 LFE 2.0、FC 1.0；upmix LFE send = 0.5 × 七路主输出和 | ✅ §4-19/§4-22 |
| 原生 7.1 LFE 集成（上混开 vs 关） | 电平一致 | 归一化后 RMS 0.07699 vs 0.07743（0.05 dB）；归一化前 0.10686/0.10532 | ✅ §4-19/§2 |
| 原生 FC-only 8ch（upmix on/off） | 一致，不应出现 send | RMS **0.0130723 vs 0.0130775**（0.04%），峰值逐样本相同 | ✅ §13.5 |
| 原生 LFE-only 8ch（WAVEFORMATEXTENSIBLE，mask 0x63F，upmix on/off） | 一致 | RMS **0.064609 vs 0.064661**（0.08%），峰值相同 | ✅ §13.5 |
| 任意 HRIR 的自动分析/新文件 | 跟随文件 | `27_95_142` 3.98 ms/−4.15 dB；复制并前置 96 样本（2 ms）后约变为 2.0 ms | ✅ §13.4/§13.8 |
| δ 归一化增益（中心 HRIR 40–80 Hz） | 与 HRIR 低频一致 | AppleSpatial 0.7263（−2.78 dB）；AtmosRenderer 0.384（−8.32 dB）；27_95_142 0.622（−4.13 dB） | ✅ §12 |
| LFE 三模式频响（AppleSpatial，40/80/100/120 Hz 多音） | 与设计一致（相位对齐后） | **balanced +3.36/+3.01/+2.51/+1.88 dB；impact +5.79/+5.27/+4.54/+3.48 dB**（相对 reference；reference RMS 0.01758） | ✅ §4-22 |
| LFE 三模式频响（27_95_142，用户当前预置） | 与设计一致；27_95 对齐 3.98 ms + all-pass | balanced **+3.57/+3.18/+2.73/+2.07 dB**；impact **+6.10/+5.46/+4.72/+3.67 dB**（40/80/100/120；reference RMS 0.01449） | ✅ §2/§4-22 |
| LFE 模式切换（播放中） | 无新增停顿/尖峰/暂停 | 与预置/上混同路径；录音 0 个 ≥20 ms 静音段、0 个 >3× 基线窗、MPRIS 0 Pause（本次隔离会话；OFF→ON 自身的约 70 ms 下陷除外） | ✅ §4-22/27 |
| 实时输入检测 | 立体声/多声道文案正确 | 8ch 全通道信号 → “检测到多声道，自动直通”；切回立体声 → “检测到立体声，上混已生效”；块计数心跳驱动，窗口可见即轮询 | ✅ §4-23 |
| 手动把虚拟 sink 设为默认 | 恢复物理默认 | 桥/效果 sink 均被 `pactl set-default-sink` 后 1 s 内回到 `vsm_test_sink` | ✅ §4-24 |
| 手动把应用移到内部效果 sink | 改回稳定桥 | `pactl move-sink-input` 后链接监听重定向，最终 sink 为桥；重定向节流 1.5 s | ✅ §4-25 |
| 内置扬声器跳过 | 不路由、检测提示 | `device.form-factor=internal` 的 null sink 为默认时：流留在 internal_test，文案“输出为内置扬声器，已自动跳过处理”；切回普通 sink 后自动路由到桥 | ✅ §4-26 |
| 切换时 MPRIS 暂停 | 0 次 | 6 次播放中切换 **0 次 Pause**（旧图延迟 1.2 s 销毁后）；mpv `pause=false` 全程 | ✅ §4-27 |
| 正常退出（SIGTERM）时 MPRIS 暂停 | 0 次 | 等待流离开桥 1500 ms，播放中退出再给 WirePlumber 1.2 s 结算；最终隔离复测 0 次 Pause（对照 kill -9 仍在 ~300 ms 内 Pause） | ✅ §4-16/§4-27 |
| 重启后过期 target 的接管（实机 Edge） | 自动回到桥 sink | 修复前 `left alone`（物理直出）；修复后 `routed to effect_input.virtual-surround-manager` | ✅ §4-18 |
| 对照：`kill -9` 应用（sink 在流仍链接时消失） | 必须触发 Pause | **10 ms 内收到 Pause** | ✅ 测试有效性 |
| 旧静音方案（已被取代） | — | 3 次切换各约 1.9–2.0 s 静音；mpv 无 MPRIS 所以没测出真实暂停 | ❌ |

**2026-10-30 新增验收（宿主 + 隔离 PipeWire）：**

> 下表的“实验”行是定稿前的组合验证；用户随后把 **BW4 + 扩散场 + 每耳** 定为唯一默认（§4-29）。这些行保留为选型数据，不代表仍有可选开关。

| 项 | 预期 | 实测 | 结论 |
|---|---|---|---|
| send 门控 | 未对齐预设不加 send | AtmosFar 249° → `LFE send disabled`；27_95 仍 `bass send` | ✅ §4-28 |
| auto 切换中置连续性（1 kHz 居中 + FC） | 中置不中断 | min FC block RMS 0.0627（旧版 0）；无 click | ✅ |
| auto 切换 click | 无硬切换跳变 | upmix→bypass 0.0132、bypass→upmix 0.0076（阈值 0.02；旧 0.26） | ✅ |
| auto 稳态 block step | 无解码器边界跳变 | 440 Hz 稳态 FL 0.00030、FC 0.00760 | ✅ |
| auto 原生 LFE bypass | 2.0 / 1.0 | 1.99309 / 1.00000 | ✅ |
| 输出限幅宿主 | latency 240、below 1、hot ≤0.85 | 240 / 1.00000 / 0.84140；quiet linked 0.08414 | ✅ |
| 44.1k HRIR 图 | 正常加载、对齐按 48k | `source 44100 / graph 48000`，fit 3.98 ms、allpass 11066.6 Hz Q0.143 | ✅ |
| 44.1k δ 文件 | 与源同率 | 44.1 kHz mono float，卷积器 q14 重采样 | ✅ |
| 预设响度匹配 | 27_95 默认列表 trim 约 −1 dB | −1.05 dB，outGain 0.2659 | ✅ |
| 端到端 440 Hz（27_95 balanced） | 有声、限幅开/关一致 | RMS 0.018020、peak 0.027624（两态相同） | ✅ |
| 44.1k 图加载 | 正常 | 图加载成功（44.1k 文件）；音频通路未单独录制 | ✅ 图/待音频复测 |
| 系统 LADSPA | 新 label 可用 | 旧版备份 `/tmp/freesurround_ladspa.old.so`；新版安装到 `/usr/lib/ladspa`、`/usr/lib64/ladspa` | ✅ |
| 实验 BW4 | 图加载、对齐 | fit 3.96 ms / 平均 11.26° / worst 28.58°（+all-pass）；60 Hz RMS 0.02704、120 Hz 0.02470 | ✅ |
| 实验 BW8 | 图加载、对齐 | fit 8.08 ms / 平均 24.50° / worst 41.11°（+all-pass）；60 Hz 0.02749、120 Hz 0.02398 | ✅ |
| 实验 center IR | 图加载、电平 | fit 4.19 ms / 5.17°；60 Hz RMS 0.02704 | ✅ |
| 实验 diffuse IR | 图加载、电平 | fit 4.31 ms / 5.50°；60 Hz RMS 0.02704（L/R 0.02729/0.02679） | ✅ |
| 实验 per-ear（27_95） | L/R 独立、图加载 | main 3.98 ms、LFE L/R 0、残差 4.81/5.31°；60 Hz RMS 0.02704 | ✅ |
| 实验 per-ear（AtmosNear，δ/center） | 非零 LFE delay 图 | main 0、LFE L/R 8.50 ms、8.33°；图加载 | ✅ |
| 实验 per-ear（AtmosFar，δ/center） | send 门控 | 251.19/247.17° → `LFE send disabled` | ✅ |
| 默认 diffuse 相位自对齐 | 所有预置相同 | `arg(LFE/主和)≡arg(BW4)`（1e−14°）；main 4.29 ms、LFE 0/0；校正后 5.5° RMS / 10.6° max；门控全通过 | ✅ §2/§4-29 |
| 实验组合 BW4+center+per-ear | 图加载 | main 4.17 ms、11.32/11.46° | ✅ |
| 实验组合 BW8+diffuse+per-ear | 图加载 | main 8.44 ms、25.45/25.45° | ✅ |
| 44.1k BW4+diffuse+per-ear | 图加载 | source 44100 / graph 48000，main 4.29 ms，11.73/11.73° | ✅ |
| 定稿默认（BW4+diffuse+per-ear） | 新默认图加载、送量正确 | `lfe_experiment: lowpass butterworth4 timbre diffuse per-ear`；27_95 main 4.29 ms、LFE 0/0、11.73/11.73°；60 Hz RMS 0.02755、120 Hz 0.02469 | ✅ §4-29 |

## 7. 已知问题与优化清单（2026-11-10 更新）

**真问题**
1. **上混延迟 128 ms**（4096 FFT / 6144 样本，§0G 定稿；2048/64 ms 方案因低频质感被回滚）。自动切换 wait+crossfade 已消除硬切换 click/中置空洞。
2. **延迟上报**——filter-graph 报主/LFE 分支延迟的平均，不是最大分支（历史实测方法见本行旧记录；FFT 回滚后需按新延迟重测）。
3. **图重建的 ~128–150 ms 空洞**（换 HRIR/音色/限幅/电平匹配时，短 IR）：新图需填满 6144 样本上混 + 对齐延迟灌注；旧图只有短 IR 尾巴。上混/模式切换已不重建图。跨世代逐样本 crossfade 未实施。

**已关闭的疑虑**
- "卷积器内部延迟未上报"：PipeWire ≥1.4 convolver 是 0-latency 两段式 FFT 卷积，不存在需要上报的卷积器延迟。实测互相关约 −1 样本。

**已关闭的问题（2026-11-10）**
- **LFE/主路径相位对齐丢失**：新图最初用固定 512 延迟，导致 27_95 的低频 LFE 晚约 3.9 ms。现按实际音色 IR + BW4 在 20–120 Hz 拟合，主路径公共延迟 + LFE 每耳延迟 + 可选 allpass；两脉冲验证从 −160 样本改善到 +10 样本，60 Hz 相干和误差 0.2%。
- **合成 LFE send 的相消/重复低频**（2026-11-10 白天历史，已被 §0C 取代）：当时删除 send，改为 `1+boost·|P|²` 搁架；2026-11-10 晚因听感偏弱/偏前恢复 send（仅上混分支），并保留功率平均 IR + 每耳对齐/allpass。
- **扩散场音色实际是相干平均**：2026-11-10 白天改为 14 通道功率平均的最小相位实现；同日晚因听感偏 mono/前置，改为 stereo hybrid（§0E）；δ/center 仍可选。
- **center_image 是 no-op**：2026-11-10 白天曾实现 constant-power 中置分配 + 总功率归一化；2026-11-10 晚已按用户要求还原为 upstream no-op（§0D）。
- **sample-peak 限幅不是重建峰**：现为 8×/48-tap 多相重建峰检测；独立 32×/128-tap 测试 12k/23k/方波均 ≤约 0.84。
- **bass/limiter 的逐 tap 取模开销**：改为双写线性窗口 + 连续点积；1 GHz powersave 下 bass 9.7%、limiter 1.8%，满频约 2–4%/<1%。
- **非 48k HRIR 的对齐/重采样**：对齐按文件时间轴、BW4 按 48k 图；卷积器 q14；44.1k 图正常。
- **换预设响度跳变**：可选 A 计权中位/参考电平匹配（默认关闭，±8 dB，只改 outGain）。
- **impact 极值可能削波**：可选立体声联动 lookahead 限幅（默认关闭，5 ms/−1.5 dBFS/100 ms）。

**已定稿（2026-10-30 用户试听/指定）**
- 低频固定为 **BW4 + 每耳对齐**；LFE 音色可选 **diffuse（默认）/ delta / center**。旧的 LR4/BW8 与共耳选项及配置键已删除（§4-29）。
- 换歌开头的爆音：upmix 开启时新流路由先 mute 效果图输出 150 ms，再在音量 0 处解除并 100 ms 淡入（`gate_effects_output_locked()`）；源流 30 ms mute 仅作无门控回退。实测首样本爆冲 0.354→0，连续音解除跳变 0.135→0.037，待实际播放器确认。
- 启动时 LFE 送量误显示“上混关闭”：已通过在读取全部配置后再次 `refresh_hrir_analysis()` 修复（§4-29）；状态行现在还会在重建/重载时先显示“刷新中…”，失败/无数据显示“未获取…”，缺失每耳值显示“—”，不会保留旧值或默认 0.00。
- 相位门控在默认 diffuse 下对所有预置通过（LFE 与主声道使用同一组 HRIR 平均，相位抵消）；δ/中心音色才可能出现 AtmosFar/dtshx 禁用或 impact 退回。
- 试听重点：扩散场 vs δ vs 中心 HRIR 的 LFE 音色/外化；BW4 的 120 Hz 量感。参考数据见 §6。

**已关闭的问题（2026-10-28）**
- **播放中切换预设/上混开关的"变响 + 暂停"与 2 s 静音**：根因是删除 sink 触发 WirePlumber 的 MPRIS 暂停 + 流回落默认设备；静音方案只是遮住电平。现改为稳定桥 sink；新图创建约 0.6–1.1 s（等待 registry/Link），旧图在无输入后延迟 1.2 s 销毁；播放中切换 0 MPRIS 暂停、0 尖峰，OFF→ON 约有 30–220 ms 的灌注下陷（短 IR 更明显，见 §7）（§4-13~17、§4-27、§6）。
- **24-bit HRIR 导致效果图静音**：δ IR 头格式错误，已修（§4-15）。
- **正常退出时也会暂停播放器**：`disable_routing` 自回声已修（§4-16）；另在等待桥输入 Link 消失后再给 WirePlumber 结算时间（1500 ms + 播放中 1.2 s），最终测试 0 次 Pause。
- **混合内容增益缺口**：auto 插件 bypass 分支现在给原生 LFE 补 2×；上混开/关的原生 7.1 LFE 实测只差 0.25 dB（§4-19）。
- **非 14ch HRIR**：现在直接拒绝并回滚，不再生成错误空间像（§4-20）。
- **auto 插件内容切换的 128 ms / 75 ms 空洞**：旁路延迟环和上混核心保持预热，宿主回归测试 0 空洞（§4-19）。
- **默认设备被虚拟 sink 占用 / 回退默认又回到虚拟设备**：优先级 0 + 恢复物理默认 + 写入 `default.configured`（§4-24）。
- **内部效果 sink 与桥同名、误选会丢流**：描述区分 + 链接监听改回稳定桥（§4-25）。
- **内置扬声器仍被虚拟化**：`device.form-factor=internal` 自动跳过并释放流（§4-26）。
- **原生多声道被重复低频（send 在图里）**：send 移到 auto 插件上混分支；原生旁路不再加 send，FC/LFE on/off <0.1% 差（§4-22/§13.5）。

**准问题（生态固有）**
5. HRIR 预设是特定耳机测量，通用预设对任意耳机是近似。
6. **图重建的短 IR 空洞**：换 HRIR/音色/限幅/电平匹配时，新图需要 6144 样本上混 + 对齐延迟灌注；短 IR（如 `27_95_142.wav`）约 128–150 ms 空洞（4096 FFT 后按比例增大）。上混开关/LFE 模式已是运行时控制，不再重建图。自动内容类型切换（插件内 bypass↔upmix）有 20 ms crossfade；纯中置 5.1 流在 L/R 静音时仍有 kLatency 级空洞。彻底消除需要双图并行/跨世代 crossfade，当前未实施。
7. HRIR 滤波器本身的低频群延迟/音色随预置变化；对齐只保证等权相干参考，不保证任意权重。

**优化想法（未实施）**
7. 内置扬声器/耳机共用模拟输出时，用设备当前 Route/Port 精确区分（目前只按 form-factor=internal）。社区做法：HeSuVi/EqAPO 用户将 Preamp 调到 **Peak gain ≈ −0.1 dB**（按 IR 峰值校准），而我们的 0.30 固定值恰好等于"预留 10 dB headroom（×0.3）"的经验法则——可作为可选的自适应 preamp。
8. FreeSurround 控制参数暴露到 GUI（filter-graph 支持运行时改 control）。
9. HRIR 切换 crossfade（现在换预设要重建模块）。
10. 48k 锁定改为可选跟随内容（例如跟随设备/内容 44.1k 以减少重采样；HRIR 需重采样）。
11. （调研线索）Linux 已有 Bridge 0.5.0 音频混音器宣称支持 HeSuVi HRIR（r/linux_gaming，源码未获取）；EasyEffects（zita-convolver）、JDSP4Linux 可作延迟/图结构参考。HRTF 最小相位研究（CNKI《近场头相关传输函数的最小相位特性研究》）：HRTF 大体近似最小相位，相位误差在听觉上不可辨——若未来做 IIR 化/缩短 HRIR 时可利用。

## 8. 打包与分发

- **AppImage（2026-10-28 重建）**：`dist/appimage/build_virtual_surround_manager.sh`（quick-sharun），x86-64-v3，产物 `build_appimage/virtual-surround-manager-0.0.1-x86_64-v3.AppImage`（约 101 MB，SHA-256 `320cad4d0b0cb2f3dbb7055699bd24e9ad5ea8a006c04ebef2b52f6cce55b5f5`；精简后 AppDir 约 362 MB）。
  - 脚本使用独立目录：`build-appimage`（编译）、`build_appimage/install`（安装树）、`AppDir-appimage`（打包根），不污染本机 `build/`；版本从 `project(... VERSION ...)` 读取。
  - x86_64 上强制 `-march=x86-64-v3`（要求 AVX2 级别 CPU，CachyOS 默认），插件与应用同批编译。
  - 依赖扫描设 `QT_QPA_PLATFORM=offscreen` 与空的 `PIPEWIRE_RUNTIME_DIR`，不碰用户正在播放的 PipeWire 会话，也不需要 xvfb。
  - **dlopen 的 PipeWire 模块和 SPA 插件必须手动补**：`libpipewire-module-filter-chain.so` 与 `spa-0.2/filter-graph/`。路径必须用 `pkg-config --variable=moduledir libpipewire-0.3` / `plugindir libspa-0.2` 获取；Arch/CachyOS 实际在 `/usr/lib/pipewire-0.3`、`/usr/lib/spa-0.2`，不在 libpipewire-0.3.so 旁边（旧脚本的 `dirname $(ldd ...)` 是错的）。
  - quick-sharun 会误带 `/usr/share/code`（近 1 GB）、WebEngine、GPU 驱动、Plasma/QML 与媒体库。脚本把 QML 裁到本应用实际 import 的模块，然后做一次 ELF 可达性闭包：删除未被任何可执行文件/Qt 插件/QML 插件引用的库，并校验可达根没有缺失依赖。GPU 厂商库和 DRI 不打包，使用目标机器的 Mesa/NVIDIA，避免版本/内核不匹配。
  - 内置：Qt 6.12/Kirigami、Qt Wayland 插件、PipeWire 1.6.10 客户端 + filter-chain 模块 + SPA filter-graph、FreeSurround LADSPA、全部 HRIR。依赖目标机器有运行中的 PipeWire + WirePlumber（CachyOS 默认）和 FUSE；无 FUSE 时用 `APPIMAGE_EXTRACT_AND_RUN=1` 或 `--appimage-extract-and-run`。
  - niri 下直接运行 AppImage 即可；启用“登录时自动启动”时，应用会把 desktop 文件的 `Exec` 改成 AppImage 绝对路径（`$APPIMAGE`）。
  - 隔离 PipeWire + null sink 验收（2026-10-28）：AppImage 内创建 `effect_input.virtual-surround-manager` 与 `….effects.1`；LADSPA 从 AppImage 的 `/usr/lib/ladspa` 加载；1 kHz 立体声被识别为“检测到立体声，上混已生效”；录音 RMS 0.03819、峰值 0.05585，与 AppDir 运行一致。上混关闭路径 RMS 0.02904。完整运行日志无缺库/启动错误。
  - 旧 `virtual-surround-manager-15fb76f-x86-64-v3.AppImage`（201 MB）构建于 LFE 重构之前，早于稳定桥/24-bit δ IR 修复，**不要再分发**。
- 应用配置：`~/.config/virtual-surround-manager`（**单文件**，非目录；`lfe_mode=balanced|reference|impact`、`lfe_timbre=diffuse|delta|center`（默认 diffuse；旧 `lfe_lowpass`/`lfe_per_ear_alignment` 启动时删除）、`preset_level_match=false|true`、`output_limiter=false|true`）；HRIR 用户目录：`~/.local/share/virtual-surround-manager/hrir_wav/`；LFE 音色 IR：`~/.local/share/virtual-surround-manager/lfe_bypass_delta.wav`（δ，每次建图重新生成）、`lfe_diffuse_ir.wav`（扩散场，仅 `lfe_timbre=diffuse` 时生成；中心音色直接用 HRIR）。
- **用户 HRIR 生成参数（27_95_142 / LR20260404）**：完整记录、来源截图、生成设置与 SHA-256 在 `docs/hrir/27_95_142/README.md` 和 `parameters.json`。2026-04-04 输入：`x1=14.0`、`x3=20.0` cm，`Input left ear only` 未勾选；`dL=1.94/0.43/1.92/6.41/0.56/1.30`，`dR=1.75/0.45/1.64/5.55/0.60/1.20`。不要与 2026-01-09 HRTF1 的 `x1=14.5`/`x3=19.5`/left-only 旧参数混用；0/95/265 通道可逐样本复现，27/142/333/218 依赖 MATLAB 专有 `interpolateHRTF`（最近邻+1 样本近似 0.984–0.996）。

## 9. 测试方法与环境速查

- 环境：CachyOS，PipeWire 1.6.x；工具：`pw-play`、`pw-record`、`parec`、`pactl`、numpy。
- **隔离测试**：`pactl load-module module-null-sink sink_name=vsm_test_sink` → `pactl set-default-sink` → 重启应用（filter-chain 在创建时连默认设备）→ 测完恢复默认设备并卸载 null sink。
- **录音**：必须用 `parec --device=vsm_test_sink.monitor --format=float32le --rate=48000 --channels=2 --raw` 走管道读固定字节数（timeout/SIGTERM 会截断 wav 文件）。`pw-record` 无法按名字连接 monitor，且目标不匹配会静默录麦克风——**务必先验证连接**（`pw-dump` 查 Link 对象确认）。
- **测试信号**：8ch WAV，WAVEFORMATEXTENSIBLE + PCM(0x0001) 16-bit，通道掩码 0x63F；往返校验电平。
- 项目自带：`cmake -B build -G Ninja -DBUILD_TESTING=ON -DBUILD_LADSPA_TEST_HOST=ON && cmake --build build && ctest --test-dir build --output-on-failure`（5 项：appstream、hrir_wav、hrir_analysis、plugin_regression、plugin_smoke）。`plugin_regression_test` 现在覆盖：任意/变块长、短原生事件、限幅脉冲/联动/阈值/最小 lookahead、bass 对称/非负频响/DC 增益/HF 单位增益/相位、独立 32× 重建峰（12k/23k/方波）、center_image no-op、auto 切换。ASan/UBSan 用同配置加 `-fsanitize=address,undefined`，约 372 s。注意：**系统 `/usr/lib/ladspa` 必须安装与 `build/bin` 同批的插件**，否则 filter-chain 会在新 label（`output_limiter`/`binaural_bass`）上报 “cannot create label”；`find_ladspa_plugin()` 现在要求这四个 label 都存在。
- **端到端隔离播放（2026-10-30）**：`dbus-run-session` + `pipewire/wireplumber/pipewire-pulse` + null sink + `build/bin/virtual-surround-manager`；`paplay --volume=65536 --device=effect_input.virtual-surround-manager tone.wav`（**必须显式 volume**：隔离 Pulse 的 stream-restore 可能把 paplay 恢复到 0%，会误判成应用静音）；`parec --device=vsm_test_sink.monitor ...`。27_95 balanced/440 Hz 实测 RMS 0.018020、peak 0.027624，限幅开/关一致。
- **44.1k HRIR 测试**：`ffmpeg -i 27_95_142.wav -af aresample=44100:resampler=soxr:precision=28 -c:a pcm_f32le 27_95_44k.wav` 放入隔离 `XDG_DATA_HOME/virtual-surround-manager/hrir_wav/`；应用日志应出现 `source 44100 Hz, graph 48000 Hz`、`fit 3.98 ms`、`all-pass 11066.6 Hz Q 0.143`；δ 文件为 44.1 kHz，图正常加载。
- **LFE 定稿默认测试（2026-10-30）**：配置只写 `lfe_timbre=diffuse|delta|center`（不写时默认 diffuse）。日志应出现 `lfe_experiment: ... lowpass butterworth4 timbre diffuse per-ear, main ... ms, LFE L ... / R ... ms, residual L ... / R ... deg`；启动后分析行应直接显示正确 LFE 送量（不再是“上混关闭”）。27_95 默认 60 Hz 端到端 RMS 0.02755、120 Hz 0.02469。`/tmp/vsm_exp_case.sh`（图）与 `/tmp/vsm_exp_e2e.sh`（60/120 Hz 录音）可复跑；换歌爆音的隔离测试用 `/tmp/vsm_song_change.sh`（upmix 开启时日志应出现 `gate_effects_output: muted for 150 ms` 与 `fade-in start`，而不是源流 30 ms mute；upmix 关闭时才回退到 `fade_in_new_stream: ... muted for 30 ms`）。
- 应用重启：先 `ps -eo pid,comm | grep virtual-surroun` 看清 PID，再 `kill <pid>`，然后 `setsid nohup /usr/bin/virtual-surround-manager &`。**不要用 `pgrep -f` 拼在同一行**（会匹配到自己的 shell，§5 已栽过）。
- **播放中切换的验收方法（2026-10-28 定稿，可复现）**：完全隔离的 PipeWire + WirePlumber + pipewire-pulse + D-Bus 会话，null sink 做默认输出，假 MPRIS 播放器（Python dbus，mpv 作为子进程）播放稳定正弦，应用用 `VSM_TEST_AUTO_SWITCH_MS=4000` 自动切预置/上混，`parec` 录 null sink monitor，numpy 做 10 ms 窗 RMS/峰值包络；再看假播放器的 Pause 日志。
  - 启动骨架（`/tmp/vsm_pw/run_audio_test.sh` 已有一份可直接跑的版本；`fake_mpris.py` = 假播放器，`mpv_poll.py` = IPC 轮询）：
    ```
    mkdir -p /tmp/vsm_pw/{run,config,data}; chmod 700 /tmp/vsm_pw/run
    dbus-run-session -- bash -c '
      export XDG_RUNTIME_DIR=/tmp/vsm_pw/run PIPEWIRE_RUNTIME_DIR=/tmp/vsm_pw/run
      export XDG_CONFIG_HOME=/tmp/vsm_pw/config XDG_DATA_HOME=/tmp/vsm_pw/data
      pipewire & wireplumber & pipewire-pulse & sleep 2
      pactl load-module module-null-sink sink_name=vsm_test_sink
      pactl set-default-sink vsm_test_sink
      VSM_TEST_AUTO_SWITCH_MS=4000 QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1 \
        build/bin/virtual-surround-manager &'
    ```
  - 验收阈值：切换过程 0 个 >3× 基线的 10 ms 窗；0 个 ≥20 ms 静音段（上混 OFF→ON 自身约 70 ms 的延迟线灌注除外）；MPRIS Pause 0 次；mpv `pause=false`。总替换耗时约 0.6–1.1 s（等待新图 registry/Link），不是音频空洞。
  - **必须加对照**：`kill -9` 应用（不清理、桥 sink 在流仍链接时消失）→ 假播放器应在 ~10 ms 内收到 Pause，证明测试真的能捕获用户症状；否则是假阴性。正常 `SIGTERM` 退出则必须 0 次 Pause。
  - 预置列表要覆盖 **24-bit PCM**（如 `AtmosRenderer-*.wav`）和 32-bit float（`AppleSpatial*.wav`）；只测 32-bit 会漏掉 δ IR 头 bug。
  - `VSM_TEST_AUTO_SWITCH_MS` 钩子在 `FrontendManager` 构造函数里，**默认关闭**（未设环境变量时完全不生效）；本机 Wayland 无法点 UI，所以用它走与 UI 相同的 setter 路径。
- Wayland 会话下 GUI 自动化不可用（`xdotool` 查不到 X11 窗口），验证一律回到"同一条代码路径 + 环路实测"。
- 配置改动后需重启应用才生效（GUI 内的预置/上混/LFE 模式会即时只换效果图，不重启）。
- LFE 三模式频响脚本：`/tmp/vsm_modes/run_mode_test.sh`（隔离会话，40/80/100/120 Hz 多音，依次 reference/balanced/impact，录制 null sink monitor 并用 numpy 比较）。
- 实时检测验证：隔离会话播放 8ch 全通道 WAV → 应用日志应出现“检测到多声道，自动直通”；换回立体声 → “检测到立体声，上混已生效”。日志来自 `refresh_input_detection` 的状态变化（qDebug）。

## 10. 工作规则（用户偏好，务必遵守）

1. **参数/算法必须有依据**：标准、接口契约、作者定义或实测，并在需要处注明；明确bug与必要重构直接修复，只有涉及设计取舍或决策才询问（2026-11-09用户更新，取代旧的逐项确认规则）。
2. **允许大胆改源码**（用户原话："所有源码都大胆改，不用怕改坏"），但要保持真实、诚实，不确定就说不确定。
3. **已定稿的参数不要反复重新论证**（§2：70/90 插件端口、分源增益、120 Hz **BW4**（2026-10-30 定稿，取代 LR4）、每耳对齐、扩散场 LFE 音色）。
4. **结论要能被数据复现**，尤其是通道路由/延迟类问题：禁止只靠听感或录音下结论（见 §5 教训）。
5. 用户重视与 HeSuVi/行业惯例的对照；解释问题时先给结论，再给依据。
6. 系统级操作（sudo 安装等）执行前告知用户；密码等敏感信息绝不写入文档或仓库。

## 11. 提交历史（关键节点）

| 提交 | 内容 |
|---|---|
| `c755918` | LFE 通路按行业依据重构（70/90、分源增益、120 LR4）、打包用户 HRIR、AppImage 修复 |
| `3ea3091` | HeSuVi 通道映射权威说明 + IR 级验证（README/注释） |
| `cc6fde6` | LFE 直通模式（δ IR + GUI 开关） |
| `270d51c` / `0846c25` / `9837ecd` | 上一个 LLM 的切换修复尝试（重叠重建 / 钉路由 / 流静音 + 确认框）；**2026-10-28 已证明不足，mute/confirm/pin 代码全部删除**，见 §4-11/13 |
| `86c5cc5` | 稳定桥 sink + 只重建效果图（§4-14）；WirePlumber MPRIS 暂停根因；24-bit δ IR 头修复（§4-15）；`disable_routing` 自回声修复（§4-16）；隔离会话 + 假 MPRIS 验收（§4-17、§6）；重启过期 target 接管修复（§4-18）；删除静音/确认/回滚/钉路由机制；加默认关闭的 `VSM_TEST_AUTO_SWITCH_MS` 测量钩子；实机安装并验证 Edge 经新桥路由 |
| `1095d78` | auto 插件内容切换无空洞 + 原生 LFE bypass 2×（§4-19）；非 14ch HRIR 拒绝（§4-20）；短 IR OFF→ON 90ms 缺口记录（§4-21）；插件宿主回归测试升级；README/AGENTS 的 LFE/切换文档同步（含旧 `convLFE` 表修正） |
| `d65ff37` | 删除已无行为的 `lfe_bypass` 旧配置键（与 `channels` 一起在启动时清理） |
| 上一轮（2026-10-28，已并入本轮） | δ IR 按 HRIR 中心声道 40–80 Hz 归一化；reference + LFE send（−6 dB）作为临时默认；60 Hz 实测 send −6 0.02890 / send −3 0.03219 / reference 0.02223 |
| 本次（2026-10-28，待试听后提交） | **三种用户可选 LFE 模式**（balanced/reference/impact，`lfe_mode`）+ 删除 legacy/env；**HRIR/LFE 相位对齐（任意 14ch HRIR 自动计算：20–120 等权拟合 delay + 自动 all-pass；UI 显示归一化/延迟/平均/最坏残差）**；impact 改为 0 dB send（无静态 EQ）；**send 移入 auto 插件 `lfe_send` 控制，修复原生多声道重复低频**；插件导出 `vsm_auto_upmix_state` + `vsm_auto_blocks` 心跳；实时输入检测；虚拟 sink 默认保护；内部效果 sink 防误选；内置扬声器跳过；旧效果图 1.2 s 延迟销毁修复切换 MPRIS 暂停；δ IR 上限 2s→10s；中文 UI；§13 声学/数值复核（LR4/δ/对齐/模式/多声道/headroom/ITD） |
| 本次（2026-10-30，工作树未提交） | **send 30°/15° 门控**（AtmosFar/dtshx 不再抵消）；**auto 切换 wait+crossfade**（20 ms，中置/click 修复）；**非 48k HRIR q14 + 48k 对齐**；**可选预设响度匹配**（中位 A 计权，±8 dB）；**可选输出限幅**（`output_limiter`，5 ms/−1.5 dBFS/100 ms）；**实验性低频三轴**（LR4/BW4/BW8、δ/center/diffuse、每耳对齐，先默认关闭；同日随后定稿，见下一行）；UI/README/插件 README/AGENTS §14 同步；宿主与隔离验收通过 |
| 定稿（2026-10-30，工作树未提交） | 用户试听后定稿：LFE 低通固定 **BW4**、**每耳对齐默认**、**扩散场音色默认**（δ 第二、中心第三）；删除 `lfe_lowpass`/`lfe_per_ear_alignment` 配置与 UI 及 LR4/BW8 路径；修复启动时 LFE 送量误显示“上混关闭”（配置读取后重算）；新流 30 ms mute 修复换歌爆音（后续实测首批仍泄漏，已改为效果图输出 150 ms 门控 + 100 ms 音量淡入，见下一行）；状态显示改为失效/缺失显式标记；文档同步 |
| 2026-11-10 最终架构（工作树未提交） | 2048 FFT/3072 样本；删除合成 LFE send，改为已渲染双耳信号上的 `1+boost·|P|²` 零相差搁架；LFE 音色改为 14 通道功率平均最小相位 IR；恢复并重算 LFE/主路径公共/每耳延迟 + allpass（两脉冲 −160→+10 样本）；`center_image` constant-power 实现；8×/48-tap 重建峰限幅；上混/LFE 模式运行时控制；bass/limiter 双写线性窗口去逐 tap 取模；CTest 5/5、ASan/UBSan 5/5、隔离 e2e 60 Hz +3.00 dB 与相干和 0.2%；README/AGENTS/docs/plugin README 同步 |
| 2026-11-10 晚 低频修正（工作树未提交） | 用户反馈 shelf 偏弱/偏前；恢复单声道 LFE send（balanced 0.501/−6 dB、impact 1.0/0 dB、reference 0，仅上混分支），删除主路径 `binaural_bass` 搁架及 512 延迟；diffuse 功率平均 IR + 每耳对齐/allpass 保留；原生多声道 balanced/reference <0.1 dB；应用侧 sink 延迟 3166；plugin_smoke/plugin_regression 恢复 send 断言；AGENTS §0C、README/QML 同步 |
| 2026-11-10 晚 还原 FreeSurround（工作树未提交） | 按用户要求删除 2026-11-10 白天新增的 `center_image` C→FL/FR phantom 与全局功率归一化；graph 不再设置 `center_image`；上游 tygyh/Kodi/Real3D-VST 与旧提交均只保存不读；send 0.501 下 60/80/100/120 与 `f615312` 差 0.01–0.07 dB；plugin 测试改为 no-op；AGENTS §0D |
| 2026-11-10 晚 LFE IR 结构修正（工作树未提交） | 用户反馈偏低频偏高/没劲；A/B 排除了稳态 RMS（20–120 与旧版 ±0.2 dB、LFE-only 60 相同）；改为 stereo hybrid diffuse IR：相干相位/时间/L-R 65% + 35% 每耳功率幅度，2ch ch0/1；LFE-only 60 比旧版低 0.53 dB，主对齐 4.19 ms/全通 8.00 kHz；CTest 5/5；AGENTS §0E |
| 2026-11-10 深夜 回滚定稿（工作树未提交） | 低频全部回滚到 f615312 行为 + **FFT 4096 回滚**（2048 版实测 20–120 Hz −4~−5 dB，§0G）；删除 binaural_bass/quality_dsp/ir_processing；恢复相干 diffuse IR、30°/15° send 门控（建图持久化）；CTest 5/5；`~/.local/bin/vsm-version` 多版本切换工具 + 版本库；文档精简（删早期审计数据、压缩 §0B/0C/0E、acoustics.md 刷新） |
| 更早 | `15fb76f` Flatpak 崩溃修复（上游历史） |

## 12. 低音管理复核（2026-10-28；调研历史）

> **最终实现见 §0F/§0G**：mono LFE send（仅上混分支，30°/15° 门控），无主路径搁架；reference 无 send，保留原生 LFE。本节保留 2026-10-28 的 send 调研过程与数据，仅作历史记录；`VSM_BASS_MODE` / `VSM_BASS_LEGACY` / `VSM_LFE_SEND_DB`、legacy 70/90 和 +4 dB 低架均已删除。
> 结论：概念方向（LFE 独立、120 Hz 带限、源相关增益、δ IR 不卷积）是对的；之前的问题是 **δ IR 绝对电平未归一化**，以及 FreeSurround 70/90 重定向是"扬声器低音管理"，在双耳渲染里会带来随 HRIR 变化的低频染色。

### 12.1 权威结论（外部）

| 结论 | 出处 |
|---|---|
| 扬声器回放 LFE +10 dB（LFE 录制时低 10 dB；只补原生 LFE，不补主声道重定向的低音） | UA Apollo FAQ、Genelec 7050C（+10 dB LFE 开关） |
| **双耳渲染 LFE 只 +5.5 dB**：先按扬声器 +10 dB，再因双耳只模拟两个点声源而 −4.5 dB | Midifan 2024-07-28（Dolby 工程师转述）；Apple Logic 的 LFE 双耳模式固定 "Off"（仍包含、不作距离建模） |
| 主声道在双耳渲染里保持全频并通过 HRTF；LFE 是独立效果声道；不把主低音搬到 LFE | Apple/Dolby 双耳文档；HeSuVi 立体声矩阵 `SUB=0` |
| 无 sub 的系统用**反向低音管理**：把 LFE/sub 混入主声道并**大幅衰减**；LFE 在 stereo downmix 中被丢弃，重要低频必须留在主声道 | Microsoft bass management；Apple Compressor |
| AVR 低音管理：小箱主声道 HPF + sub LPF，LFE 另加 120 Hz LPF；THX 分频 80 Hz；LFE LPF 常见 24/48 dB/oct @120 Hz | Denon AVC-X6500H 手册、Yamaha CX-A5200 实测（Audioholics）、SVS、JRiver |
| HeSuVi 的 LFE：中心 HRIR + `lfc.txt` 2×（+6 dB）+ LP 111 Hz；相对主声道约 −4 dB，和双耳 +5.5 dB 基准只差约 0.5 dB | 原版 HeSuVi 安装包（本机已验证） |

### 12.2 实测（离线：LADSPA 插件 + 本机 HRIR 卷积图，中心声像 20–200 Hz）

对比四种：**B = 不重定向**（`use_lfe=0`，主声道全频过 HRIR，无 LFE）；**A = 当前**（FreeSurround 70/90 + 图里 LFE 1× + 全尺度 δ）；**A_norm = 当前 + δ 按中心 HRIR 归一化**；**D = 标准 LR4 80 Hz 低音管理**（`use_lfe=0` + 7 路 HPF/LPF，LPF 求和进 LFE）。

| HRIR | A/B @20–60 Hz | A/B @80 Hz | A_norm/B @60 Hz | 原生 LFE/FC @60 Hz（同电平输入） | 减去 LFE 录制 −10 dB 后相对主声道 | 双耳目标 −4.5 dB |
|---|---|---|---|---|---|---|
| AppleSpatial | +2.4 dB | −1.9 dB | −0.6 dB | **+8.33 dB** | −1.67 dB | **+2.83 dB 偏热** |
| AtmosRenderer-Far | +4.5…+7.4 dB | −7.3 dB | −1.1 dB | **+13.81 dB** | +3.81 dB | **+8.31 dB 偏热** |
| 27_95_142（短 IR） | +3.8…+4.4 dB | −3.7 dB | −0.4 dB | **+9.63 dB** | −0.37 dB | **+4.13 dB 偏热** |

- **δ 归一化因子**（中心 HRIR ch6/13 在 60 Hz 的幅度）：AppleSpatial 0.721（−2.84 dB）、AtmosRenderer-Far 0.384（−8.32 dB）、27_95_142 0.622（−4.13 dB）。按此缩放 δ 后，原生 LFE 的 2× 在 60 Hz 恰好 +5.5 dB（相对中心 HRIR），即与 Dolby 双耳基准一致；提取低音的 1× 为 0 dB（相对中心 HRIR），即重定向低音的正确电平。
- 当前 A 的深低音隆起和 80 Hz 凹陷**随 HRIR 变化**（Atmos 最严重），说明"用户喜欢的密度"同时包含了一个**随预置变化的染色**；A_norm 去掉深低音隆起，但 80 Hz 凹陷仍在（甚至更明显）。
- **D（标准 LR4）并不能解决**：δ 是无色/平坦的，主声道是带 HRIR 染色的，两者不是同一传递函数；LR4 的互补性只在"两路同一响应"时成立。实测 D 在 Apple 上仍有 +2.5 dB @20、−1.8 dB @80；归一化后 −0.3…−3.2 dB。**标准 LR4 是扬声器（小箱+sub）的低音管理，不是双耳渲染的参考做法。**

### 12.3 结论与可选方案

**必须修（客观电平错误）**
1. **δ IR 按所选 HRIR 的中心声道低频幅度归一化**：`write_lfe_bypass_delta` 生成 δ 时乘以中心 HRIR（ch6/13）40–80 Hz（或 60 Hz）的幅度。这样：
   - 原生 LFE：图里 2.0（或插件 bypass 2×）× 归一化 δ = 相对中心 HRIR +6 dB，减 10 dB 录制 = −4 dB，贴近 Dolby 双耳 −4.5 dB；
   - 提取低音：图里 1.0 × 归一化 δ = 0 dB，符合"重定向低音不加 +10 dB"；
   - LFE 电平不再随预置漂移 8 dB。**这会显著降低 Atmos 预置当前的 LFE（约 −8 dB ），需要试听确认。**
2. **LFE 120 Hz LR4 保留**；可选的 20–30 Hz 高通/DC 阻断另议（Dolby LFE 下界 3 Hz，但耳机/直流安全性）。

**最终方案（2026-10-28 实现，用户可选）**
- **自然耐听 reference**：`use_lfe=0`，主声道全频过 HRIR；LFE 只保留原生 LFE（归一化 δ + 2×）。最接近 Apple/Dolby 双耳渲染和 HeSuVi 立体声 `SUB=0`；没有 70/90 的深低音隆起和 80 Hz 凹陷。
- **均衡听感 balanced（默认）**：reference + 上混时由插件把 7 路主声道输出求和 ×0.501 写入 LFE（图只做 LR4/δ）；主低音仍在 HRIR 里，附加单声道低频密度。相位对齐后实测相对 reference：Apple +3.4/+3.0/+2.5/+1.9 dB、27_95 +3.6/+3.1/+2.6/+2.0 dB。
- **低音劲感增强 impact**：同一 send 提高到 0 dB；相位对齐后与主声道同相，实测 Apple +5.8/+5.3/+4.5/+3.5 dB、27_95 +6.1/+5.5/+4.7/+3.6 dB。静态低架已删除（用户反馈软糊、无劲感）。
- **不推荐**：为耳机路径加标准 LR4 低音管理——它解决不了 δ 与 HRIR 的传递函数差异，且双耳参考渲染器并不把主低音搬到 LFE。FreeSurround 的 70/90 重定向与 legacy/env 开关已删除。

### 12.4 实现状态（2026-10-28）

1. ✅ **δ 归一化**：`lfe_reference_gain` 用中心 HRIR（ch6/13）40–80 Hz 平均幅度缩放 δ 脉冲；实测 AppleSpatial 0.7263、AtmosRenderer 0.384、27_95_142 0.622。原生 LFE 上混开/关 0.07699/0.07743（0.05 dB）。
2. ✅ **三模式**：`lfe_mode=balanced/reference/impact` 写入配置并即时重建效果图；balanced/impact 的 send 仅在上混开启时存在；impact 为 0 dB send，无静态 EQ。
3. ✅ **多音实测（AppleSpatial，40/80/100/120 Hz）**：balanced 相对 reference +3.36/+3.01/+2.51/+1.88 dB；impact +5.79/+5.27/+4.54/+3.48 dB；27_95 为 +3.56/+3.20/+2.75/+2.08 与 +6.08/+5.46/+4.73/+3.70 dB。
4. ✅ **切换**：与预置/上混共用稳定桥替换路径；播放中模式切换录音无新增 ≥20 ms 静音、0 个 >3× 基线窗、MPRIS 0 Pause。
5. ✅ 三种 LFE 模式保留；用户已选定 BW4 + 扩散场 + 每耳为默认（§4-29）；当前 working tree 未提交。

测量脚本：`/tmp/vsm_pw/ladspa_dump.cpp`（导出插件 8 声道）、`/tmp/vsm_pw/analyze_bass.py`（HRIR 卷积 + LFE/LR4/归一化对比）。数据在 `/tmp/vsm_pw/sweep/`。

## 13. 声学/数值复核（2026-10-28，可复现）

> 数据来源：本机 27_95_142 / AppleSpatial / AtmosRenderer HRIR、隔离 PipeWire 会话的多音录音、Python DFT/双二阶计算。
> **注意**：本节是 2026-10-28 **LR4 + δ + 共耳**时代的复核数据；2026-10-30 定稿改为 **BW4 + 扩散场 + 每耳**，新的默认数值见 §2/§4-29/§6。LR4 的推导与对照组保留为历史依据。

### 13.1 120 Hz LR4（两只 Q=0.7071 低通级联）

| f (Hz) | 20 | 40 | 60 | 80 | 100 | 120 | 150 | 240 |
|---|---|---|---|---|---|---|---|---|
| 幅度 dB | −0.007 | −0.107 | −0.527 | −1.566 | −3.418 | −6.021 | −10.735 | −24.611 |

40–120 Hz 相位拟合群延迟 **4.36 ms**。上混 send 与主声道路径的相位差主要是这个 LR4 延迟减去 HRIR 低频群延迟；这解释了旧版 send 在 90 Hz 以上抵消。

### 13.2 δ IR 归一化

`lfe_reference_gain` 使用中心声道 ch6/13 在 40/50/60/70/80 Hz 的幅度平均：

| HRIR | 增益 | dB |
|---|---|---|
| AppleSpatial | 0.72627 | −2.78 |
| 27_95_142 | 0.62003 | −4.15 |
| AtmosRenderer-Far | 0.48851 | −6.22 |
| atmos.wav | 0.65427 | −3.69 |

原生 LFE 链路：插件旁路 ×2 × δ 增益 × LR4 × outGain。相对中心 HRIR（只 × outGain），60 Hz 比值为 `2 × 0.9414 = 1.883`（+5.50 dB），正好是 Dolby 双耳 LFE 的 +5.5 dB；LFE 录制低 10 dB → 相对主声道 **−4.50 dB**。该比值不随 HRIR 变化（δ 已按同一中心响应归一化）。

### 13.3 三种模式（27_95_142，40/80/100/120 Hz，相对 reference）

| 模式 | 实测 | 理论（send 幅度=LR4、相位 0） |
|---|---|---|
| balanced（send 0.501） | +3.58 / +3.10 / +2.59 / +1.99 dB | +3.48 / +3.04 / +2.53 / +1.94 dB |
| impact（send 1.0） | +6.13 / +5.39 / +4.54 / +3.59 dB | +5.97 / +5.28 / +4.47 / +3.52 dB |

AppleSpatial 同为 +3.36/+3.01/+2.51/+1.88 与 +5.79/+5.27/+4.54/+3.48 dB。实测与“send 与主声道同相、幅度等于 LR4 响应”的模型偏差 ≤0.2 dB，说明 send 电平、LR4、对齐 delay 的实现一致。

### 13.4 对齐 delay 的残差

| HRIR | 20–120 拟合延迟 | 斜率残差 | 对齐后平均/最坏声道 | 自动 all-pass 后平均/最坏 |
|---|---|---|---|---|
| AppleSpatial | 2.98 ms | 1.40° | 8.22° / 26.02° | 3.70° / 18.84°（10206 Hz/Q0.10） |
| 27_95_142 | 3.98 ms | 0.88° | 5.05° / 22.84° | 2.18° / 18.31°（11067 Hz/Q0.143） |
| AtmosRenderer-Far | —（跳过） | 43.95° | — | — |

27_95 录音中 send 相对主声道的相位在 40–120 Hz 约为 0–4°（跨独立录音的相位测量有启动相位/对齐误差，仅作旁证）；模型平均残差经 all-pass 后为 2.18°。固定 delay/all-pass 取全 14 通道平均，纯中置/纯侧向内容仍有约 18° 的最坏声道残差，这是单一 LFE 通路的合理折中。

### 13.5 原生多声道（send 不应出现）

- FC-only 8ch 信号：upmix on vs off RMS **0.0130723 vs 0.0130775**（0.04%），峰值逐样本相同。
- LFE-only 8ch（WAVEFORMATEXTENSIBLE，mask 0x63F）：upmix on vs off RMS **0.064609 vs 0.064661**（0.08%），峰值相同。
- 结论：插件 send 只在上混分支出现；原生 5.1/7.1 旁路不会重复低频。

### 13.6 HRIR 通道映射/ITD（27_95_142）

| 扬声器 | ITD（近耳领先） | 近耳/远耳电平 |
|---|---|---|
| FL | −0.208 ms | +12.0 dB |
| FR | +0.208 ms | −10.4 dB |
| SL | −0.688 ms | +21.0 dB |
| SR | +0.646 ms | −19.2 dB |
| RL | −0.354 ms | +14.2 dB |
| RR | +0.333 ms | −11.6 dB |
| C | 0.000 ms | +1.0 dB |

符号/零 ITD 与物理规律一致；部分合成/反射强的预置（AppleSpatialMovie、dtshx）用全体互相关会取到晚反射，改用开头 5 ms 窗后 ITD 变为合理的 ±0.2–0.7 ms——审计时要用起始窗，不能只看全 IR 峰值。

### 13.7 Headroom

- 测试输入峰值 0.1398；impact 输出峰值 0.0640 → 换算到满刻度输入约 **0.458**；满刻度 40 Hz 单音 per-ear 约 **0.514**（reference 0.254，balanced 0.385）。
- outGain 0.30 是 −10.46 dB；impact 最坏低频相干叠加 +6 dB 后仍有约 4.5 dB 数字 headroom（纯 40 Hz 满刻度实测约 −5.8 dBFS）。默认没有 limiter；2026-10-30 增加可选 `output_limiter`（默认关，5 ms/−1.5 dBFS）。极端“7 个主声道全部满刻度同相 40 Hz”的理论上界仍可能削波，但实测音乐/上混分布远低于此；极端内容可开启 limiter。

### 13.8 已知近似（不是当前 bug）

- δ 归一化取中心 HRIR 40–80 Hz 平均；对低频起伏大的合成预置（Atmos ±20%）LFE 相对主声道的频率响应不是严格平坦。
- 对齐 delay 是每个预置一个固定值；自动 all-pass 只能降低平均残差，不能消除主声道之间约 18° 的最坏残差。
- 对齐 delay 不改变同一主路径内部的空间时序，但使主路径整体增加 3–4 ms 延迟（相对于 LFE/发送路径），总延迟仍远小于 FreeSurround 的 128 ms；all-pass 为二阶、单位幅度，额外延迟极小。
- 原生 5.1/7.1 的 LFE 与主声道内容由录音决定；固定 delay 只能对齐 HRIR/LR4 的参考相位，不能保证每个混音的 LFE 相位。
- 旧效果图销毁延迟 1.2 s 是为了避免 MPRIS 暂停；其 IR 尾巴在这段时间仍可能与新图叠加（此前已有同类残留）。
- 内置扬声器判定用 `device.form-factor=internal`；笔记本内置耳机口共用同一 sink 时也会跳过。

## 14. 与扬声器 AVR 低频管理最佳实践的对照（2026-10-30）

> 依据：用户收集的 `/home/nuofang/桌面/a.txt`（Dolby/SVS/Denon/Onkyo/Barefoot 手册 + 论坛帖合集）。该资料主体是**扬声器 + 低音炮**的低频管理，不是耳机双耳渲染；其中有价值的共识与论坛意见需要区分。

### 14.1 可直接迁移的原则

- LFE 是独立、带限于 120 Hz（Dolby；DTS 内容到 80 Hz）的效果声道；LFE ≠ 超低音。
- LFE 录制比主声道低 10 dB；扬声器回放补 +10 dB；**双耳回放的目标是 +5.5 dB**（+10 再 −4.5，Dolby 工程师/Apple 双耳惯例）。本项目的 2× 原生 LFE（+6）+ 按中心 HRIR 40–80 Hz 归一化的 LFE 音色 IR + 120 BW4 在 60 Hz 为 **+6.0 dB**，比目标高 0.5 dB（旧 LR4 为 +5.5）；严格校准可整条 LFE 路径 −0.5 dB。
- 主声道全频内容不应被丢弃（LFE 在 stereo downmix 中会被丢）；本项目的主声道全频过 HRIR。
- LFE 与主声道的电平/时间要对齐；本项目用中心 HRIR 归一化 + 每预置每耳 delay/all-pass 做低频对齐。
- 不要重复低频；本项目对原生 5.1/7.1 不加 balanced/impact send（只在立体声上混时加），避免 LFE+Main 式重复。
- 保留 headroom；impact 极值可选 limiter。

### 14.2 不应照搬到耳机的设置

| AVR 设置 | 为什么不能照搬 | 本项目 |
|---|---|---|
| Small + 80 Hz HPF / 把主低频重定向到 LFE | 耳机没有小箱/房间限制；HPF 会去掉 HRIR 的低频空间/音色线索，并让低频变单声道 | 主声道全频，`use_lfe=0`；不做 redirect |
| Sub 自身 LPF 最大 / LFE+Main 重复 | 没有物理 Sub；LFE+Main 式的原生多声道重复会让低频叠加 | 原生多声道仅原生 LFE；balanced/impact 只用于立体声上混 |
| LFE LPF 80 Hz | 会砍掉 80–120 Hz 的 LFE 冲击 | 120 Hz BW4（120 处 −3 dB、24 dB/oct；用户 2026-10-30 定稿） |
| LFE +10 dB | 扬声器 SPL 校准，不是双耳电平 | +5.5 dB 双耳目标；当前 BW4 实测 +6.0（高 0.5 dB） |
| 80 Hz 以上有方向性 → Sub 分频点 | 主低频在 HRIR 中应保持空间；LFE 单声道本身无方向 | 主声道全频过 HRIR；δ 为等量 L/R，默认 diffuse 有少量 L/R 差异；send 低通 120 |

### 14.3 合理的组合

**扬声器 AVR（文件语境）：**

- **合理**：80 Hz 分频 + 120 Hz LFE LPF + Sub LPF 最大/bypass + LFE（THX/SVS/Dolby 标准）；小箱用 100–120 Hz 分频 + 同样的 LFE 设置。
- **可选**：LFE+Main（重复主低频；可能低频隆起/定位；不是默认）；文件里的中文文章推荐它，但同一文件的 SVS/Dolby 部分说 LFE 是默认。
- **不合理**：80 Hz LFE LPF + 80 Hz 分频（砍 80–120 LFE）；120 LFE LPF + Sub LPF 80（双重低通）；LFE LPF Off/250（>120 可能定位）。

**本耳机链：**

- **最中性/最合理**：主声道全频 + 120 LFE BW4 + 原生 LFE 2× + 归一化 **δ**（严格无染色/等量 L/R）+ **reference**（对应文件里的 LFE-only）；默认 diffuse 是音色/外化取向，不是严格中性。
- **口味但合理**：balanced/impact + 立体声上混 + **HRIR 已对齐**（+3–6 dB 低频密度；不是 LFE+Main 标准）；impact 注意 headroom/可选 limiter。
- **不合理**：balanced/impact + **未对齐 HRIR**（实测 AtmosFar 40 Hz −4.6 dB、dtshx 100 Hz −3.5 dB；已加门控）；主声道 80 Hz HPF/redirect；原生多声道 LFE+Main 重复；LFE LPF 80；LFE +10 dB。
- **有效但无送量**：balanced/impact + 原生多声道（插件 bypass 不加 send，等同 reference；UI 标签只是模式名）。
- **已定稿的低频设置（2026-10-30 用户指定）**：
  - LFE 低通固定 **Butterworth 4 阶 @120 Hz**（−3 dB、24 dB/oct）；LR4/BW8 路径已删除。
  - **每耳 LFE 对齐常开**：L/R 独立 delay/allpass、主路径共用 delay；send 门控取较差耳。
  - LFE 音色可选：**扩散场 HRIR（默认，7 对平均、按中心 40–80 Hz 归一化；非严格等量 L/R，可能带 HRIR 音色）**、δ（第二，严格无染色/等量 L/R）、中心 HRIR（第三）。
  - 当前 BW4 使 27_95 原生 LFE 在 60 Hz 为 +6.0 dB（比 +5.5 基准高 0.5 dB），120 Hz 比旧 LR4 高约 3 dB；这是 BW4 的听感取向。
  - 默认路径走 `lfe_alignment_configured()`；旧 `lfe_alignment()` 仅保留为参考。
  - 已完成：2048 FFT（64 ms）、stereo hybrid LFE IR、LFE/主路径延迟+allpass 对齐、mono LFE send、8× 重建峰限幅。`center_image` 保持 upstream no-op（§0D）。仍未做：HRIR 切换跨世代 crossfade、FreeSurround 控制 GUI、可选跟随内容采样率。

### 14.4 文件中不应照单全收的观点

- “LFE+Main 推荐”只是其中一篇中文文章；同一文件的 SVS/Dolby 资料明确说默认 LFE、LFE+Main 会重复主低频。
- 有帖子把 “LFE 120” 说成“主声道 120 以下交给 Sub”（错误；LFE LPF 只管 LFE 声道），后文已纠正。
- 有帖子说 LFE 80–120 会“交给主声道”（错误；LPF 是滤掉，不是重定向），后文已纠正。
- “100 Hz 以下归 Sub”是个人意见；THX/SVS 是 80 Hz 起、按喇叭 −3 dB 点加约 10 Hz。
- “Sub 不好就设 80 Hz”针对 Sub 定位/轰鸣，不适用于耳机双耳链。
- Dolby LFE 20–120、DTS 20–80；本项目的 120 BW4 两者都覆盖。
