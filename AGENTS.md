# 项目手册（AGENTS.md）

> **本文件是该项目唯一、最新的上下文文档**，合并了历代会话记录、决策依据与实测数据。
> 面向未来的 LLM/协作者：先读完本文件再动手。发现内容与代码不一致时，以代码和新的实测为准，并更新本文件。
> 最后更新：2026-10-28。

## 1. 项目概况

- **Virtual Surround Manager**：Berny23 上游项目的硬分叉（hard fork），版本从 0.0.1 重新起算。C++20、Qt6 + Kirigami、CMake/Ninja。
- **功能**：创建 PipeWire filter-chain 虚拟 8 声道 sink（7.1），可选 FreeSurround 上混，然后经 HeSuVi 14 通道 HRIR 卷积做耳机双耳渲染，输出 2 声道。
- **用户背景（重要）**：本仓库用户是 **Real3D-VST 的作者**（基于 FreeSurround 改写的插件，网上搜不到）。历史上 80/111 Hz 的分频值出自用户本人，非外部权威；现已改为 70/90（见 §3）。
- **构建**：`cmake -B build -G Ninja && cmake --build build`，安装前缀 `/usr`（`sudo cmake --install build --config Release`）。启动方式见 §9。
- **关键源码**：`src/pipewire_manager.cpp`（稳定桥 sink + 可重建效果图：`create_virtual_surround_module`、`replace_virtual_surround_module`、`build_bridge_module_args`、`build_effects_module_args`、`find_ladspa_plugin`、`write_lfe_bypass_delta`、`parse_wav_format`）、`src/frontend_manager.cpp`（配置与 Q_PROPERTY）、`src/qml/Settings.qml`、`third_party/freesurround_ladspa/`（LADSPA 插件，双 label：`freesurround71` 与 `freesurround71_auto`）。

## 2. 当前信号链与参数（唯一真相）

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
| center_image | 0.7 | FreeSurround/Real3D 作者默认 |
| shift / focus | 0.0 | 同上 |
| depth / front_sep / rear_sep | 1.0 | 同上 |
| circular_wrap | 90.0 | 同上 |
| **bass_lo / bass_hi** | **70.0 / 90.0** | 升余弦提取带，中点 = THX 标准 80 Hz |
| use_lfe / upmix_enable | 1.0 / 1.0 | 启用低频重定向与自动检测 |

- 插件按 FFT bin（4096 点）做升余弦幅度拆分：`<bass_lo` 100% 进 LFE；`bass_lo–bass_hi` 渐变；`>bass_hi` 不进 LFE。
- **自动检测**：仅当所有非 L/R 通道静音时上混；检测到多声道内容则旁路（延迟线补偿 128 ms，见下）。迟滞容差导致"孤立短多声道瞬态"会被吞（设计使然，非 bug）。
- **插件固有延迟 128 ms**（4096 FFT × 1.5 块 = 6144 样本），通过 LADSPA latency 端口上报；PipeWire filter-graph 已消费（输出端口显示 6144），但**未传播到 sink 输入端口**（应用侧看到 0，见 §7）。

### LFE 通路（2026-10-27 定稿，直通为唯一行为）
```
copyLFE → 低通 120 Hz LR4（两只 Q=0.7071 biquad 级联）→ 增益（见下）
        → convLFE_L/R 卷积【运行时生成的等长 δ 脉冲 IR】（无着色、等量 L/R、对齐 ±1 样本）
        → mixL/mixR（In 8）
```
| 项 | 值 | 出处 |
|---|---|---|
| 低通 | 120 Hz，LR4（24 dB/oct），Q=0.7071×2 | Dolby LFE 频带上限 + LR 专业标准（实测频响与理论逐位吻合） |
| 增益 | 上混开 → 1.0；上混关 → 2.0 | UA/Apollo：+10 dB 补偿只适用于原生 LFE 通道 |
| 渲染 | **固定直通**（δ IR 等长方案，无开关、无旧卷积路径） | 专利 CN107770718A 的虚拟化器惯例（LFE 不卷积 + 等量 L/R + 延迟对齐）；用户试听确认低频密度/力度改善 |

### 卷积与输出
- **HRIR**：HeSuVi 14 通道 WAV，48 kHz，格式 24/32-bit（`src/assets/hrir_wav/`，含用户自有的 6 个预设，构建时默认打包）。
- **映射**：见 §5（已彻底解决）。
- **LFE 不再经过中置 HRIR**（2026-10-27 固化）：LFE 用运行时生成的等长 δ IR 直通（见上表）；其余 7 个虚拟扬声器各用一对 HRIR（共 14 个卷积器）。
- **求和**：每耳 8 路卷积输出 ×1.0 进 mixL/mixR（同 HeSuVi mix.txt）。
- **输出 headroom**：outGain = 0.30（对齐 HeSuVi 响度预算，实测 FC 冲激峰值 0.5×0.475×0.30=0.07125 与实测 0.0713 吻合）。
- **playback 节点**：`node.passive=true`，2ch，48k，1024 quantum。

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

链接：
- Microsoft 低音管理：https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/bass-management
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
6. LFE 低通 111→**120 Hz LR4**（Dolby 频带 + LR 标准 + JRiver 反抵消）。
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
| 切换耗时（6 次，桥→新效果 sink 的 Link 确认） | 越快越好 | **27 / 30 / 31 / 31 / 37 / 40 / 44 / 45 / 47 ms**（历次） | ✅ |
| 切换电平尖峰（10 ms 窗，null sink monitor） | 0 个 >3× 基线 | **0 个**；最大 1.85× 基线（乐曲/预置自身动态） | ✅ |
| 切换静音段（10 ms 窗，RMS <0.05× 基线） | 越短越好 | **0 个 ≥20 ms**；上混开/关切换有 <20 ms 的瞬时下陷（旧 HRIR 1 s 长尾/旧 FreeSurround 延迟线覆盖新图 128 ms 灌注） | ✅ |
| 短 IR（`27_95_142.wav`，218 帧≈4.5 ms）切换 | 无长静音、无 >3× 尖峰 | ON→ON 无 ≥20 ms 静音（仅 10 ms 级下陷）；OFF→ON 实测无 ≥20 ms 静音；**ON→OFF 有约 128 ms 的旧延迟线重叠**，峰值 1.5–2.3× 基线（相位相关，<3×） | ✅/残留 |
| 切换时 MPRIS 暂停 | 0 次 | 6 次切换 **0 次 Pause**；mpv `pause=false` 全程 | ✅ |
| 正常退出（SIGTERM）时 MPRIS 暂停 | 0 次 | **0 次**（等流离开桥后才删 sink） | ✅ §4-16 |
| 重启后过期 target 的接管（实机 Edge） | 自动回到桥 sink | 修复前 `left alone`（物理直出）；修复后 `routed to effect_input.virtual-surround-manager` | ✅ §4-18 |
| 对照：`kill -9` 应用（sink 在流仍链接时消失） | 必须触发 Pause | **10 ms 内收到 Pause** | ✅ 测试有效性 |
| 旧静音方案（已被取代） | — | 3 次切换各约 1.9–2.0 s 静音；mpv 无 MPRIS 所以没测出真实暂停 | ❌ |

## 7. 已知问题与优化清单（按优先级）

**真问题**
1. **上混延迟 128 ms**（FreeSurround 固有）——对游戏/交互是硬伤。方向：直通路径免延迟（需迟滞防抖）、缩小 FFT 块 4096→2048（需听感验证）。
2. **延迟未传播到 sink 输入端口**——应用侧（mpv 等）看到 0 → 音画同步风险。输出端口有 6144，输入端口 0。疑似 PipeWire filter-chain 传播限制，可尝试上报上游；播放时复测确认。
3. **混合内容增益缺口**——上混开关为 ON 时，原生 7.1 的 LFE 也只吃 1.0 增益（Apollo 规则要求 2.0）。静态图无法区分内容类型；可选方案：GUI 增加"强制原生 LFE 增益"选项。
4. **非 14ch HRIR 文件**（见 §5）。

**已关闭的疑虑**
- "卷积器内部延迟未上报"：**PipeWire ≥1.4 的 convolver 是 0-latency 两段式 FFT 卷积**（小 FFT 处理头部 + 大 FFT 处理长尾，官方 1.4 release notes），不存在需要上报的卷积器延迟。实测互相关 −1 样本亦印证。唯一未传播到输入端口的是 LADSPA 的 128 ms（输出端口已报 6144）。

**已关闭的问题（2026-10-28）**
- **播放中切换预设/上混开关的"变响 + 暂停"与 2 s 静音**：根因是删除 sink 触发 WirePlumber 的 MPRIS 暂停 + 流回落默认设备；静音方案只是遮住电平。现改为稳定桥 sink，实测 27–44 ms 完成、0 尖峰、0 ≥20 ms 静音、0 MPRIS 暂停（§4-13~17、§6）。
- **24-bit HRIR 导致效果图静音**：δ IR 头格式错误，已修（§4-15）。
- **正常退出时也会暂停播放器**：`disable_routing` 自回声把流重新路由回桥，已修（§4-16）。

**准问题（生态固有）**
5. HRIR 预设是特定耳机测量，通用预设对任意耳机是近似。
6. **上混 ON→OFF 切换有约 128 ms 的旧延迟线重叠**：旧效果图的 FreeSurround 延迟线在新图开始输出后还会把最后 128 ms 音频放完，与新图实时输出叠加。实测峰值 **1.5–2.3× 基线（+3.5~+7 dB，相位相关，低于 3× 尖峰阈值）**，听感是短暂的"双声/隆起"，不是原来的持续 +10.5 dB 跳变；ON→ON、OFF→ON 没有这个问题（旧延迟线正好盖住新图灌注）。彻底消除需要双效果图交叉淡化（未实施，复杂度高）。

**优化想法（未实施）**
7. HRIR 预设响度归一化（换预设音量跳变）。社区做法：HeSuVi/EqAPO 用户将 Preamp 调到 **Peak gain ≈ −0.1 dB**（按 IR 峰值校准），而我们的 0.30 固定值恰好等于"预留 10 dB headroom（×0.3）"的经验法则——可作为可选的自适应 preamp。
8. FreeSurround 控制参数暴露到 GUI（filter-graph 支持运行时改 control）。
9. HRIR 切换 crossfade（现在换预设要重建模块）。
10. 48k 锁定改为可选跟随内容。
11. （调研线索）Linux 已有 Bridge 0.5.0 音频混音器宣称支持 HeSuVi HRIR（r/linux_gaming，源码未获取）；EasyEffects（zita-convolver）、JDSP4Linux 可作延迟/图结构参考。HRTF 最小相位研究（CNKI《近场头相关传输函数的最小相位特性研究》）：HRTF 大体近似最小相位，相位误差在听觉上不可辨——若未来做 IIR 化/缩短 HRIR 时可利用。

## 8. 打包与分发

- **AppImage**：`dist/appimage/build_virtual_surround_manager.sh`（quick-sharun）。已修复并验证的坑：
  - 脚本会把**本机 VS Code**（`/usr/share/code`，近 1 GB）误打包——二进制字符串匹配 "code" 导致。打包前删除。
  - GPU 驱动（NVIDIA/Mesa/libLLVM，~350 MB）与 `libQt6WebEngineCore` 应清除。
  - **dlopen 的 PipeWire 模块和 SPA 插件必须手动补**：`libpipewire-module-filter-chain.so` 与 `spa-0.2/filter-graph/`（ldd 收集不到）。否则报 "Error creating virtual surround device"。
  - 成品：~210 MB（x86-64-v3，`-DCMAKE_C_FLAGS/-DCMAKE_CXX_FLAGS="-march=x86-64-v3"`），朋友机器需 Mesa/NVIDIA 正常驱动。
- 朋友版 AppImage 构建于 LFE 重构**之前**，且早于 2026-10-28 的稳定桥/24-bit δ IR 修复；需要重新打包（否则切换预置仍会变响/暂停，24-bit 预置会静音）。
- 应用配置：`~/.config/virtual-surround-manager`（**单文件**，非目录）；HRIR 用户目录：`~/.local/share/virtual-surround-manager/hrir_wav/`；直通 δ 文件：`~/.local/share/virtual-surround-manager/lfe_bypass_delta.wav`（每次创建模块按当前 HRIR 重新生成）。

## 9. 测试方法与环境速查

- 环境：CachyOS，PipeWire 1.6.x；工具：`pw-play`、`pw-record`、`parec`、`pactl`、numpy。
- **隔离测试**：`pactl load-module module-null-sink sink_name=vsm_test_sink` → `pactl set-default-sink` → 重启应用（filter-chain 在创建时连默认设备）→ 测完恢复默认设备并卸载 null sink。
- **录音**：必须用 `parec --device=vsm_test_sink.monitor --format=float32le --rate=48000 --channels=2 --raw` 走管道读固定字节数（timeout/SIGTERM 会截断 wav 文件）。`pw-record` 无法按名字连接 monitor，且目标不匹配会静默录麦克风——**务必先验证连接**（`pw-dump` 查 Link 对象确认）。
- **测试信号**：8ch WAV，WAVEFORMATEXTENSIBLE + PCM(0x0001) 16-bit，通道掩码 0x63F；往返校验电平。
- 项目自带：`ctest --test-dir build`（目前只有 appstream）；插件冒烟：`cmake -B build -DBUILD_LADSPA_TEST_HOST=ON && cmake --build build --target freesurround_ladspa_test && ./build/bin/freesurround_ladspa_test build/bin/freesurround_ladspa.so`。
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
  - 验收阈值：切换耗时 <100 ms；0 个 >3× 基线的 10 ms 窗；0 个 ≥20 ms 静音段；MPRIS Pause 0 次；mpv `pause=false`。
  - **必须加对照**：`kill -9` 应用（不清理、桥 sink 在流仍链接时消失）→ 假播放器应在 ~10 ms 内收到 Pause，证明测试真的能捕获用户症状；否则是假阴性。正常 `SIGTERM` 退出则必须 0 次 Pause。
  - 预置列表要覆盖 **24-bit PCM**（如 `AtmosRenderer-*.wav`）和 32-bit float（`AppleSpatial*.wav`）；只测 32-bit 会漏掉 δ IR 头 bug。
  - `VSM_TEST_AUTO_SWITCH_MS` 钩子在 `FrontendManager` 构造函数里，**默认关闭**（未设环境变量时完全不生效）；本机 Wayland 无法点 UI，所以用它走与 UI 相同的 setter 路径。
- Wayland 会话下 GUI 自动化不可用（`xdotool` 查不到 X11 窗口），验证一律回到"同一条代码路径 + 环路实测"。
- 配置改动后需重启应用才生效（GUI 内的预置/上混切换会即时只换效果图，不重启）。

## 10. 工作规则（用户偏好，务必遵守）

1. **每个参数必须有出处**：权威标准 > 行业经验 > 作者默认/实测，并写进代码注释；无理由的"拍脑袋值"不接受。先列方案/想法，用户确认后再改代码。
2. **允许大胆改源码**（用户原话："所有源码都大胆改，不用怕改坏"），但要保持真实、诚实，不确定就说不确定。
3. **已定稿的参数不要反复重新论证**（§2 的三个参数组：70/90、分源增益、120 Hz LR4）。
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
| 本次（2026-10-28，未提交） | 稳定桥 sink + 只重建效果图（§4-14）；WirePlumber MPRIS 暂停根因；24-bit δ IR 头修复（§4-15）；`disable_routing` 自回声修复（§4-16）；隔离会话 + 假 MPRIS 验收（§4-17、§6）；重启过期 target 接管修复（§4-18）；删除静音/确认/回滚/钉路由机制；加默认关闭的 `VSM_TEST_AUTO_SWITCH_MS` 测量钩子；实机安装并验证 Edge 经新桥路由 |
| 更早 | `15fb76f` Flatpak 崩溃修复（上游历史） |
