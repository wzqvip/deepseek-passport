<p align="right">
  <a href="bridge-protocol.md">English</a> · <strong>简体中文</strong>
</p>

# Passport 桥接协议

当前契约，更新于 2026-10-31。实现为
[passport_bridge.py](../../../tools/passport_bridge.py) 与设备的工作/语音客户端。
桌面[伴侣 CLI](../../../tools/passport_work_companion.py) 能发布真实任务状态并取回答案，
但**不会自动镜像现有 DeepSeek Harness Web 会话**。尚未验证该会话的安全订阅/回答公开接口。

## 认证与部署

所有端点要求 `Authorization: Bearer <bridge_token>`。设备 `bridge_token` 与进程环境
`PASSPORT_BRIDGE_TOKEN` 配置相同秘密；不要写入源码或日志。中继默认监听 `127.0.0.1`。
服务实体设备时，需显式用 `--host` 绑定可达的可信局域网地址，并设置设备 `bridge_url`。
普通 HTTP 无法保护令牌、任务和音频免受网络旁观者读取：仅在可信局域网使用，或配置 HTTPS 前端。
不要为了设备连接而把 agent Web 服务暴露到局域网。

`--insecure-local` 仅允许本机 loopback 测试免认证；`--fake` 不绕过认证。
提供商凭据另外通过 `PASSPORT_BRIDGE_KEY` 传入，不会自动安装常驻服务。

## 工作状态与回答

`GET /work-status` 返回扁平 JSON；`404` 表示尚无任务发布，不是网络故障。
伴侣通过 `POST /work-status` 发布：

```json
{"task":"deploy","state":"waiting","question":"Which branch?",
 "option0":"main","option1":"develop","selected":0}
```

字段包括 `task`、`state`（`idle/running/waiting/done/failed`）、可选 `progress`（0–100）、
`question`、连续的 `option0` 至 `option3` 及初始 `selected`。发布问题会生成 `question_id`。
设备重复轮询相同问题时保留本地选择与已回答状态；身份、问题或选项改变时才视为新选择。
通知不会抢占设置/配网页，也不会中断持有中的说话手势。

实体确认键只排队给 worker，不在输入线程联网：

```json
{"question_id":"<opaque-id>","answer":"develop"}
```

`POST /answer` 用 JSON 转义保留完整选项标签。同问题同答案重试幂等；冲突或过期答案返回 `409`。
网络失败后，同一当前问题允许重试。旧文件文档没有 ID 时仍支持仅标签答案，但不具备问题代际保护。

`GET /answers?question_id=<id>` 在等待时返回 `ready: 0`，有答案时返回 `ready: 1` 与 `answer`。
结果仅在内存保留最近 64 条，重启即丢失。中继默认不再持久写出回答或转写文本日志。

伴侣命令示例（令牌从环境读取）：

```text
python tools/passport_work_companion.py --help
python tools/passport_work_companion.py status --help
python tools/passport_work_companion.py ask --help
python tools/passport_work_companion.py answer --help
```

这些接口可由实际桌面程序显式发布任务。自动 DSH 镜像、可靠持久投递、对该会话的自由文本语音指令、
远程转发仍未完成；提供 CLI 不代表这些功能已经实现。

## 语音上传

`POST /voice` 接收有界小端 PCM16、单声道、16kHz，包括分块上传。
设备完整写出 framing，检查 HTTP `202` 并解析完整响应，保存其 32 位十六进制 `turn_id`。
每轮有独立缓冲、事件和结果；重叠上传返回 `409`，不会混音频。中继最多保存 16 轮。
上传上限为 60 秒／1,920,000 字节；非法 framing、超长和超时均拒绝。
本地静音判断发生在桥接音频上传**之后**，不是传输之前。

`GET /voice/transcript?turn_id=<id>` 返回同一 `turn_id`、整数 `ready`（0/1）及完成的 `text`。
设备校验 ID，在有界预算内轮询并显示思考状态。空文本不发送语言模型。
中继兼容无 ID 的旧读取，但当前固件要求明确回合；固件和中继需配套更新。

## 语音输出

`GET /speak?text=<percent-encoded-UTF8>` 返回小端 PCM16、单声道、16kHz。
录音和朗读统一由 speech worker 串行处理。输出有界，奇数字节 PCM 与传输失败会报告错误；
部分已播放不能把失败变成成功。设备仅朗读显示回复的有界前缀，不是完整回答流式播放。
提交朗读请求表示**已排队**，不表示已经听到声音。

设备直连 STT/TTS 使用各自配置，见[语音后端](voice-backends.zh_CN.md)。桌面工作集成仍需要中继，
语音桥接则作为可选路径保留。本机 fake-provider 集成测试只证明协议行为，不证明真实服务、麦克风质量、
局域网可达性或真机验收。
