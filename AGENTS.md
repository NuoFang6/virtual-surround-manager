# 项目手册（AGENTS.md）

> **本文件是该项目唯一、最新的上下文文档**，合并了历代会话记录、决策依据与实测数据。
> 面向未来的 LLM/协作者：先读完本文件再动手。发现内容与代码不一致时，以代码和新的实测为准，并更新本文件。
> 最后更新：2026-10-27。

## 1. 项目概况

- **Virtual Surround Manager**：Berny23 上游项目的硬分叉（hard fork），版本从 0.0.1 重新起算。C++20、Qt6 + Kirigami、CMake/Ninja。
- **功能**：创建 PipeWire filter-chain 虚拟 8 声道 sink（7.1），可选 FreeSurround 上混，然后经 HeSuVi 14 通道 HRIR 卷积做耳机双耳渲染，输出 2 声道。
- **用户背景（重要）**：本仓库用户是 **Real3D-VST 的作者**（基于 FreeSurround 改写的插件，网上搜不到）。历史上 80/111 Hz 的分频值出自用户本人，非外部权威；现已改为 70/90（见 §3）。
- **构建**：`cmake -B build -G Ninja && cmake --build build`，安装前缀 `/usr`（`sudo cmake --install build --config Release`）。启动方式见 §9。
- **关键源码**：`src/pipewire_manager.cpp`（滤波图构建：`create_virtual_surround_module`、`find_ladspa_plugin`、`write_lfe_bypass_delta`、`parse_wav_format`）、`src/frontend_manager.cpp`（配置与 Q_PROPERTY）、`src/qml/Settings.qml`、`third_party/freesurround_ladspa/`（LADSPA 插件，双 label：`freesurround71` 与 `freesurround71_auto`）。

## 2. 当前信号链与参数（唯一真相）

### 输入端（虚拟 sink）
| 项 | 值 | 说明 |
|---|---|---|
| 通道 | 8ch，FL FR FC LFE RL RR SL SR | 标准 7.1 顺序 |
| `audio.rate` | 48000（capture/playback 两端都锁） | HRIR 原生 48k，避免重采样 |
| `node.latency` | `1024/48000` | 防止低延迟应用把 quantum 拉低导致 xrun |
| 路由 | 应用通过 metadata 把所有播放流强制改道到虚拟 sink | 产品行为 |

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

### LFE 通路（2026-10-27 定稿）
```
copyLFE → 低通 120 Hz LR4（两只 Q=0.7071 biquad 级联）→ 增益（见下）
        → [默认] convLFE_L/R 用中置 HRIR 通道 6/13
          [直通模式] 用运行时生成的等长 δ 脉冲 IR（无着色、等量 L/R、对齐 ±1 样本）
        → mixL/mixR（In 8）
```
| 项 | 值 | 出处 |
|---|---|---|
| 低通 | 120 Hz，LR4（24 dB/oct），Q=0.7071×2 | Dolby LFE 频带上限 + LR 专业标准（实测频响与理论逐位吻合） |
| 增益 | 上混开 → 1.0；上混关 → 2.0 | UA/Apollo：+10 dB 补偿只适用于原生 LFE 通道 |
| 直通开关 | Settings → "Render LFE without HRIR convolution"（config key `lfe_bypass_enabled`） | 专利 CN107770718A 的虚拟化器惯例（LFE 不卷积 + 等量 L/R + 延迟对齐） |

### 卷积与输出
- **HRIR**：HeSuVi 14 通道 WAV，48 kHz，格式 24/32-bit（`src/assets/hrir_wav/`，含用户自有的 6 个预设，构建时默认打包）。
- **映射**：见 §5（已彻底解决）。
- **LFE 与中置共用 HRIR 通道 6/13**（HeSuVi lfc 惯例的等价实现；卷积线性，数学等价）。
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

链接：
- Microsoft 低音管理：https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/bass-management
- UA Apollo +10 dB：https://m.midifan.com/news_body.php?id=53378
- SVS 分频指南：https://www.svsound.com/blogs/subwoofer-setup-and-tuning/tips-for-setting-the-proper-crossover-frequency-for-a-subwoofer
- JRiver 低音管理：https://wiki.jriver.com/index.php/Room_Correction
- 专利（LFE 不卷积惯例）：https://patents.google.com/patent/CN107770718A/zh
- Apple 双耳渲染 LFE 关：https://support.apple.com/zh-cn/guide/logicpro/lgcp789f000d/11.2/mac/14.4
- HeSuVi 14ch 格式：https://sourceforge.net/p/ash-toolset/wiki/Documentation/
- Dolby LFE 定义：https://professional.dolby.com/tv/dolby-digital/

## 4. 决策史（2026-10-27 会话，按顺序）

1. 用户原设想"上混 LFE → 跳过 HRIR → ×2 → 直通中置"；调查发现 **HeSuVi 也从不对 LFE 跳过卷积**（它把 ×2 LFE 预混进 CVI，卷积前求和），原设想的依据不成立 → 暂不修改。
2. 线性代数确认：HeSuVi 预混方式与本项目"专用 LFE 卷积器（同 IR）再求和"**逐样本等价**。
3. 调查原版 HeSuVi 安装包（`/home/nuofang/下载/HeSuVi_2.0.0.1.exe`，`7z x` 可解）：原版立体声默认矩阵 `SUB=0.0`——HeSuVi 不生成 LFE；挂载盘上的配置是"上混关闭"预设（非默认），且其 `matrix.txt` 的 include 被注释。
4. FreeSurround 提取参数 80/111（用户 Real3D 值）→ 40/90（库默认，试听）→ **定为 70/90**（中点对齐 THX 80 Hz）。
5. LFE 增益改为分源：上混提取 1.0 / 原生 LFE 2.0（Apollo 规则）。
6. LFE 低通 111→**120 Hz LR4**（Dolby 频带 + LR 标准 + JRiver 反抵消）。
7. LFE 走中置 HRIR 保持不变（相位/延迟自动对齐，等效专业 all-pass 对齐做法）。
8. 新增 **LFE 直通模式**（δ IR 等长方案）。用户试听结论：低频密度感/力度感明显改善，其他频率无差异 → 保留为可选开关（默认关，待用户最终决定）。
9. HRIR 通道映射：确定现有值正确（见 §5）。
10. 环路实测全部通过（见 §6）。

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

**验证方法（不要用录音！）**：直接对 IR 文件做通道对互相关。物理规律：近耳领先（前 ~0.2 ms、后 ~0.3 ms、侧 ~0.5–0.7 ms）且响 6–12 dB；中置对零 ITD 等响度。全部 9 个真实预设通过；Apple/Atmos 合成预设有能量不对称但 ITD=0。

**血的教训（录音工具会骗人，本项目已两次踩坑）**：
- `pw-record --target <sink>` / `parec --device=<sink>.monitor` 匹配失败时会**静默回退到麦克风**；`pw-record` 无法按名字连接 monitor（需用 `parec`，或 pulse monitor 名）。
- 手写 WAV 头必须与数据编码一致；声明 PCM 却装 float32 会被解码成 7 倍电平 + 失真的垃圾（本项目实测 0.1 正弦变成 0.48）。测试信号建议直接写 16-bit PCM（WAVEFORMATEXTENSIBLE，subformat 0x0001，mask 0x63F）。
- 交叉验证：任何"通道路由"结论必须能在文件域或同一次录音内的相对时序上复现，不能依赖跨次录音的绝对时间。

**非 14 通道 HRIR**：`oal_dflt.wav`（7 通道）选中后**不会报错**（PipeWire 容忍越界通道号），但空间像错误。README 已注明；可选改进：加载列表过滤非 14ch 文件。

## 6. 实测验证结果（2026-10-27，null-sink 环路）

环境：`module-null-sink` 隔离 → 8ch 测试 WAV → `pw-play → 虚拟sink → 链路 → null sink` → `parec` 录 monitor → numpy。三组配置：上混 OFF/直通 ON、OFF/OFF、ON/ON。

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

## 7. 已知问题与优化清单（按优先级）

**真问题**
1. **上混延迟 128 ms**（FreeSurround 固有）——对游戏/交互是硬伤。方向：直通路径免延迟（需迟滞防抖）、缩小 FFT 块 4096→2048（需听感验证）。
2. **延迟未传播到 sink 输入端口**——应用侧（mpv 等）看到 0 → 音画同步风险。输出端口有 6144，输入端口 0。疑似 PipeWire filter-chain 传播限制，可尝试上报上游；播放时复测确认。
3. **混合内容增益缺口**——上混开关为 ON 时，原生 7.1 的 LFE 也只吃 1.0 增益（Apollo 规则要求 2.0）。静态图无法区分内容类型；可选方案：GUI 增加"强制原生 LFE 增益"选项。
4. **非 14ch HRIR 文件**（见 §5）。

**准问题（生态固有）**
5. HRIR 预设是特定耳机测量，通用预设对任意耳机是近似。

**优化想法（未实施）**
6. convLFE 并入 convFC（HeSuVi 式预混）省 2/16 卷积器（输出逐样本不变）。
7. HRIR 预设响度归一化（换预设音量跳变）。
8. FreeSurround 控制参数暴露到 GUI（filter-graph 支持运行时改 control）。
9. HRIR 切换 crossfade（现在换预设要重建模块）。
10. 48k 锁定改为可选跟随内容。

## 8. 打包与分发

- **AppImage**：`dist/appimage/build_virtual_surround_manager.sh`（quick-sharun）。已修复并验证的坑：
  - 脚本会把**本机 VS Code**（`/usr/share/code`，近 1 GB）误打包——二进制字符串匹配 "code" 导致。打包前删除。
  - GPU 驱动（NVIDIA/Mesa/libLLVM，~350 MB）与 `libQt6WebEngineCore` 应清除。
  - **dlopen 的 PipeWire 模块和 SPA 插件必须手动补**：`libpipewire-module-filter-chain.so` 与 `spa-0.2/filter-graph/`（ldd 收集不到）。否则报 "Error creating virtual surround device"。
  - 成品：~210 MB（x86-64-v3，`-DCMAKE_C_FLAGS/-DCMAKE_CXX_FLAGS="-march=x86-64-v3"`），朋友机器需 Mesa/NVIDIA 正常驱动。
- 朋友版 AppImage 构建于 LFE 重构**之前**（功能落后两个提交），用户满意后需重新打包。
- 应用配置：`~/.config/virtual-surround-manager`（**单文件**，非目录）；HRIR 用户目录：`~/.local/share/virtual-surround-manager/hrir_wav/`；直通 δ 文件：`~/.local/share/virtual-surround-manager/lfe_bypass_delta.wav`（每次创建模块按当前 HRIR 重新生成）。

## 9. 测试方法与环境速查

- 环境：CachyOS，PipeWire 1.6.x；工具：`pw-play`、`pw-record`、`parec`、`pactl`、numpy。
- **隔离测试**：`pactl load-module module-null-sink sink_name=vsm_test_sink` → `pactl set-default-sink` → 重启应用（filter-chain 在创建时连默认设备）→ 测完恢复默认设备并卸载 null sink。
- **录音**：必须用 `parec --device=vsm_test_sink.monitor --format=float32le --rate=48000 --channels=2 --raw` 走管道读固定字节数（timeout/SIGTERM 会截断 wav 文件）。`pw-record` 无法按名字连接 monitor，且目标不匹配会静默录麦克风——**务必先验证连接**（`pw-dump` 查 Link 对象确认）。
- **测试信号**：8ch WAV，WAVEFORMATEXTENSIBLE + PCM(0x0001) 16-bit，通道掩码 0x63F；往返校验电平。
- 项目自带：`ctest --test-dir build`（目前只有 appstream）；插件冒烟：`cmake -B build -DBUILD_LADSPA_TEST_HOST=ON && cmake --build build --target freesurround_ladspa_test && ./build/bin/freesurround_ladspa_test build/bin/freesurround_ladspa.so`。
- 应用重启：`kill $(pgrep -f "^/usr/bin/virtual-surround-manager")`，再 `setsid nohup /usr/bin/virtual-surround-manager &`。
- 配置改动后需重启应用才生效（除 GUI 内开关，会即时重建模块）。

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
| 更早 | `15fb76f` Flatpak 崩溃修复（上游历史） |
