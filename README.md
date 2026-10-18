# Youn Ink Four Color

ZecTrix ESP32-S3 4.2 英寸四色（黑/白/红/黄）墨水屏设备的固件，附一个可以在电脑上运行的模拟器。屏幕 400×300，三个按键：UP、DOWN、BOOT。

设备开机进入**首页**（应用列表），UP/DOWN 选择，BOOT 打开，**长按 BOOT 回首页**。内置应用：

| 应用 | 说明 |
| --- | --- |
| 相册 | 浏览照片；通过设备网页或局域网接口上传（2bpp 四色 BWRY / 1bpp 黑白） |
| 天气 | Open-Meteo 天气预报，最多 5 个城市，在设备网页 `/weather` 里设置 |
| 语音 | 长按 UP 录音，松开保存；列表、播放、删除 |
| AI 问答 | 长按 UP 说话提问，答案一次性显示；多会话、记住上下文、联网搜索（阿里云百炼） |
| 配网 | Wi-Fi 配置页 |
| 设置 | Wi-Fi、局域网服务、亮度/睡眠等 |

每个页面怎么操作、按键含义、已知限制，见 **[user_manual.md](user_manual.md)**。

![2BP BWRY architecture](README-2bp-architecture.png)

> 本仓库的四色 `2BP BWRY` 链路与 4BP 黑白灰阶相册独立维护：面板颜色、像素格式和刷新驱动均不同。

## 目录结构

```text
.
├── firmware/        ESP-IDF 固件：RawDraw UI、页面渲染、屏幕驱动、局域网/AP HTTP 服务
│   ├── main/        固件源码（模拟器也直接编译这里的文件）
│   └── scripts/     build_local.sh（编译）、upload_firmware.sh（发布下载）
├── sim/             PC 模拟器：网页界面 + 设备真实的 HTTP 上传服务 + 自动化测试
├── server/          mock_client.py（旧的对话协议调试脚本，固件当前不用）
├── docs/            局域网传图接口（LAN_PHOTO_PUSH_API.md）等
├── user_manual.md   用户手册
└── CLAUDE.md        开发约定：共享代码、模拟器与固件的差异、编译发布流程
```

## AI 问答

- 全程语音输入：按住 UP 说话，松开发送。录音先交给语音识别 `qwen3-asr-flash`，识别出的文字再发给 `qwen-flash`（已开启百炼内置联网搜索），都走 `dashscope.aliyuncs.com` 的兼容接口，**一个 API Key 通用**。
- 答案等全部生成后**只刷新一次屏幕**（墨水屏刷新慢，不显示"思考中"）。录音时指示灯闪烁、有提示音。
- 会话在设备上管理：BOOT 打开会话列表，长按 UP 新建、长按 DOWN 删除。会话只存在内存，重启后清空。
- API Key 不编进固件：烧录后在浏览器打开 `http://设备IP/ai` 填入，保存在设备 NVS 里。
- 逻辑在 `firmware/main/common/ai_chat.*`（与硬件无关，模拟器共用），设备端网络和 Key 存储在 `ai_chat_platform_esp.cc`。

## 在电脑上体验（模拟器）

```bash
make -C sim                       # 编译（首次会下载 cJSON）
make -C sim test                  # 导航/AI 问答等自动化测试
python3 sim/web/server.py         # 网页 http://127.0.0.1:8000 ，设备上传服务 http://127.0.0.1:8080
```

- 网页上用鼠标或键盘 ↑ ↓ Enter 模拟按键；长按 ↑ 会打开浏览器麦克风，可以真的对着电脑说话提问。
- AI 问答默认没有 Key：启动前设置 `DASHSCOPE_API_KEY=你的百炼Key` 才会调用真实接口；设置 `SIM_AI_FAKE=1` 则使用内置的模拟回答。
- 模拟器没有 Wi-Fi、屏幕刷新、状态栏等硬件相关部分，这些只能在真机上验证，详见 `CLAUDE.md`。

## 编译和烧录固件

需要 **ESP-IDF v6.0.0**（其他版本，如 6.0.3，会编译失败）。

```bash
firmware/scripts/build_local.sh              # 增量编译，加 --clean 完全重编
# 产物在 firmware/releases/v<版本>_zectrix-s3-epaper-4.2/
#   merged-binary.bin     完整镜像，烧录地址 0x0
#   app-only_xiaozhi.bin  仅应用，烧录地址 0x20000（保留设置）
firmware/scripts/upload_firmware.sh          # 可选：发布到自己的服务器供下载
```

烧录（设备需处于下载模式）：

```bash
python -m esptool --chip esp32s3 -b 460800 write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x0 merged-binary.bin
```

- 默认面板为四色 BWRY（`ZECTRIX_EPD_PANEL_4COLOR_SSD2683`），也保留 1bpp 黑白屏配置，需在 `idf.py menuconfig` 中切换后重编。
- 编译细节（环境、依赖、发布脚本配置）见 `CLAUDE.md` 的 "Building the firmware" 和 "Publishing a build"。
- `upload_firmware.sh` 的服务器信息不在仓库里，从环境变量读取（`REMOTE_HOST`、`SSH_KEY`、`REMOTE_DIR`、`URL_BASE`）。

## 当前状态

- 固件完整编译通过；首页、按键导航、各应用在模拟器里有自动化测试。
- AI 问答的请求和响应在电脑上用真实的百炼接口验证过（语音识别 + 联网对话）；设备端的 HTTPS、NVS 保存 Key、整段录音上传（约 1.3 MB 请求体）的内存占用，以真机运行为准。
- 设备按键驱动不产生双击事件，所以依赖双击的功能（照片删除对话框、快速切换浮层）在设备上用不了。

## 不要提交

构建产物 `firmware/build/`、`firmware/managed_components/`、`firmware/sdkconfig`、`firmware/releases/`、`sim/build/`，以及任何 `.env`、日志、密钥。**API Key 绝不写进源码或仓库。**
