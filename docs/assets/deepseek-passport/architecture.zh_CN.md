<p align="right">
  <a href="architecture.md">English</a> · <strong>简体中文</strong>
</p>

# DeepSeek Passport 架构

本文说明 [requirements.md](requirements.md) 中的需求如何在真实板子上得到满足、
哪些设计被否决及其原因，以及剩余风险在哪里。

硬件事实来自
[`components/bsp/include/bsp_pins.h`](../../../components/bsp/include/bsp_pins.h)
和[硬件开发指南](../../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md)。
任何标注 **estimate**（估算）的内容，在被依赖之前都必须由实测替换。

## 0. 本仓库已有的实测经验

三份上游参考笔记描述了在这块完全相同的板子上所做的工作。它们被当作第一手证据阅读，
并在本文中通篇引用；此处不重写其内容。

| Reference | What it establishes |
| --- | --- |
| [网络音频流式传输与内存](../../reference/phoenixzhc/network-audio-streaming-and-memory.md) | 一条有界的流式管线：24 KiB 压缩输入缓冲、2304 采样点的 PCM 缓冲，以及 8 KiB 播放任务栈，被实测为一条 MP3 管线的可用工作点。另外：一个固定 4096 字节的 JSON 文档，在真实 6971 字节响应上崩溃。 |
| [SoftAP 配网与资源预算](../../reference/phoenixzhc/softap-provisioning-and-resource-budget.md) | 启用 SoftAP 之前的**最大连续空闲块（largest free block）**在一个固件上约为 13 KiB，通过推迟音频初始化升高到约 31 KiB。一个可工作的单客户端配置服务器使用了三个 HTTP socket、为 2 的 backlog、6144 字节任务栈，以及 2880 字节的 TCP 发送/接收窗口。 |
| [无 PSRAM 的 LVGL 内存池预算](../../reference/starsms007/lvgl-pool-budget-without-psram.md) | 在 80 KB 的 LVGL 内存池和丰富 UI 下，系统堆稳定在约 131 KB 空闲。决定下一次大额分配能否成功的是 `free_biggest_size`，而不是 `free_size`。内存池耗尽会让画面冻结在半绘制的一帧上，日志中却没有任何提示。 |
| [音频压缩的取舍](../../reference/shinku-chen/audio-compression-trade-offs.md) | 在这块板子上实测的解码开销：IMA-ADPCM 已存在且可忽略，MP3 占用 30–40 KB Flash，Opus 占用 60–80 KB Flash 外加 RAM。原始 16 kHz 单声道 PCM 为 32000 B/s。 |

上述内容中最关键的一个数字是**最大连续空闲块**，而不是总的可用堆。它才是本项目真正会耗尽的资源。

产品负责人于 2026-10-13 做出的决策记录在
[requirements.md](requirements.md#61-answered) 中，并贯穿本文档。其中对设计影响最大的两项
是：设备可以持有提供商凭据并直接与提供商通信，以及默认电源配置为常连。


## 1. 决定一切的唯一约束

这块板子是 **8 MB Flash、没有 PSRAM**。Flash 实际上不受限；RAM 则相反。
下面每一个设计决策都源自这种不对称：

- 代码、字体、精灵图和 Web 资源都存放在 Flash 中，原地读取。
- 音频以小块流式传输，从不整体缓冲。
- 同一时刻只保持一个网络安全上下文。
- 供应商编排、密钥存储和音频转码从设备移到配套进程上。

| Resource | Confirmed value | Consequence |
| --- | --- | --- |
| MCU | ESP32-C3, single-core RISC-V, no AI/vector extensions | 无法在设备上做语音识别，也无法在设备上做回声消除 |
| Flash | 8 MB, single factory application today | 足以容纳大型中日韩像素字体和精灵图表，若重做分区表还可容纳支持 OTA 的布局 |
| RAM | Internal SRAM only, no PSRAM | 所有缓冲、栈、Wi-Fi、TLS 和 LVGL 内存池共享同一份预算 |
| Display | ST7789P3 240 × 320 RGB565 over SPI | 一个 9.6 KB 的部分绘制缓冲；没有帧缓冲 |
| Audio | ES8311, full-duplex I2S, 16 kHz 16-bit mono, microphone and speaker | 仅支持 PCM，输入输出都是 |
| Radio | 2.4 GHz Wi-Fi (STA and AP), Bluetooth 5 LE | Wi-Fi 用于供应商和桥接服务；BLE 保留 |
| Input | Three buttons on one ADC ladder, plus a hardware power button | 仅用于导航；没有键盘，没有触摸 |
| Battery | 500 mAh with a CW2017 gauge | 有线供电和电池两种配置都必须存在 |

### 1.1 What the chip cannot do

| Capability | Verdict | Reasoning |
| --- | --- | --- |
| Wake-word detection ("hey whale") | **Possible, deferred** | 小的免 PSRAM 唤醒词模型确实支持这块芯片，这一点与完整音频前端不同。之所以推迟，是因为它会在单核芯片上占用一个永久监听的麦克风、Flash 和 CPU。首个版本只在用户要求时才打开麦克风。 |
| Full-duplex barge-in with echo cancellation | **Not available** | 两个彼此独立的障碍：乐鑫的音频前端不支持这块芯片，而且它的每一种回声消除模式都额外需要 64–126 KB 的 PSRAM，而这块板子没有。可行的设计是半双工按键说话，并在播放期间硬静音麦克风。少了这道静音，扬声器输出会直接回灌给转写器。 |
| On-device speech-to-text or text-to-speech | **Not available** | 模型体积和 RAM 都不允许。在本设计中两者都是网络服务。 |
| Compressed audio decode (MP3, Opus, AAC) | **Possible but rejected as the default** | 在这块板子上实测，一条 MP3 通路占用 30–40 KB Flash，并且除解码器状态之外还想要一个 24 KiB 压缩输入缓冲；一条 Opus 通路占用 60–80 KB Flash 外加 RAM（[解码开销](../../reference/shinku-chen/audio-compression-trade-offs.md)、[流式缓冲](../../reference/phoenixzhc/network-audio-streaming-and-memory.md)）。向语音合成供应商请求线性 PCM 就能把这些全部去掉。解码器仍是刻意保留的后续选项，按独立里程碑做预算。 |
| Parallel TLS sessions | **Avoid** | 每个 Session 要花费数十 KB。语音转文字、大模型和语音合成被串行化。 |
| Full-screen animated framebuffer | **Rejected** | 完整的 240 × 320 RGB565 帧缓冲是 150 KB。渲染改为从存放在 Flash 中的图像经 9.6 KB 部分绘制缓冲完成。 |
| A captive portal that needs no typing on any phone | **Best effort only** | 门户页能否自动弹出取决于手机厂商和操作系统版本。本设计始终显示字面地址和二维码作为可靠路径。 |
| SoftAP and an active BLE connection at the same time | **Avoid** | 乐鑫的文档指出，在这颗芯片系列上 Wi-Fi 接入点加上一个已连接的 BLE 对端属于受支持但不稳定。配网流程在无 BLE 的情况下运行。 |

## 2. 系统架构

三个层级。设备是瘦终端；桥接服务负责编排。

```text
+--------------------------------------------------------------------------+
|  Tier 1 - Device (ESP32-C3, this repository's firmware)                  |
|                                                                          |
|   pixel UI (LVGL)   pet state machine   voice state machine              |
|        |                   |                    |                        |
|   BSP display         button input          BSP audio (mic / speaker)    |
|        \___________________|____________________/                       |
|                            |                                             |
|              application services:                                       |
|                wifi manager | config store (NVS) | passport link client  |
|                audio codec path | provisioning (SoftAP + captive portal) |
|                web console (HTTP server + static SPA)                    |
+--------------------------------|-----------------------------------------+
                                 |  LAN, plain HTTP, pairing token
                                 |  POST /passport/v1/audio   (PCM up)
                                 |  POST /passport/v1/message (text up)
                                 |  GET  /passport/v1/events  (long poll / stream)
                                 |  POST /passport/v1/answer  (choice down->up)
+--------------------------------|-----------------------------------------+
|  Tier 2 - Passport Bridge (user's computer, Node or Python)              |
|                                                                          |
|   provider adapters      session store        DSH session mirror         |
|   - STT  (Whisper, Gemini, ...)                 |                        |
|   - LLM  (DeepSeek, OpenAI-compatible)          | reads 127.0.0.1 only   |
|   - TTS  (see section 5.4)                      v                        |
|   PCM normalization (resample, gain, VAD)   DSH local HTTP endpoints      |
+--------------------------------|-----------------------------------------+
                                 |  HTTPS, provider credentials
+--------------------------------|-----------------------------------------+
|  Tier 3 - Providers: DeepSeek, speech-to-text, text-to-speech            |
+--------------------------------------------------------------------------+
```

图：三层架构。为保持对齐，框图内标签保留英文。

### 2.1 Two supported deployment modes

产品负责人批准了两种模式（`Q-4`）：设备可以持有提供商凭据并直接调用提供商，并且接受
明文凭据存储。因此桥接服务不是对话的前提条件。它带来的是：镜像智能体会话、无需固件
发布即可切换提供商，以及让凭据不落在一台可能丢失的设备上。

| Mode | What it needs | What it gives |
| --- | --- | --- |
| **Direct** —— 独立基线 | 设备上的提供商凭据、每个阶段一条串行化的 TLS 会话、精简的 TLS 缓冲 | 无需电脑参与即可完成一整轮语音对话 |
| **Through the bridge** —— 只要有电脑可用就推荐 | 同一网络上的桥接服务 | 工作模式、提供商灵活性、凭据留在电脑上，以及设备侧只有一种音频格式 |

塑造初稿的那些取舍依然成立；它们现在解释的是为什么桥接服务**更受推荐**，而不是为什么直连模式被排除：

| Concern | Direct | Through a bridge |
| --- | --- | --- |
| Peak RAM | 每个阶段一条 TLS 会话、共三个阶段且严格串行，外加响应缓冲，同时还要跑 UI | 三个阶段全部共用局域网上的一个纯 HTTP socket |
| Credentials | 存放在一台可能丢失的设备上，且按负责人接受的方式以明文存储 | 留在电脑上 |
| Audio format | 设备必须接受提供商输出的任何采样率 | 桥接服务统一为一种采样率 |
| Provider churn | 每次更换提供商都是一次固件发布 | 改一次桥接服务配置 |
| Latency | 少一跳 | 多一跳局域网，大约 5–20 ms —— 相比提供商时延可以忽略 |
| Without a computer | 可用 | 对话不可用；两种模式下的工作模式都需要电脑 |

由于直连模式现在位于关键路径上，它的内存开销是一等风险，而不再是脚注。它在这块板子上可行的原因：

- 各阶段严格串行，上一条连接在下一条打开之前就关闭，因此同一时刻只有一个安全上下文存活。
- TLS 输入和输出的内容长度相对默认值被调小，固件自身的缓冲在启动时一次性分配，而不是每轮分配。
- 每个响应都有显式的大小上限，每个解析器的容量都由该上限推导而来，因为一个固定 4096 字节的文档已经在这块板子上导致过一次崩溃。
- `M1` 会在存在活跃 TLS 会话的情况下实测最大连续空闲块，且早于任何提供商代码的编写。若结果为负，改变的只是这个决策，不改变这项测量。

## 3. Voice conversation pipeline

### 3.1 Interaction model

半双工、由按键主导，并具有显式可见的状态。没有常开麦克风，没有唤醒词。

```text
idle -> [OK pressed] -> listening -> (silence or [OK]) -> thinking
     -> speaking -> idle
         ^                        |
         +---- [OK] interrupts ---+   (stop playback, start a new turn)
```

| State | Screen | Microphone | Speaker |
| --- | --- | --- | --- |
| `idle` | 宠物待机动画 | 关闭 | 静音 |
| `wake` | 宠物抬起头，出现对话气泡 | 关闭 | 短的确认提示音 |
| `listening` | 宠物在聆听，电平条，"tap OK to send" | 打开，流式采集 | 静音 |
| `thinking` | 宠物思考动画，显示转写文本气泡 | 关闭 | 静音 |
| `speaking` | 回复气泡随文本增长 | 关闭 | 播放 PCM |
| `error` | 宠物沮丧，错误气泡，重试提示 | 关闭 | 可选的短错误提示音 |

在 `speaking` 时按 `OK` 会停止播放并立即进入 `listening`：这就是硬件实际能提供的打断（插话）。

### 3.2 Capture

- 格式：16 kHz、16 位、单声道 PCM，与上游音频演示已在使用的编解码通路一致。
- 音频工作任务以固定块调用 `bsp_audio_read()` 并推入一个小环形缓冲，然后交给网络工作任务。采集绝不在按键回调中或 LVGL 任务中运行。
- 一句话的结束由一个轻量的能量阈值加一个静音超时判定，两者都可配置，并设有硬性最大话长。这是一个廉价的电平检测器，不是语音模型。
- 一个用户可见的电平条由同样的分块统计驱动，因此用户能看到设备正在听到自己。
- 分块大小和环形缓冲深度是核心的 RAM 旋钮：16 KB 的采集环形缓冲在 32 KB/s 下约等于半秒音频。

### 3.3 Transport to the bridge

音频在仍在采集时就上传，而不是等用户停止之后。规范中给出两种等价的编码方式；实现按里程碑选择其一，并把选择记录在进度日志中：

1. **Streamed POST** —— `Content-Type: application/octet-stream`，分块传输编码，原始 PCM，轮次元数据放在头部。简单，且能与 `esp_http_client` 的流式写入配合。响应中携带最终的转写文本。
2. **Short-turn POST** —— 整句话放在一个请求里，带 `Content-Length`。更简单，但会把话长限制在采集环形缓冲的大小以内。

桥接服务每轮回复一个 JSON 文档，描述转写文本和回复，或返回一个错误对象。设备不需要理解来自桥接服务的 SSE；流式传输是桥接服务的内部事务。

在直连模式下，设备自己完成同样的三个阶段，每个阶段一条串行化的 HTTPS 连接：把这一句
音频上传到语音转文字端点，流式接收大模型响应，然后逐句合成。它直接解析提供商的服务端
发送事件（SSE）流。这比桥接服务路径需要更多的设备端代码，也正是设备不再算是瘦终端的
那个临界点。

### 3.4 Reply: text first, then speech

桥接服务流式接收大模型的响应，切分成可朗读的句子，并在每句完成时合成并返回该句。因此设备在第一句之后就开始说话，而不是等整个回复结束。

设备侧播放规则：

- 一个音频工作任务独占扬声器。它从句子队列取数据，并以分块方式调用 `bsp_audio_write()`。
- 播放分块大小依据 I2S DMA 预算选定（六个 240 帧的描述符，在 16 kHz 下是 90 ms 音频），这样 Flash 写入或 UI 重绘都不会造成可听见的空隙。
- 尽可能匹配采样率而不是重采样。各供应商以不同的原生采样率输出原始 PCM，而编解码器支持不止一种：规则是在本次会话中按供应商的原生采样率打开音频通路，而不是在设备上重采样。当某供应商的采样率固定且难以使用时，由**桥接服务**重采样，因此设备每个会话始终只见到一个采样率。会话中途改变采样率要走 `bsp_audio_set_format()` 及其文档化的关闭/打开开销。
- 从第一个播放字节开始直到扬声器排空为止，**麦克风通路都被静音**，且此规则不是可选项：本设备没有回声消除，因此播放期间开着的麦克风会转写进宠物自己的声音。按键说话让这条规则显得自然；但实现仍必须在代码中强制它，而不能依赖交互模型。
- 回复文本随到达逐步显示在宠物的对话气泡中。

### 3.5 Latency budget

这些是目标，不是实测值。它们存在是为了让延迟回退可见。

| Segment | Target | Notes |
| --- | --- | --- |
| Capture stop to upload start | < 100 ms | 上传与采集重叠 |
| Upload of a 4 s utterance | 100–400 ms | 约 128 KB 经 Wi-Fi |
| Speech-to-text | 400–1200 ms | 取决于供应商 |
| Language model, first sentence | 400–1000 ms | 流式 |
| Text-to-speech, first audio | 300–900 ms | 取决于供应商和音色 |
| Bridge and LAN overhead | 20–60 ms | |
| **First audio, typical** | **1.2–3.5 s** | 需求 `R-B7` 承诺 2.5 s 中位数和一个可见的思考状态 |

### 3.6 Provider abstraction

桥接服务定义三个窄接口。一个后端就是一个名称、一个 base URL、一个模型、一个密钥和一个小适配器：

```text
SpeechToText: transcribe(pcm, sampleRate, language) -> text
LanguageModel: stream(messages, systemPrompt) -> text chunks
TextToSpeech:  synthesize(text, voice, sampleRate) -> pcm chunks
```

设备只存储选中了*哪些*后端以及桥接服务地址；默认情况下它完全不存任何供应商密钥。每个后端都接受自定义 base URL，因为某些供应商无法从每个网络直接访问（假设 `A-4`）。

候选后端，将对照开放问题 `Q-2` 最终确定：

| Stage | Candidates | Selection criteria |
| --- | --- | --- |
| Speech-to-text | Whisper-style HTTP transcription, Gemini, Deepgram, AssemblyAI | 流式部分结果、中文准确率、价格、可达性 |
| Language model | DeepSeek (default), any OpenAI-compatible endpoint | 成本、中文质量、流式支持 |
| Text-to-speech | OpenAI TTS, ElevenLabs, Google, Azure, MiniMax, CosyVoice-class services | 中文自然度、16 kHz 的 PCM 输出、首字节延迟、价格 |

语音合成的决定性筛选条件是**线性 PCM 输出**。一个以编解码器能够打开的采样率返回原始 PCM 的供应商，能让解码器从固件中彻底消失；而一个只返回 MP3 的供应商，则使一个 30–40 KB 的解码器及其 CPU 开销变为必需。

这里记录三个提供商层面的约束，因为它们在纯文本测试中没有一个可见：

- 默认的大模型端点可能**默认启用推理模式**，在语音环路中会在第一个说出的词之前产生数秒静默。必须在对话通路上显式禁用它。
- 大多数语音合成供应商**不公布首字节时间**，因此时延目标 `R-B7` 无法从文档中验证。它必须用桥接服务上的测试工具实测。
- 中间转写结果现在是**需求而不是偏好**（`Q-5`）：屏幕在文本到达时显示文本。只返回最终转写
  文本的后端不能作为默认。批量转写仍作为节省成本的替代方案得到支持，只是不显示实时文本。

供应商调研、当前价格和被否决的选项单独记录在[语音后端](voice-backends.md)。

## 4. Work mode: mirroring a DeepSeek Harness session

### 4.1 What exists on the computer side

配套插件仓库 `wzqvip/dsh-app` 在 DeepSeek Harness Web 服务器上暴露本地只读 JSON 端点。以下为该仓库于 2026-10-13 的报告（**尚未对运行中的宿主验证**——验证是里程碑 `M4`）：

| Endpoint | Payload |
| --- | --- |
| `GET /dsh-pet-7340/work-status` | `{state, task, ts}`，其中 `state` 取 `thinking`、`working`、`result`、`waiting`、`success`、`error` 或 `null` |
| `GET /dsh-pet-7340/notify` | 携带单调递增 `seq` 的增量帧：轮次结束、请求批准、请求提问、代理错误 |
| `GET /dsh-efficiency/api/pending` | 待处理的代理提问：`{callId, questions:[{id, question, detail, header, options, multiSelect}]}` |
| `POST /dsh-efficiency/api/answer` | `{callId, answers:[{id, selected:[label], custom}]}` |

两条约束主导了这次集成：

1. Harness Web 服务器刻意绑定到回环地址。按它自己的文档，将其暴露到局域网等同于远程代码执行。因此设备**绝不能**直接与它通信。
2. 被记录为问题投递的那个插件端点，在该仓库中本身就被描述为降级路径。问题捕获必须按尽力而为处理，并尽早验证。

### 4.2 The bridge is the only LAN surface

```text
device --(LAN, token)--> bridge --(127.0.0.1 only)--> Harness plugin endpoints
```

桥接服务轮询回环端点（本地进程可以安全地这样做），并向局域网重新发布一套小而稳定、经过认证的协议。这让 Harness 宿主保持不暴露，并给设备一份在 Harness 内部变化时不随之改变的契约。

面向设备的协议，带版本且刻意保持极小：

| Method and path | Purpose |
| --- | --- |
| `GET /passport/v1/hello` | 桥接服务身份、协议版本、支持的特性 |
| `GET /passport/v1/state?since=<seq>` | 长轮询：当前工作状态、任务文本、待处理问题、事件序列号 |
| `POST /passport/v1/answer` | `{questionId, selected:[label], custom}` |
| `POST /passport/v1/message` | 向运行中的会话发送自由文本或转写后的指令 |
| `POST /passport/v1/audio` | 语音轮次：PCM 上行，转写文本和回复下行 |
| `POST /passport/v1/ack` | 确认一个事件序列号 |

每个请求都在头部携带配对令牌。

### 4.3 State mapping and on-device presentation

| Bridge state | Pet | Screen | Sound |
| --- | --- | --- | --- |
| `idle` (no session) | 睡觉或待机 | 主页面，时钟和电量 | 无 |
| `thinking` | 思考动画 | 三点进度，任务文本 | 无 |
| `working` | 工作动画 | 进度指示器和任务文本 | 无 |
| `waiting` | 警觉，挥手 | 问题及其选项，高亮当前选择 | 提示音 |
| `success` | 开心跳跃 | 结果摘要气泡 | 成功提示音或朗读摘要 |
| `error` | 沮丧 | 错误文本和应采取的操作 | 错误提示音 |
| `result` | 好奇 | 结果文本 | 按用户设置 |

行为规则：

- `waiting` 状态永不丢失。如果设备在睡眠或屏幕关闭，它会唤醒、响铃、显示问题，并一直保留到被回答、过期，或通过长按被忽略。
- 回答问题会确认事件序列号，这样重启后该问题不会再次显示。
- 通知按事件类别可由用户配置：静音、提示音或朗读。
- 投递默认基于轮询。电源配置决定轮询间隔；常连配置保持网络和链路存活，电池配置以投递延迟换取待机电流，并在 Web 后台中说明这一点（需求 `R-D2`）。

### 4.4 Standby default

产品负责人选择了常连配置（`Q-8`）：设备空闲时网络和桥接服务链路保持存活，因此屏幕
关闭时到达的问题会在轮询间隔内被投递，永不丢失。深度睡眠和电池配置是被推迟，而不是
被取消；上游已有的按键唤醒守卫行为得到保留，因此那条配置不经过重新设计仍然可达。

必须明说的后果：待机电流将由 Wi-Fi station 主导，而它的第一次诚实测量应当与内存数字
一起记入 `M1`。

## 5. Provisioning and the web console

### 5.1 First-run flow

```text
boot -> credentials in NVS?
          |            |
         no           yes
          |            |
          v            v
   SoftAP + QR     connect as STA
          |            |
   phone scans QR      +--> ok  -> web console on LAN address
          |            |
          v            +--> fail -> keep SoftAP, show failure and QR again
   captive portal page
          |
   SSID + password -> connect -> store -> restart networking -> web console
```

设计决策：

- **配网服务器是手写的，而不是取自 ESP-IDF provisioning manager。** 使用 SoftAP 传输的 `esp_wifi_prov_mgr` 通过 protocomm 传输 protobuf，并且需要乐鑫的手机配网 SDK。它没有配网门户页、没有 DNS 劫持、也没有 HTML 页面，因此无法满足需求 `R-E2`（"无需单独应用"）。它对本产品被否决；代价是配网流程成了我们要维护的代码。
- 设备自行开启接入点，客户端数量有上限（单用户设置页面用一到两个 station），并在 80 端口运行一个 `esp_http_server`。接入点接口和 station 接口在这颗芯片上可以共存；射频分时共用一条 2.4 GHz 通路。
- 一个小型通配 DNS 响应器对每个名字都返回接入点地址，并且对已知的连通性探测路径返回指向设置页面的重定向，而不是成功状态。有几种探测走 HTTPS，仅靠 DNS 无法重定向，因此自动弹出被视为兼容性加分项，绝不是主要路径。
- **屏幕始终同时显示二维码和字面地址。** 显示两种载荷：标准 Wi-Fi 关联载荷（`WIFI:T:WPA;S:<ssid>;P:<pass>;;`），原生手机相机可以直接对其动作；以及设置页面的 URL，用于手动加入和配置。二维码控件用 LVGL 自带的，在固定的 LVGL 版本中以每像素一位可用；一个 200 px 的码占用 LVGL 内存池约 6.4 KB。启用它只需改 Kconfig，不引入新依赖。
- 接入点名称包含一段短设备后缀，以便区分多台设备，并且接入点是受保护的而非开放的。
- 设置页面由同一个 HTTP 服务器和同一份资源包提供，与正常后台一致，但在未配网时限制为设置相关路由。
- 凭据**只有在 station 连接验证成功之后**才写入 NVS；在此之前保留先前可用的那一对，因此一个笔误不会让设备失联。报告进度时区分关联和地址分配：加入接入点与获得 DHCP 租约不是同一个事件。
- 配网完成后会拆除 DNS 响应器、HTTP 服务器和接入点。永久保持接入点和 station 接口同时在线，会在一台两者都无余量的设备上消耗堆和 socket。
- **可选子系统在网络启动之前被释放。** 这块板子上有一个实测固件在启用软接入点之前最大连续空闲块约为 13 KiB，仅通过推迟音频初始化就升至约 31 KiB（[参考](../../reference/phoenixzhc/softap-provisioning-and-resource-budget.md)）。因此配网在音频子系统未初始化的情况下运行，并且会在接入点启动前、启动后、HTTP 启动后以及手机加入后分别记录实测的最大连续空闲块。
- 配网流程中不使用蓝牙：在这颗芯片系列上，接入点加上已连接的 BLE 对端被记录为不稳定。
- 清除凭据既存在于设备上（长按路径），也存在于后台中，并且都需要显式确认。

### 5.2 Storage

| Data | Location | Notes |
| --- | --- | --- |
| Wi-Fi credentials | NVS namespace `passport.net` | 仅由配网流程写入 |
| Provider and preference configuration | NVS namespace `passport.cfg` | 一个带版本字段的 JSON 文档，小到可以原子重写 |
| Conversation history | Not persisted by default, or a bounded circular buffer | 需求 `R-B9` 禁止无界增长 |
| Web console assets | A `storage` partition (LittleFS) or embedded compressed | 见 5.4 节 |
| Pixel assets and fonts | Application `.rodata`, read from Flash in place | 无运行时拷贝 |

### 5.3 Web console API

SPA 背后的一小组 REST 接口：

| Route | Purpose |
| --- | --- |
| `GET /api/status` | 版本、运行时长、堆、最大连续空闲块、RSSI、电量、模式 |
| `GET /api/config` | 当前配置，其中**密钥被脱敏为一个布尔值** |
| `PUT /api/config` | 写入配置；密钥字段为空表示"保持不变" |
| `POST /api/test/{stt,llm,tts,bridge}` | 运行单次供应商或桥接服务自检 |
| `POST /api/wifi` | 替换凭据并重新连接 |
| `POST /api/ota` | 固件上传 |
| `POST /api/restart`, `POST /api/factory-reset` | 受限的维护操作 |

密钥是只写的：后台从不把 API Key 回显给浏览器（需求 `R-F7`）。

### 5.4 Serving the console from 8 MB of Flash

此 ESP-IDF 版本中的 HTTP 服务器没有静态文件或 gzip 辅助函数；响应处理函数都是手写的。两种可行方案：

| Approach | Flash cost | RAM cost | Tradeoff |
| --- | --- | --- | --- |
| One hand-written HTML file with inline styles and scripts, gzipped at build time and embedded in application `.rodata`, served with an explicit `Content-Encoding: gzip` | **Estimate** 8–40 KB | 无需解压缓冲，因为以压缩形式存储 | 无需改分区；浏览器必须发送 `Accept-Encoding: gzip`，由处理函数检查 |
| `storage` partition with a filesystem, assets written at build time | 资源大小加文件系统开销 | 几 KB 的读取缓冲 | 资源可以独立于应用更新；代价是一个分区、一个挂载步骤，以及又一种失败模式 |

对单个配置页面而言，嵌入是默认方案。所有内容都内联进一个文件，这样浏览器不会打开额外的 socket，并以这块板子参考笔记中一个保守的实测服务器配置作为起点：三个 socket、为 2 的 backlog、一个 6 KB 任务栈，以及小的 TCP 发送和接收窗口（[参考](../../reference/phoenixzhc/softap-provisioning-and-resource-budget.md)）。

空中升级需要改动分区表（开放问题 `Q-6`），因为当前布局只有一个应用槽，且没有 `otadata` 分区。推荐的布局，顺序与当前文件相同：

```text
nvs        0x6000
otadata    0x2000
phy_init   0x1000
ota_0      ~0x1F0000
ota_1      ~0x1F0000
storage    optional, if the console assets become a filesystem
```

以两个各约 2 MB 的应用槽再加一个 storage 分区来看，8 MB 的容量仍然宽裕。**注意：** 分区改动后用合并镜像重新烧录可能会重置已存储的 NVS 数据；分段烧录会保留它。如果升级路径要可恢复，必须刻意启用应用回滚。

## 6. Pixel user interface

### 6.1 Visual language

- 仅使用原创像素风美术。上游演示菜单、其卡片及其吉祥物禁止用于应用 UI，不得复用。
- 调色板由 DeepSeek 品牌蓝推导而来，并扩展一组小而固定的强调色，使美术保持协调。

| Role | Suggested value | Use |
| --- | --- | --- |
| Brand blue | `#4D6BFE` | 宠物身体、高光、主要操作 |
| Deep blue | `#2B3A8F` | 阴影、蓝色上的描边 |
| Ink | `#101828` | 描边、文本 |
| Paper | `#F2F5FF` | 气泡、面板 |
| Sky | `#BFD4FF` | 背景渐变条带 |
| Accent warm | `#FFB23E` | 注意、警告 |
| Success | `#39C46B` | 已完成的工作 |
| Error | `#E4573B` | 失败 |

- 文本使用像素字体，而不是抗锯齿矢量字体，使 UI 读起来是统一连贯的风格。
- 精灵图的调色板是 UI 调色板的一个更小子集；它与美术素材一起记录在
  [`art/README.md`](art/README.md) 中。

### 6.2 The pet

一只面向右侧的原创 Q 版鲸鱼，采用 DeepSeek 品牌蓝设计。
美术素材及其可复现的生成脚本位于 [`art/`](art/README.md)；第一版已经存在，正在等待验收确认。

| Property | Value |
| --- | --- |
| Logical grid | 32 × 32，以 2×（64 × 64）绘制，使像素保持明显的大颗粒感 |
| Palette | 十一种不透明颜色加透明度，以 `#4D6BFE` 为基准，该值是 DeepSeek 官方对外公布的品牌色 |
| Expressions | 待机（两帧呼吸浮动）、眨眼、好奇、聆听、思考、工作中、说话、开心、睡觉、沮丧 |
| Flash cost | 每帧以 RGB565A8 存储为 12,288 B；十一帧为 132 KiB，相对 8 MB 并不构成约束 |

宠物面向右侧，而不是照搬官方标志的朝左姿态；它的 `working`（工作中）表情会抬起尾鳍，
因为 DeepSeek 自己的 Harness 用一个动态鲸鱼尾鳍作为“智能体正在工作”的指示。美术素材是
原创的：没有描摹或修改官方剪影，同类桌面宠物的素材也不可复用，因为其许可证禁止商业用途
并要求署名。相关推理记录在
[`art/README.md`](art/README.md#reference-research-and-independence) 中。

每一帧都保持完全相同的边界，因此动画只需在一个控件上切换图像源，常驻对象数量永不改变。
精灵图由 LVGL 从 Flash 绘制，因此运行时开销是已有的绘制缓冲，而不是精灵图的大小。

尚待产出：气泡尾巴、状态条图标（Wi-Fi、电量、麦克风、工作徽标）、选项卡片边框、进度圆点，
以及二维码边框。

### 6.3 Rendering strategy

每个 LVGL 对象和 canvas 缓冲都要占用 RAM，因此设计是资源驱动的：

- 背景和精灵图是**存放在 Flash 中的 LVGL 图像描述符**（`LV_IMAGE_DECLARE` / 生成的 `lv_image_dsc_t`）。LVGL 通过其部分绘制缓冲读取它们；不存在全屏帧缓冲。
- 需要透明度的精灵图以 `RGB565A8` 存储（颜色加一个 8 位 alpha 平面）。一帧 56 × 56 的精灵图约 9.4 KB Flash，因此一段十二帧动画约 113 KB —— 相对 8 MB Flash 微不足道，在 RAM 中不占空间。
- 帧动画用定时器或 LVGL 动画在一个控件上切换图像源。对象数量保持不变。
- 不透明全屏像素场景是普通 RGB565 图像，同样从 Flash 绘制。
- 配网二维码使用 LVGL 自带的二维码控件，其 canvas 以每像素一位存储。一个 200 px 的码大约占用 LVGL 内存池 6.4 KB，外加几百字节的编码器暂存，而典型载荷属于低版本二维码。它绝不能在每个动画帧上重建。
- 每屏的 LVGL 对象数量像其他任何分配一样做预算并评审。画面冻结在半绘制的一帧且日志中没有提示，意味着内存池耗尽，而不是显示故障。

一个资源管线脚本（Python，已入库，可复现）把源 PNG 转换为 C 描述符：

```text
tools/assets/png_to_lvgl.py
  assets/pixel/<name>/frame_XX.png  ->  main/assets/<name>.c + <name>.h
  - exact palette quantization and a palette report
  - optional RGB565A8 or RGB565 conversion
  - generated header records the source file, palette, size, and byte cost
```

资源遵循仓库的资源约定：可复用的源文件和生成文件放在 `assets/` 下，许可证、转换器版本和命令记录在资源索引中。

### 6.4 Fonts

默认的 Montserrat 字体不含中日韩字形；除非编译并选中一款中日韩字体，否则中文文本会渲染成空白方框。计划如下：

- 使用开源的**像素中日韩字体**（例如 Ark Pixel 或 Fusion Pixel 系列），正文用 12 px，状态条用更小的字号。
- 用固定的 LVGL 字体转换器生成一个**子集**，覆盖 UI 所需的中文和英文字符，加上可能出现在模型回复中的字符；如果 Flash 开销可接受，也可使用完整的常用字符集。
- 以程序化方式验证字形覆盖（构建期的字符清单检查），并另行在设备上验证，因为构建成功并不能证明某个字形能够渲染。
- 遵循仓库的中文字体检查清单。字体资源放在 `assets/fonts/` 下，并记录许可证和转换器命令。

### 6.5 Screens

```text
 home / pet                     conversation                  work progress
+----------------------+      +----------------------+      +----------------------+
|  (#)  wifi  bat  mic |      |  (#)                 |      |  (#)  WORK     45%   |
|                      |      |      [ bubble ]      |      |                      |
|        (o o)         |      |   "what should I..." |      |  refactor parser     |
|       (  ^  )        |      |                      |      |  [####------] steps  |
|        whale         |      |   [ bubble ]         |      |                      |
|                      |      |   "sure, I can..."   |      |  (pet working)       |
|   [ OK ] hold: menu  |      |                      |      |                      |
+----------------------+      +----------------------+      +----------------------+

 agent question                 provisioning + QR            web console notice
+----------------------+      +----------------------+      +----------------------+
|  AGENT ASKS          |      |  SETUP WIFI          |      |  WEB CONSOLE         |
|  Which branch?       |      |   +------------+     |      |  http://192.168.    |
|  > main              |      |   |  QR CODE   |     |      |  4.1                |
|    feature/x         |      |   +------------+     |      |                      |
|    custom...         |      |  1 join Passport-3F  |      |  open on your phone  |
|  up/down  ok confirm |      |  2 open 192.168.4.1  |      |  to configure        |
+----------------------+      +----------------------+      +----------------------+
```

图：六个屏幕的布局草图。为保持对齐，框内标签保留英文。

每个屏幕在被删除前都必须停止自己的定时器、动画和工作任务，非 LVGL 上下文在做短暂 UI 更新时必须持有 `bsp_lvgl_lock()`。

## 7. Memory budget

下面所有数字都是**待实测的估算**。两个实测锚点界定了问题范围。乐鑫针对这颗芯片给出的自身数据是：空项目可用堆约 332 KB，Wi-Fi station 运行时约 219 KB，Wi-Fi station 加一条 TLS 连接时约 210 KB，每多一个并发 Session 要花费数十 KB；其文档中的 RAM 削减设置可把 station 加 TLS 的数字提高到接近 300 KB。另有一项独立数据：这块板子上一个带 80 KB LVGL 内存池和丰富 UI 的应用，稳定在约 131 KB 的可用系统堆。缩减 LVGL 内存池会把内存转移到系统堆，而 Wi-Fi 和 TLS 正需要在那里。

| Item | Estimated cost | Kind |
| --- | --- | --- |
| LVGL internal pool (`CONFIG_LV_MEM_SIZE_KILOBYTES`) | 32 KB | 静态 |
| LCD partial draw buffer (240 × 20 RGB565) | 9.6 KB | 静态 |
| I2S DMA, both directions | ~6 KB | 静态 |
| Capture ring | 16 KB | 堆，一次性分配 |
| Playback ring | 16 KB | 堆，一次性分配 |
| Wi-Fi station stack | 40–60 KB | 堆 |
| One TLS session, tuned buffers | 25–35 KB | 堆，位于关键路径上：直连模式以及任何使用 TLS 的桥接服务都需要它 |
| HTTP server plus one response buffer | 8–12 KB | 堆 |
| Task stacks (UI, audio, net, input, HTTP) | ~25 KB | 静态/堆 |
| Sprites, fonts, SPA | Flash only | Flash |

因此峰值系统堆由 Wi-Fi、一个安全上下文和音频缓冲主导：桥接模式下约为 130–160 KB，
当设备自行与提供商通信时再增加约 25–35 KB。**总的可用堆仍然不是约束瓶颈。** 这块板子
的实测参考是：最大连续空闲块在软接入点之前约为 13 KiB，在推迟音频初始化之后约为 31 KiB。配网、TLS 和一次 JSON 解析各自都需要一个大块连续分配，因此本项目首先为最大连续空闲块做预算，其余一切排在其次。

### 在本板上的实测

上文的估算在存在实测值之处已被替换。以下数据来自运行在设备上的应用：

| 状态 | 可用堆 | 最大连续空闲块 |
| --- | --- | --- |
| 空闲、射频未开 | 154,624 B | 112 KiB |
| 已关联 Wi-Fi、无 TLS | 163,580 B | 112 KiB |
| **TLS 会话活跃且有一次服务商请求在途** | **80,376 B** | **68 KiB** |

那个真正的约束是存在的，而且它就是最大连续空闲块：**一个活跃的 TLS 会话会吃掉约 44 KiB 的
最大连续空闲块，只剩 68 KiB。** 今后每一个缓冲区都要拿这个数字衡量；也正是它决定了音频
流水线必须用小分块流式传输 PCM，而不是把一整轮对话暂存在 RAM 里。那一次服务商往返含握手共
实测 1,730 ms，而时延预算是按三个串行阶段首段音频 2.5 s 来假设的。

由此得出的常备规则：

- 在启动时以及每个主要状态转换时记录 `lv_mem_monitor()`、`esp_get_free_heap_size()` 和最大连续空闲块。
- 在反复进入/退出循环中跟踪**历史最小值**的可用堆和最大块，而不是最后一次采样。
- 每个 JSON 解析器的容量都从可接受的响应上限推导，并在解析之前拒绝过大的响应。这块板子上有一个实测固件崩溃，就是因为一个固定 4096 字节的文档遇到了 6971 字节的响应（[参考](../../reference/phoenixzhc/network-audio-streaming-and-memory.md)）。
- 热缓冲在启动时一次性分配，而不是每轮分配，这样长会话不会把堆碎片化成无法使用的零碎块。
- 延迟初始化音频：它是最大的可选消耗者，在配网期间或待机时都不需要它。

## 8. Firmware module layout

板级硬件留在 `components/bsp`；其余都是 `main` 中的应用代码，工作在 `feature/*` 分支上。

| Module | Responsibility |
| --- | --- |
| `main/passport_app.c` | 启动序列、顶层状态机、屏幕路由 |
| `main/passport_pet.c` | 宠物状态、表情选择、动画调度 |
| `main/passport_voice.c` | 语音状态机、采集与播放协调 |
| `main/passport_link.c` | Passport Link 客户端：状态轮询、事件、回答 |
| `main/passport_wifi.c` | station 和接入点生命周期、重连策略 |
| `main/passport_prov.c` | SoftAP 配网、二维码载荷、配网门户页路由 |
| `main/passport_web.c` | HTTP 服务器、REST API、资源服务 |
| `main/passport_config.c` | 配置模式、NVS 持久化、默认值、脱敏 |
| `main/passport_ui_*.c` | 屏幕和控件，每个屏幕组一个文件 |
| `main/assets/*.c` | 生成的像素资源和字体描述符 |
| `tests/` | 每个状态机、协议编解码和布局计算的宿主测试 |

可测试逻辑 —— 语音状态机、链路协议编解码、配置编解码、问题渲染、待机计时和二维码载荷构造 —— 必须写成无需 ESP-IDF 或 LVGL 即可在宿主机上编译运行，遵循现有的 `tests/` 模式。

## 9. Security model

| Asset | Threat | Control |
| --- | --- | --- |
| Provider API keys | 设备丢失、日志外泄 | 按产品负责人接受的方式（`Q-4`）以明文存储在设备的 NVS 中；这是刻意的设备丢失风险。仍然绝对成立的控制项是：绝不记录日志、绝不由后台回显、绝不提交到仓库 |
| Harness session | 局域网攻击者触及代理宿主 | Harness 服务器留在回环地址；设备只能看到桥接服务那套窄的、经过认证的 API |
| Bridge API | 未授权的局域网客户端 | 每设备配对令牌，配网期间显示在设备屏幕上，每个请求都必须携带 |
| Wi-Fi credentials | 日志或仓库泄露 | 只存储在 NVS 中；仓库门禁会扫描密钥 |
| Voice audio | 局域网上的窃听 | 桥接服务预期在可信网络上使用；协议默认在局域网上为纯 HTTP，到供应商为 TLS。未来的选项是桥接服务上启用 TLS，代价是 RAM |
| Web console | 未授权的配置更改 | 仅绑定局域网；可选的后台密码；密钥从不回显 |

在这颗芯片上可以通过烧入 eFuse 的 HMAC 密钥启用加密 NVS，这既不需要单独的密钥分区，也不需要完整的 Flash 加密。它是后续里程碑的一个候选加固步骤；必须实测和验证而不是假定，并且烧入 eFuse 密钥不可逆，因此这是设备所有者的一次刻意单向决定，绝不能作为构建默认值。

直白地说，NVS 加密能买到什么、不能买到什么：它保护已存储的密钥不被读取 Flash 芯片或被导出的镜像。它不能防护有人用串口线接触设备、从同一接入点提供恶意页面，或密钥经局域网上的纯 HTTP 离开设备。此处真正见效的控制手段正是已经选定的那些：密钥默认放在桥接服务上、密钥绝不记录日志、每个 HTTP 输入都有边界、设置用接入点受保护且短时存在，以及先前可用的凭据在新配对失败后仍然保留。

## 10. Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Peak RAM does not fit with Wi-Fi, one TLS session, and audio simultaneously | 对直连模式是阻塞项 | 在 `M1` 中、编写提供商代码之前，带活跃 TLS 会话实测最大连续空闲块；串行化各阶段；调小 TLS 内容长度；热缓冲一次性分配；LVGL 内存池保持小；经桥接服务绕行仍然是完全避开设备端 TLS 的一条途径 |
| Plain-text credential storage on a device that can be lost | 谁拿到设备谁就拿到密钥 | 已由产品负责人接受；缓解措施是从不在设备屏幕上显示机密、从不记录它，并在 Web 后台中说明该风险 |
| Harness question capture is the degraded path described by the plugin | 工作模式无法回答问题 | 先在 `M4` 中验证；如果不可用，回退到确实可用的批准/问题来源，或增加一个专用桥接服务插件 |
| Captive portal behaves differently across phone vendors | 用户无法配网 | 始终显示字面地址和指向该页面的二维码；支持手动输入 |
| Largest free block too small to start the access point or TLS | 只有等其他功能都起来之后，配网或直连模式才会失败 | 推迟音频初始化、先释放可选子系统、热缓冲一次性分配，并在每个阶段边界记录最大连续空闲块 |
| SoftAP combined with a connected BLE peer | 射频行为不稳定 | 配网流程仅使用 Wi-Fi |
| Text-to-speech provider returns only compressed audio | 增加一个解码器及其 RAM | 优先选择输出 PCM 的供应商；把解码器当作单独做预算的里程碑 |
| CJK pixel font size | Flash 和查找开销 | 子集生成、CI 中的覆盖检查、设备上验证 |
| LVGL pool exhaustion with animation | 屏幕冻结且日志无提示 | 为每屏做对象数量预算、监控内存池、让动画期间对象数量保持恒定 |
| Provider availability and cost changes | 功能中断 | 三个窄接口和每后端 base URL；不硬编码任何供应商 |
| A default reasoning mode produces dead air before the first spoken word | 语音环路让人感觉是坏的 | 在对话通路上显式禁用推理，并验证首音频时间 |
| Text-to-speech time to first byte is unpublished | 延迟目标无法从合约验证 | 在选择默认音色之前用测试工具实测多家供应商 |
| Upstream divergence | 合并冲突 | 应用工作放在 `feature/*`；fork 的 `main` 只保留 readme 对和 `docs/assets/` |

## 11. Validation and delivery

- 每个里程碑都运行 `./tools/validate.sh --static` 并报告其结果。
- 固件里程碑额外运行 `./tools/validate.sh --firmware`。
- 宿主测试覆盖状态机、协议编解码、配置编解码和布局计算。
- 设备验收是独立的显式步骤，配有按里程碑的检查清单；构建通过绝不会被报告为设备验证。
- 每份报告都分别陈述 Build、Host tests、Device tests 和 Unverified 字段，并且[进度日志](progress-log.md)记录证据。
