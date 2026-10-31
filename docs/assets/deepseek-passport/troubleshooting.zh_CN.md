<p align="right">
  <a href="troubleshooting.md">English</a> · <strong>简体中文</strong>
</p>

# 故障排查

主人真正会报告的症状,以及如何用设备自己的控制台把这些原因区分开。这里每一条都来自**这块板子上的真实观察**,
不是想象;日期和原文都照抄,后来的人才能判断是不是同一件事。

用 115200 波特率的串口控制台(Windows 上是 `tools/passport-console.ps1`)。**只有它会说出"为什么"**。

## "按住 OK 没反应"

三个互不相同的原因,用两条命令就能分清。

### 1. 设备不在"能说话"的那一屏

```text
ui screen
```

显示 `screen work` 或 `screen settings`,说明说话键被绑定成了那一页自己的含义——在工作屏上它是"确认选项"。
长按 OK 回到宠物。如果 `ui screen` 显示的是 `passport` 而按键仍然没反应,继续往下看。

### 2. 录音被当作静音丢掉了

```text
voice status
```

```text
state=idle mic=closed turns=0 gate=3694 (twice the measured room) last="no turn yet"
```

`gate` 远高于一个人能发出的音量,就是原因。2026-10-31 实测:启动预热测到房间峰值 1,847,阈值取峰值两倍
= 3,694,于是正常说话被丢掉。该日期之后的固件改用房间**平均值**推导阈值,并夹在 300..1500;同一个读数会
变成 600 左右。要手动钉住:

```text
voice gate 600      # 固定阈值
voice gate 0        # 永不判定为静音,用于台架测试
```

设备会把原始测量值 `room rms=… peak=…` 打印在阈值旁边,于是"产生错误阈值的两个数"是**看得见的**,
而不是靠推断。

### 3. 没地方可发,或者桥接拒绝了

```text
config list
work client
```

2026-10-31 在上一轮会话交付的那台设备上实测:

```text
  stt_base_url       (empty)      <- 没有配置直连转写
  bridge_url         http://192.168.2.183:8080
work client: bridge=http://192.168.2.183:8080 polls=14 failures=14 last="fetch failed: ESP_ERR_HTTP_CONNECT"
```

`stt_*` 为空时,每一个回合都走桥接。`ESP_ERR_HTTP_CONNECT` 表示主机或端口不可达——这次是电脑的地址变了,
存下来的那个指向了空。把 `bridge_url` 改成电脑当前地址后,同一条命令变成:

```text
work client: ... last="fetch returned HTTP 401"
```

`401` 是**进展,不是新故障**:设备已经到达桥接,而桥接因为 `bridge_token` 缺失或不一致拒绝了它。
早于认证工作的固件根本不发这个令牌,所以新版桥接对它一律回 `401`;更新固件并在两端设置同一个令牌即可。

## 读状态

| 现象 | 含义 |
| --- | --- |
| `ESP_ERR_HTTP_CONNECT` | 地址/端口错、桥接没跑,或不在同一个网络 |
| `HTTP 401` | 能到达,但令牌缺失或不一致——两端都查 |
| `/work-status` 返回 `HTTP 404` | 能到达且已认证;伴侣程序还没发布任务 |
| `gate` 上千 | 阈值高于真人说话音量,见上文 |
| `last="capture failed: …"` | 麦克风通路失败,不是网络 |

## 不打开网页也能改配置

```text
config set bridge_url http://<电脑>:8080
config set bridge_token <令牌>
config save
```

`config save` 单独一步是有意的:`config set` 只改运行时配置,一个笔误在你确认之前不会变成存下来的那份。
