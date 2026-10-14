<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# Fork 素材与设计说明

本目录存放属于本 fork 而非上游项目的内容：产品设计、架构说明、需求和项目专属素材。上游除一个占位文件外保持本目录为空，此处内容不会提议回上游。

## 本目录的规则

- 每份维护中的文档都以配对的英文 `.md` 和简体中文 `.zh_CN.md` 文件存在，并带有互惠的语言切换，与仓库其余部分完全一致。
- 只有 fork 私有的材料才属于这里。任何能帮助任意 AI Passport 使用者的内容都应改为放到上游。
- 切勿在此放置凭据、令牌、设备二维码密钥或未脱敏的日志。
- 当新增、移动或删除文档时，在同一次变更中更新下方索引和根 [readme](../../README.md)。

## 索引

| 文档 | 内容 |
| --- | --- |
| [需求](deepseek-passport/requirements.md) | 产品负责人提出了什么，拆解为需求、假设、验收标准和待决问题 |
| [架构](deepseek-passport/architecture.md) | 硬件约束、内存预算、固件与中继架构、语音流水线、智能体集成、配网、Web 后台，以及像素 UI |
| [语音后端](deepseek-passport/voice-backends.md) | 语音流水线决策背后的提供商调研，以及被否决的设计 |
| [路线图](deepseek-passport/roadmap.md) | 里程碑、退出标准和验证计划 |
| [进度日志](deepseek-passport/progress-log.md) | 带日期的决策、证据、验证状态和待决事项 |

## 相关

- 根 readme：[README.md](../../README.md)
- Fork 工作流以及哪些内容属于 fork 自有：[fork 指南](../fork-guide.md)
- 上游项目概览：[docs/README.md](../README.md)
