<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

### 应用中文子集字体

| 文件 | 内容 | 用途与来源 |
| --- | --- | --- |
| [`fonts/cjk-inventory.txt`](fonts/cjk-inventory.txt) | 8,273 个码点，UTF-8 | 字体生成所依据的字符清单：由 GB2312/CP936 的首字节与尾字节范围 `0xA1A1`–`0xF7FE` 推出的 8,178 个字符，加上可打印 ASCII。它是**推导**出来的而不是从某个列表抄来的，因此可复现，成员也容易核对。 |
| [`fonts/passport_cjk_16.c`](fonts/passport_cjk_16.c) | 生成的 LVGL 字体，16 px，2 bpp，7,574 个字形 | 设备上的对话与界面文本。由 `tools/make_cjk_font.mjs` 从上面的清单和 `SourceHanSansSC-Normal.otf`（SIL Open Font License 1.1）生成；后者随 LVGL 一起提供，位于 `managed_components/lvgl__lvgl/scripts/built_in_font/`。OFL 字体的位图子集**允许再分发**；多数系统字体不允许，替换成它们会给本仓库带来许可问题。 |

重新生成：先 `npm install lv_font_conv@1.5.3 --prefix build/fontgen`，再
`node tools/make_cjk_font.mjs`，然后重新构建。未压缩的 2 bpp 约占 0.5 MB flash；由于生成的
C 文件有 3.5 MB，构建约需三分钟。改动清单就会改动字体，因此必须重新生成并**在真机上用
`font check` 重新测量**覆盖，而不是假定新的字符集覆盖了屏幕上显示的内容。不要手改生成文件。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。
