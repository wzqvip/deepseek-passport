<p align="right">
  <a href="serial-cli.md">English</a> · <strong>简体中文</strong>
</p>

# 串口控制台

USB Serial/JTAG 端口上的命令控制台，让主机能够**读取实时屏幕**并**驱动设备**。它存在的意义，是让智能体或人都能核实屏幕上实际绘制的内容，而不是从源代码去推断。

实现位于 [`main/passport_cli.c`](../../../main/passport_cli.c)。在显示、LVGL 和应用输入通路就绪之后，由 `passport_cli_start()` 启动。

## 屏幕与控制台如何保持一一对应

这里没有第二套元素注册表。控件身份来自 **LVGL 的命名对象**，而名称派生自用户实际读到的文本：

```c
lv_obj_set_name(s_card, "audio");   /* the card labelled "Audio" */
```

应用从它绘制的同一个字符串派生出每个名称（[`slugify()`](../../../main/main.c) 会将其转为小写、以连字符连接并去掉其余部分），因此 `Low Power` 变成 `low-power`，`Wi-Fi` 变成 `wi-fi`。每条 `ui` 命令都把这些名称当作从当前活动屏幕开始的斜杠分隔路径来寻址。**为对象打印出的名称，正是命令所接受的名称**，所以列表不会偏离屏幕上显示的内容。

需要 `CONFIG_LV_USE_OBJ_NAME=y`（已在 `sdkconfig.defaults` 中设置）。

## 命令

| 命令 | 作用 |
| --- | --- |
| `ver` | 项目名称、应用版本、构建日期与时间、ESP-IDF 版本 |
| `app` | 运行时长、电池荷电状态、键盘电压、堆、最大空闲块、LVGL 池 |
| `mem` | 堆、最大空闲块和 LVGL 池，不含其余项 |
| `ui tree [depth]` | 导出当前活动屏幕的对象树：路径、类型、几何、`hidden` 和标签文本。默认深度 6 |
| `ui names` | 将每个对象列为名称路径、其类型和屏幕上的文本 |
| `ui get <name-or-path>` | 打印一个对象的几何、标志和文本 |
| `ui tap <name-or-path>` | 发送一个合成的 `LV_EVENT_CLICKED`；如果指定对象是纯标签，则由最近的可点击祖先接收 |
| `ui key <up\|down\|ok> <press\|click\|double\|long\|release>` | 注入一次按键事件 |
| `ui screen` | 活动屏幕名称、尺寸和子对象数量 |
| `reboot` | 重启设备 |
| `audio status\|tone [ms]\|loopback [ms]\|mute-check` | 当前哪个子系统占用音频通路；播放 1 kHz 单音；录一段再放回并给出电平读数；验证播放持有通路期间录音会被拒绝 |
| `wifi status\|scan\|connect [ms]\|disconnect` | station 状态与地址；列出周边接入点（不需要任何凭据）；用已存凭据关联；断开关联 |
| `config list\|get <字段>\|set <字段> <值>\|save\|reset` | 配置：列出全部字段（密钥只显示已设置/为空）、在内存中改一个字段、持久化、恢复默认 |
| `help` | ESP-IDF 控制台自带的命令列表 |

## 针对它编写脚本前值得了解的两条契约

**只有用户设置过名称的对象才可寻址。** 未命名对象仍会出现在 `ui tree` 和 `ui names` 中，但带有一个合成的 `kind_index` 标签，任何命令都不接受它。请为你想要寻址的控件命名。

**`ui key` 是驱动导航的可靠方式；`ui tap` 只在应用注册了点击处理函数的地方有效。** 按键注入进入应用自己的输入队列，因此它走的是与物理按键相同的路径，并驱动状态机。如果控件没有处理函数，合成点击不会做任何事——上游硬件测试演示就是这种情况，它的菜单由按键驱动。

## 为什么没有使用 LVGL 的名称解析器

在本项目锁定的 LVGL 版本中，`lv_obj_get_name_resolved()` 会在**未命名的根屏幕**上触发加载访问故障。只有当活动屏幕具有用户设置的名称时，它看起来才正常工作。因此控制台直接读取 `lv_obj_get_name()`，为未命名对象构建合成标签，并用自己的递归遍历解析路径。不要在未于硬件上重新测试未命名屏幕这一情形之前，重新引入 `lv_obj_get_name_resolved()`、`lv_obj_get_child_by_name()` 或 `lv_obj_find_by_name()`。

## 完整示例

```text
passport> ver
project   : FoloToy-AI-Passport
version   : 4d05296
built     : Oct 14 2026 00:58:13
esp-idf   : v5.5.3

passport> ui screen
screen menu  240x320  children=39

passport> ui names
menu                               obj        (active screen)
display                            obj
display/lv_label_0                 label      "Display"
audio                              obj
audio/lv_label_0                   label      "Audio"

passport> ui key down click
ui key: injected down click
passport> ui key ok click
ui key: injected ok click
passport> ui screen
screen obj_0  240x320  children=27
passport> ui key ok long
ui key: injected ok long
passport> ui screen
screen menu  240x320  children=39
```

同一次会话测得的数值记录在[进度日志](progress-log.md)中。

## 扩展它

在 [`passport_cli.c`](../../../main/passport_cli.c) 中用 `register_command()` 添加一条命令，并在同一次变更中向上方的表格添加一行。任何新命令都适用两条规则：

- 在每次 LVGL 访问前后持有 `bsp_lvgl_lock()`。控制台运行在自己的任务中，而 LVGL 不是线程安全的。
- 通过应用自己的路径接触应用——输入队列、状态机 getter——而不是调用另一个任务的数据结构。

控制台不是必需的子系统：如果它启动失败，设备会记录一条警告并照常运行。
