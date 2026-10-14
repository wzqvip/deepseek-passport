<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

<h1 align="center">DeepSeek Passport</h1>

<p align="center">
  <strong>一只可以对话的口袋 DeepSeek 伙伴：它看着你的 Agent 干活，按键就会应你。</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/status-firmware%20verified-16a34a?style=flat-square" alt="状态：固件已在真机验证">
  <img src="https://img.shields.io/badge/target-ESP32--C3-2563eb?style=flat-square" alt="目标：ESP32-C3">
  <img src="https://img.shields.io/badge/upstream-FoloToy%2Fai--passport-14b8a6?style=flat-square" alt="上游：FoloToy ai-passport">
  <img src="https://img.shields.io/badge/License-MIT-64748b?style=flat-square" alt="MIT 许可证">
</p>

---

**DeepSeek Passport** 把开源的 **[FoloToy AI Passport](https://github.com/FoloToy/ai-passport)**
可穿戴设备改造成一只 DeepSeek 主题的随身宠物：一只像素风的 Q 版小鲸鱼住在
240 × 320 的屏幕上，用板载麦克风听你说话，用板载扬声器回答你，同时充当电脑上
DeepSeek Harness（DSH）会话的遥控器与通知面板。

**硬件不改动。** 本仓库的全部内容都是在上游板级支持包之上叠加的固件、配套桥接服务
与设计文档。

> [!IMPORTANT]
> 应用**已经实现并在真机上运行**。上游 demo 的菜单、屏幕与视觉外壳已经删除;设备开机进入的是本项目自己的
> 界面。每个里程碑都在串口控制台上于真机验证过,而**没有**验证的部分会被明确列出而不是含糊带过——
> 见[交付说明](docs/assets/deepseek-passport/handover.zh_CN.md)与
> [进度日志](docs/assets/deepseek-passport/progress-log.zh_CN.md)。
>
> 还差一块没写的是**配套桥接服务**——那是跑在电脑上的程序,不是固件。它的契约已经完整,而且设备是对着
> 它的一份替身实现验证过的。

## 它能做什么

| 能力 | 用户可见行为 |
| --- | --- |
| **随身宠物** | 一只原创的 Q 版像素小鲸鱼待机、眨眼、呼吸、响应按键，并弹出对话气泡。它就是主页面，而不是菜单。 |
| **实时语音对话** | 按键说话，宠物用屏幕文字加语音回答。语音转文字、大模型、语音合成是三个可独立配置的后端。 |
| **工作模式** | 电脑上的 DSH 会话运行时，设备同步显示进度与选项，用 `上` / `下` / `确定` 选择并回传答案，一轮结束时用提示音或语音播报结果。 |
| **配网与 Web 后台** | 没有已保存的网络时，设备自建热点并在屏幕上显示二维码，提供配网页填写 Wi-Fi；此后提供本地 Web 后台配置 API Key、服务商、音色、亮度、音量与固件升级。 |
| **待机** | 空闲熄屏、静默待机、按任意功能键唤醒；真正的开关机仍由独立的硬件电源键负责。 |

## 硬件概览

板子是标准 AI Passport；下表是**已确认的板卡事实**，不是目标值。

| 项目 | 参数 |
| --- | --- |
| 主控 | ESP32-C3，单核，8 MB Flash，**无 PSRAM** |
| 可用内存 | 仅内部 SRAM；显示、LVGL、音频 DMA、Wi-Fi、TLS 全部争抢同一块内存 |
| 屏幕 | ST7789P3，240 × 320 竖屏，RGB565，SPI |
| 按键 | `上` / `下` / `确定` 共用一个 ADC 分压引脚（GPIO0），另有独立硬件电源键 |
| 音频 | ES8311 codec，I2S 全双工，16 kHz 16 bit 单声道录放，带麦克风与扬声器 |
| 无线 | 2.4 GHz Wi-Fi（STA 与 AP）与 蓝牙 5 LE |
| 电池 | 500 mAh 电芯，CW2017 电量计，USB Type-C 充电 |
| NFC | 无源 NTAG213 标签，主控无法访问 |

「8 MB Flash 且无 PSRAM」是本项目最重要的设计约束。正因为这条约束，架构选择**流式
而不是整段缓冲**音频、在设备端只处理 **PCM 而不做压缩音频解码**、并把服务商编排
下沉到配套桥接服务。完整推理见[架构文档](docs/assets/deepseek-passport/architecture.zh_CN.md)。

## 设计记录

Fork 私有的产品设计放在 `docs/assets/`，上游有意让该目录保持为空。以下文档是本项目的
权威来源。

| 文档 | 内容 |
| --- | --- |
| [需求记录](docs/assets/deepseek-passport/requirements.zh_CN.md) | 产品负责人的原始描述，拆解为编号需求、假设、验收标准与待确认问题 |
| [架构设计](docs/assets/deepseek-passport/architecture.zh_CN.md) | 硬件约束、内存预算、固件与桥接架构、语音链路、DSH 联动、配网、像素 UI 与服务商配置 |
| [路线图](docs/assets/deepseek-passport/roadmap.zh_CN.md) | 里程碑 `M0`–`M7` 的交付物、进入/退出标准与验证计划 |
| [进度日志](docs/assets/deepseek-passport/progress-log.zh_CN.md) | 按日期记录决策、证据与未决事项 |
| [交付说明](docs/assets/deepseek-passport/handover.zh_CN.md) | 怎么烧与怎么用、它需要什么桥接服务，以及一份明确的「已验证 / 未验证」清单 |
| [桥接协议](docs/assets/deepseek-passport/bridge-protocol.zh_CN.md) | 设备与其配套桥接服务之间的契约 |
| [串口控制台](docs/assets/deepseek-passport/serial-cli.zh_CN.md) | 用来验证一切的控制台，以及它遵守的两条契约 |
| [语音后端](docs/assets/deepseek-passport/voice-backends.zh_CN.md) | 语音流水线决策背后的提供商调研 |

## 仓库结构与 Fork 约定

本仓库是上游的 fork。只有两个位置属于 fork 自己，其余全部跟随上游
`FoloToy/ai-passport`，由同步工作流更新。

```text
README.md / README.zh_CN.md        fork 自有项目说明（本文件）
docs/assets/                       fork 自有设计文档与素材
docs/                              上游文档，保持同步
components/bsp/                    上游板级支持包
main/                              上游硬件测试示例，功能分支上会被应用替换
skills/                            上游 AI Agent 技能
sdkconfig.defaults、partitions.csv 上游构建基线
```

应用固件在 `feature/*` 分支上开发；fork 的 `main` 与上游保持一致。见
[Fork 指南](docs/fork-guide.zh_CN.md)。

## 构建

固件目标为 ESP-IDF 5.5.3 / ESP32-C3。先按
[环境准备指南](docs/development/engineering/environment-setup.zh_CN.md) 安装工具链，
再使用仓库统一的验证入口：

```bash
./tools/validate.sh --static    # 仓库检查 + 宿主测试
./tools/validate.sh --firmware  # ESP-IDF 构建 + 合并镜像校验
./tools/validate.sh             # 完整门禁
```

构建成功不等于硬件验证。构建结果、宿主测试结果与真机测试结果必须分别报告。

## 相关项目

| 项目 | 作用 |
| --- | --- |
| [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) | 上游开源可穿戴 AI 平台：硬件事实、BSP 与示例基线 |
| [wzqvip/dsh-app](https://github.com/wzqvip/dsh-app) | DSH 插件包，向外提供 Agent 提问与工作进度；设备可用的工作状态与通知接口在这里 |
| [DeepSeek Harness](https://github.com/deepseek-ai) | 本设备所镜像的 Agent 宿主 |

## 已托管项目

| 项目 | 分支 | 状态 |
| --- | --- | --- |
| DeepSeek Passport 固件 | `feature/deepseek-passport`（已实现，并在真机验证过） | 进行中 |

## 许可证

MIT，继承自上游项目。见 [LICENSE](LICENSE)。产品、品牌与硬件事实仍归上游项目所有；
见[品牌与产品说明](docs/brand/brand-and-product.zh_CN.md)。
