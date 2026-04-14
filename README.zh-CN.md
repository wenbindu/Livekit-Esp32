# livekit-esp32s3

[English](README.md) | 简体中文

`livekit-esp32s3` 是一个面向 `lichuang_esp32s3` 开发板的独立 ESP-IDF 项目。它保留了力创板的音频、配网、LCD 显示和 LiveKit 接入逻辑，并整理成适合公开开源发布的结构。

## 项目能力

- 让 ESP32-S3 设备接入 LiveKit 房间
- 采集麦克风音频并以 Opus 发布
- 订阅远端音频并在板载喇叭播放
- 提供轻量级 Wi-Fi 配网页面
- 保留力创板配套的 UI、音频处理和板级适配

## 目录结构

- `main/`：主固件逻辑、LiveKit 接入、UI、配网、板级 glue code
- `components/78__esp-wifi-connect`：本地 vendored 的配网页面组件
- `components/78__xiaozhi-fonts`：本地 vendored 的字体和表情资源
- `configs/`：板级默认配置、分支自带固件默认配置、本地配置样例文件
- `device_server/`：独立的 device server workspace，包含部署脚本和 `systemd`
- `docs/`：调试、打包、模式说明文档
- `scripts/project.sh`：配置、编译、烧录、串口监控入口
- `scripts/package_firmware.sh`：按当前 git branch 打包固件
- `scripts/debug_uplink_ws_server.py`：开发阶段接收调试音频并保存为 WAV

## 开源安全说明

真实密钥只能放在本地忽略文件里：

- `configs/livekit.local.env`
- `device_server/configs/device_server.local.env`

这些文件都已经被 `.gitignore` 忽略，不会进入仓库。

可以提交到仓库的只有样例文件：

- `configs/livekit.local.env.example`
- `device_server/configs/device_server.local.env.example`

注意：

- 仓库当前保留的固件 branch 统一使用 `AUTH_MODE=token_server`
- 不要把真实的 API key、secret、token、局域网 IP 写进 branch 默认配置、README 或默认配置

## 当前测试环境

当前仓库的整理基于以下环境：

- ESP-IDF `v5.5.3`
- target `esp32s3`
- board `lichuang_esp32s3`
- 使用 ESP-IDF 自带 Python 环境

## 开发环境搭建

### 1. 安装 ESP-IDF

示例流程：

```bash
git clone https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.5.3
bash install.sh
. ./export.sh
idf.py --version
```

可选验证：

```bash
cd examples/get-started/hello_world
idf.py build
```

### 2. 克隆本项目

```bash
git clone <your-repo-url> livekit-esp32s3
cd livekit-esp32s3
```

### 3. 创建本地配置文件

```bash
cp configs/livekit.local.env.example configs/livekit.local.env
```

然后编辑 `configs/livekit.local.env`。

机器本地至少需要这些字段：

- `TOKEN_SERVER_URL`
- `LIVEKIT_ROOM`
- `LIVEKIT_PARTICIPANT`
- `LIVEKIT_PARTICIPANT_IDENTITY`
- `LIVEKIT_AGENT_NAME`

常用鉴权服务参数：

- `TOKEN_SERVER_RETRY_DELAY_MS`
- `TOKEN_SERVER_AUTH_MAX_FAILURES`

device server 运行时密钥单独放在：

- `device_server/configs/device_server.local.env`

受 git 管理的固件默认配置放在：

- `configs/branch.defaults.env`

这个文件属于当前生命周期 branch。切换 branch，就切换默认生命周期行为。

可选的诊断 preset 放在：

- `configs/presets/`

### 4. 编译与烧录

开发固件：

```bash
git switch dev
bash scripts/project.sh flash-monitor
```

带双向音频 trace 的开发固件：

```bash
git switch dev
FIRMWARE_PRESET=audio-trace bash scripts/project.sh flash-monitor
```

生产固件：

```bash
git switch main
bash scripts/project.sh flash-monitor
```

## Branch 管理模型

生命周期 branch 只保留：

- `dev`：主开发路径
- `test`：集成验证与回归验证
- `main`：生产基线

诊断能力不再使用长期 branch，而是使用 preset：

- `uplink-trace`：导出处理后的上行音频
- `audio-trace`：同时导出上行和下行音频

更多说明见：

- `docs/branch-workflow.md`
- `docs/profiles.md`
- `docs/firmware-packaging.md`
- `docs/debug-audio.md`
- `docs/release-token.md`
- `docs/firmware-lifecycle-design.md`

## 力创开发板注意事项

当前仓库主要适配力创 ESP32-S3 开发板。

- 默认 board 是 `lichuang_esp32s3`
- 一般通过 Type-C 暴露串口，macOS 下通常是 `/dev/cu.usbmodem*`
- target 必须是 `esp32s3`
- BOOT 键用于进入配网或切换聊天状态
- 当前播放链路刻意保持应用层单声道 PCM，以适配单喇叭硬件
- 如果保存的 Wi-Fi 无法连接，固件应回退到配网流程

## Device Server

推荐用于日常开发和生产环境：

```bash
cp device_server/configs/device_server.local.env.example \
   device_server/configs/device_server.local.env
python3 device_server/scripts/device_server.py \
  --env-file device_server/configs/device_server.local.env
```

如果你希望用 `nohup` 方式后台运行，并自动管理 PID 和日志：

```bash
bash device_server/scripts/device_server_ctl.sh start
```

独立 workspace 里也附带了 `systemd` unit，文档见：

- `device_server/README.md`
- `docs/device-server.md`

建议把设备和服务端配置拆开：

- `configs/livekit.local.env`：设备构建配置、token server 地址、调试地址
- `device_server/configs/device_server.local.env`：device server 运行时密钥、LiveKit API key/secret

查看状态或停止：

```bash
bash device_server/scripts/device_server_ctl.sh status
bash device_server/scripts/device_server_ctl.sh stop
```

如果你要使用正式发布流程，也就是 server 签发 JWT、设备端自动换 token、多次鉴权失败后显示 `AUTH EXPIRED`，请看：

- `docs/release-token.md`

## Debug-Audio 工作流

启动桌面端接收器：

```bash
bash scripts/debug_audio_ws_start.sh
```

停止接收器：

```bash
bash scripts/debug_audio_ws_stop.sh
```

生成的 WAV 文件会写入 `debug_audio_ws/`，该目录已被 gitignore。

## 固件打包

查看当前 branch 的打包目标：

```bash
bash scripts/package_firmware.sh list
```

打包当前 branch 固件：

```bash
bash scripts/package_firmware.sh
```

示例：

```bash
git switch dev
bash scripts/package_firmware.sh preset audio-trace

git switch main
bash scripts/package_firmware.sh
```

## 实际使用建议

- LiveKit Cloud 是否可达仍然取决于开发板所处网络
- 如果网页端也无法和 agent 对话，应先查 agent 日志，不要优先怀疑固件
- 生产环境优先使用 token server，不要把 API secret 放到设备端
