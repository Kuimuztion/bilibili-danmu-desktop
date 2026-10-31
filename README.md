# Bilibili 桌面弹幕悬浮客户端 (bilibili-danmu-desktop)

> 文档更新：2026-10-31

基于 C 语言与现代 GTK4 框架构建的高性能、无边框、全透明 B 站直播间桌面弹幕悬浮客户端。

严格遵循内核代码风格（Linux Kernel Coding Style），具备极速渲染、极低系统资源占用、多LLM公司皮肤高质感聊天气泡、真实 B 站用户高清圆形头像裁剪、桌面鼠标完全穿透、跨越全屏游戏/视频的最高层级防遮挡置顶与全功能终端交互控制等特性。

---

## 建议使用环境 (Recommended Environment)

- **操作系统**：**Fedora 44**（推荐）、**Arch Linux** 或其他现代 Linux 发行版
- **桌面环境**：**KDE Plasma 6 (Wayland 会话，推荐最佳体验)** / KDE Plasma 5 / GNOME (Wayland / X11)
- **编译工具**：GCC (支持 C11 标准)、GNU Make、pkg-config
- **核心依赖库**：
  - `gtk4` (>= 4.10)
  - `gtk4-layer-shell` (原生 Wayland 最高 Overlay 悬浮图层支持)
  - `libcurl` (>= 7.86.0，提供 `curl_ws_recv()` / `curl_ws_send()` 原生 WebSocket API)
  - `zlib`
- **字体推荐**：Google Fonts（如 Google Sans、Roboto、Noto Sans CJK），气泡排版默认采用高辨识度加粗大字号设计质感排版

### 安装编译依赖

本项目需要 C11 编译器、GNU Make、pkg-config，以及 GTK4、GTK4 Layer Shell、libcurl 和 zlib 的开发文件。**libcurl 必须支持 WebSocket API**（`curl_ws_recv()` / `curl_ws_send()`；要求 libcurl 7.86.0 或更新版本）。

根据你的发行版选择一组命令执行：

- **Fedora 44 / RHEL 系**：
  ```bash
  sudo dnf install gcc make pkgconf-pkg-config gtk4-devel gtk4-layer-shell-devel libcurl-devel zlib-devel
  ```
- **Arch Linux 系**：
  ```bash
  sudo pacman -S --needed gcc make pkgconf gtk4 gtk4-layer-shell curl zlib
  ```
- **Debian 13 (Trixie) / 更新版本**：
  ```bash
  sudo apt update
  sudo apt install build-essential pkg-config libgtk-4-dev libgtk4-layer-shell-dev libcurl4-openssl-dev zlib1g-dev
  ```
  > **Debian 12 (Bookworm) 提醒：** Debian 12 的官方仓库提供的是 GTK3 版 `libgtk-layer-shell-dev`，不是本项目需要的 GTK4 版开发包。建议使用 Debian 13 (Trixie) 或更新版本；若必须使用 Bookworm，需要自行从上游编译安装 GTK4 版 `gtk4-layer-shell`，并确保 `pkg-config` 能找到它。
- **openSUSE Tumbleweed**：
  ```bash
  sudo zypper refresh
  sudo zypper install gcc make pkgconf gtk4-devel gtk4-layer-shell-devel libcurl-devel zlib-devel
  ```
  > **openSUSE Leap 提醒：** 安装前先运行 `zypper search -s gtk4-layer-shell-devel` 检查当前 Leap 版本的软件源是否提供 GTK4 Layer Shell 开发包。若没有，该功能依赖需要从上游单独构建安装；不要用 GTK3 的 `gtk-layer-shell` 开发包代替。
- **Gentoo**：
  ```bash
  sudo emerge --ask \
    sys-devel/gcc \
    sys-devel/make \
    dev-util/pkgconf \
    gui-libs/gtk:4 \
    gui-libs/gtk4-layer-shell \
    net-misc/curl \
    sys-libs/zlib
  ```
  `net-misc/curl` 需要启用 `websockets` USE flag。当前 Gentoo ebuild 默认启用该 flag；如果你的本地配置覆盖了默认值，可执行下面的命令显式启用，再重新构建 curl：
  ```bash
  echo "net-misc/curl websockets" | sudo tee /etc/portage/package.use/bilibili-danmu
  sudo emerge --ask --changed-use net-misc/curl
  ```

安装完成后，可用下面的命令检查 `pkg-config` 是否能找到核心依赖及其版本：

```bash
pkg-config --modversion gtk4 gtk4-layer-shell libcurl zlib
```

如果命令报告找不到某个模块，说明对应开发包未安装，或其 `.pc` 文件没有进入 `pkg-config` 搜索路径。先解决依赖识别问题，再执行项目编译。

---

## 核心特性

- ** C 与分层构建**：公共头文件集中放置于 `include/`，目标文件与依赖文件放置于 `build/`，可执行文件输出至 `bin/danmu_desktop`；Makefile 自动创建缺失目录并跟踪头文件依赖。
- **四大品牌聊天气泡主题（一键即时无损切换）**：
  - **Google**：四色经典环绕渐变边框，天蓝文字与幽蓝微光投影；
  - **OpenAI**：纯白官方矢量图标，极客黑曜石毛玻璃背景搭配极光薄荷绿（`#10a37f`）与纯白双色微光外框；
  - **Anthropic**：官方暖陶土矢量图标（`#d97757`），珊瑚沙岩自然质感渐变与深褐半透明背景；
  - **DeepSeek**：官方电光蓝矢量图标（`#4d6bfe`），深海跃动电光蓝外框与高反差澄澈天蓝文字；
  - **无损自由反复切换**：在控制台输入 `skin <名称>` 即可一键更换。切换时，**屏幕上已显示的所有弹幕气泡与后续新弹幕**均会实时更新为新品牌的 SVG 图标与 CSS 样式，反复来回切换互不影响、零损坏。
- **真·无边框全透视桌面悬浮**：
  - 去除窗口标题栏及所有多余窗口控件；
  - 彻底去除背景图片与遮挡层，窗口完全透明透视，仅纯净气泡悬浮在桌面上直视壁纸；
  - 气泡采用加粗大字号设计，远距离观看清晰醒目。
- **Wayland 原生跨全屏防遮挡置顶 (Layer Shell Overlay 最高物理图层)**：
  - **图层机制**：在现代合成器（如 KDE Plasma 6 Wayland）中，全屏激活窗口位于 `ActiveFullScreenLayer`，普通置顶窗口在全屏时会被物理压在下方；
  - **权威标准方案**：原生接入 `wlr-layer-shell-v1` 协议（通过 `gtk4-layer-shell` 库），将弹幕悬浮窗口提升至合成器的最高物理图层 `ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY`（KWin 物理图层 Layer 9）；
  - **绝对跨全屏**：无论全屏 3D 游戏、全屏视频播放（mpv/vlc/浏览器全屏）还是 F11，弹幕气泡永远绝对居于全屏图层之上！
  - **平滑手势拖拽**：手势控制器始终挂载于全屏静止画布上，彻底消除相对位移带来的卡顿与高频抖动，兼具极速平滑位移与全屏置顶能力。
- **鼠标穿透与拖拽交互双模式**：
  - **开启交互 (`mouse on`)**：恢复鼠标捕获，可在弹幕窗口任意区域直接按住拖拽移动位置，遮挡下方窗口；
  - **鼠标穿透 (`mouse off`)**：弹幕窗口锁定不可拖拽，鼠标事件完全穿透到弹幕下方的任何程序（下方窗口可正常点击、高亮选取文字、滑动浏览）。
- **低延迟 WebSocket 接收**：每轮等待 socket 前先排空 libcurl 内部接收缓冲；持续读取同一帧的剩余数据，重组 WebSocket 分片，并缓存跨 WebSocket 消息边界的 B 站协议残包；新弹幕通过 GLib 高优先级主循环源派发到 GTK 主线程。
- **礼物消息自动捕获与高亮显示**：
  - 仅处理真实交易事件（`SEND_GIFT`、`COMBO_SEND`、`GUARD_BUY`），忽略礼物星球进度、房间提示等非赠礼广播，避免生成“热心观众 / 礼物 x1”假气泡；
  - 送礼人昵称与 UID 仅从礼物事件 `data` 的直接字段或其 `user_info`、连击赠礼对象中读取，不会把 `receive_user_info` 中的主播误判为送礼人；
  - 忽略 UID 与当前房主 UID 相同的无效赠礼事件；缺少昵称但有真实 UID 时显示临时占位名，并继续尝试通过用户资料卡补全；
  - 遵循 B 站直播原生弹幕视觉规范，金黄色高亮显示送礼动作与数量（如：`投喂 粉丝团灯牌 x66`）。
- **历史弹幕自动持久化与冷启动防重复**：
  - 启动后自动将近期所有弹幕与送礼记录追加持久化至 `/tmp/bilibili_danmu_history.log`；
  - 软件重新打开时，自动从日志恢复历史弹幕并还原到桌面悬浮窗中；
  - 恢复模式下绝不重复写回日志，确保无论关闭并重新打开多少次，日志与桌面记录永远纯净不重复。
- **纯净终端交互（GTK 底层杂乱日志完全静音）**：
  - 原生挂载 `g_log_set_writer_func` 过滤框架告警，防止底层提示干扰终端控制台；
  - 支持 `skin`、`mouse`、`room`、`test`、`clear`、`scroll`、`top`、`drag_test` 等丰富控制指令。
- **真实用户头像与智能缓存**：
  - 异步请求 B 站真实用户头像，支持防空图校验与过期自动更新；
  - 基于 Cairo 进行原生高保真抗锯齿圆形羽化裁剪，下载完成后动态平滑替换气泡头像。

---

## 构建与运行

### 1. 编译项目

在项目根目录下执行：

```bash
make -j"$(nproc)"
```

Makefile 会自动创建构建目录：

- `include/`：所有公共 `.h` 头文件；
- `build/`：所有 `.o` 目标文件及自动生成的 `.d` 依赖文件；
- `bin/`：最终可执行文件 `bin/danmu_desktop`。

清理中间产物并重新编译：

```bash
make clean
make -j"$(nproc)"
```

### 2. 运行应用

```bash
# 1. 默认加载项目根目录 config.json
./bin/danmu_desktop

# 2. 命令行指定直播间号启动
./bin/danmu_desktop -r 123456

# 3. 启动并注入测试弹幕
./bin/danmu_desktop --test
```

程序会根据自身可执行文件位置定位源码树根目录，因此从其他当前工作目录启动 `bin/danmu_desktop` 时，也会优先加载项目根目录中的配置、CSS 和资源文件。

启动后终端将呈现命令提示符 `danmu> `：
```text
======================================================
  B站直播桌面弹幕客户端 (终端控制交互模式)
  直播间: 123456 | 全透明桌面悬浮窗 | 极简微光气泡
  弹幕日志: /tmp/bilibili_danmu_history.log (持久化保存)
  全屏防遮挡: 已开启 (原生 Wayland Layer Shell Overlay，跨越全屏游戏/视频/F11)
  气泡皮肤: Google (经典四色微光)
======================================================
可用命令:
  help                       - 显示此帮助信息
  log / history              - 查看弹幕日志统计与近期记录
  test [内容]                - 发送测试弹幕 (真实用户)
  test <UID> <用户名> <内容> - 使用指定B站UID发送测试
  clear                      - 清空桌面弹幕列表
  skin [名称]                - 一键更换气泡皮肤 (google/openai/anthropic/deepseek)
  scroll [on|off]            - 切换/设置自动置底滚动
  top [on|off]               - 切换/设置窗口置顶防全屏遮挡
  overlay [on|off]           - 切换全屏防遮挡 (KWin OSD浮动层)
  mouse [on|off]             - 切换鼠标交互(on:可拖拽 / off:鼠标穿透)
                               (别名: interactive, click, drag)
  drag_test [dx] [dy] [步数]  - 模拟桌面拖动测试并输出延迟分析报告
  room <房号>                - 切换连接到新的B站直播间
  status                     - 查看当前连接状态与统计
  quit / exit                - 退出程序
======================================================
danmu> 
```

---

## 终端交互控制常用命令

| 命令 | 别名 | 说明与示例 |
| :--- | :--- | :--- |
| `skin [名称]` | `theme` | 更换气泡皮肤：`skin openai`、`skin anthropic`、`skin deepseek`、`skin google` |
| `mouse [on\|off]` | `interactive`, `click`, `drag` | `mouse off` 开启鼠标全穿透（防误触，下方窗口可正常交互）；`mouse on` 开启悬浮窗拖动 |
| `room <房号>` | - | 切换直播间：`room 123456`（断开当前长连接并立即接入新房间） |
| `test [内容]` | - | 模拟弹幕注入：`test 666` 或使用真实 UID 发送 `test 2 碧诗 乾杯` |
| `drag_test [dx] [dy] [步]` | - | 模拟桌面拖动并输出微秒级延迟基准测试报告：`drag_test 300 200 60` |
| `clear` | - | 清空桌面所有已渲染的气泡并刷新终端界面 |
| `log` | `history` | 查看持久化弹幕历史日志路径、大小、行数与最近记录 |
| `status` | - | 查看当前直播间号、长连接状态、身份码配置、图层模式与皮肤状态 |
| `quit` | `exit`, `q` | 优雅退出程序 |

---

## 配置文件 (config.json)

在应用工作目录下创建或修改 `config.json` 即可持久化个人设置：

```json
{
  "room_id": 123456,
  "identity_code": "",
  "cookie": "",
  "skin": "google",
  "font_size": 14,
  "max_items": 100,
  "auto_scroll": true,
  "keep_above": true,
  "mouse_interactive": true
}
```

### 配置项说明

- `room_id` *(整数)*：默认连接的 B 站直播间真实房号（设为 `0` 时程序启动将保持待命并等待终端指令输入）。
- `identity_code` *(字符串，可选)*：预留的 B 站开放平台主播身份码字段。本版本普通弹幕连接不使用该字段，代码目前只读取并在 `status` 中显示；没有专门接入开放平台互动玩法时请保持空字符串。它不是 B 站 UID，也不是 Cookie。
- `cookie` *(字符串，可选，建议配置)*：B 站网页端完整 Cookie（至少包含有效的 `SESSDATA`，建议直接复制浏览器 `live.bilibili.com` 请求中的完整 Cookie）。程序会从 `/x/web-interface/nav` 读取登录账号 UID，并使用同一账号 UID 完成弹幕 WebSocket 认证。**未配置或 Cookie 失效时，B 站会按游客连接处理，可能把观众昵称显示为 `***`、UID 显示为 `0`；客户端无法绕过 B 站这项隐私保护。请勿把 Cookie 发给他人。**
- `skin` *(字符串)*：默认聊天气泡皮肤，可选：
  - `"google"`：经典四色全环绕微光渐变边框
  - `"openai"`：极客黑曜石极光薄荷绿纯白微光边框
  - `"anthropic"`：官方暖陶土珊瑚沙岩微光边框
  - `"deepseek"`：官方深海电光跃动蓝微光边框
- `font_size` *(整数)*：弹幕文字像素尺寸（默认 14）。
- `max_items` *(整数)*：悬浮列表中最多保留并显示的弹幕气泡条数（默认 100）。
- `auto_scroll` *(布尔值)*：新弹幕到达时是否自动平滑滚动置底（默认 `true`）。
- `keep_above` *(布尔值)*：是否启用最高图层置顶防遮挡（默认 `true`）。
- `mouse_interactive` *(布尔值)*：启动时是否默认允许鼠标拖动（`true` 可拖动；`false` 默认完全穿透防误触）。

### 如何获取 B 站 Cookie（必须仔细阅读）

本程序建议使用**你自己已登录 B 站账号的网页 Cookie**。Cookie 是登录凭证，效果等同于在一定期限内代表你的账号访问 B 站。只在自己的电脑上复制并写入本地 `config.json`，**不要发给开发者、聊天群、论坛、截图或任何第三方**。

#### 方法 A：Chrome / Chromium / Edge（推荐）

1. 在同一个浏览器中打开 [B 站直播](https://live.bilibili.com/)，确认右上角显示已登录。建议再打开你准备监听的直播间。
2. 按 `F12` 打开开发者工具，进入 **Network（网络）** 面板。
3. 点击 **Fetch/XHR** 过滤请求，然后刷新直播页面（`Ctrl + R` 或 `F5`）。
4. 在请求列表中找到 `getDanmuInfo?...`（通常请求域名为 `api.live.bilibili.com`）并**单击整条请求记录**。
5. 在请求详情面板中点击 **Headers（标头）**。注意：这是点击请求后出现的详情标签，不是 Network 列表里的 `Initiator`（发起程序）列。
6. 展开 **Request Headers（请求标头）**，找到 `Cookie`。复制 `Cookie` 后面的完整值，不要复制 `Cookie:` 字段名称。
7. 如果 `Request Headers` 中没有显示 Cookie，可改用开发者工具的 **Application（应用）→ Storage（存储）→ Cookies → `https://live.bilibili.com`**，逐项查看并复制 Cookie 值。

#### 方法 B：Firefox

1. 在 Firefox 中登录 [B 站直播](https://live.bilibili.com/)，打开目标直播间。
2. 按 `F12` 打开开发者工具，选择 **网络（Network）**，刷新页面。
3. 在请求列表中选择 `getDanmuInfo` 请求（或同域名的直播 API 请求）。
4. 在右侧请求详情中选择 **标头（Headers）**，展开 **请求标头（Request Headers）**，找到 `Cookie` 并复制其值。
5. 如果请求详情没有显示 Cookie，可打开开发者工具的 **存储（Storage）→ Cookies → `https://live.bilibili.com`** 查看。

#### 需要复制哪些字段？

建议复制浏览器请求中 `Cookie` 的**完整值**，不要只手工拼接某几个字段。确认其中至少存在有效的 `SESSDATA`；完整 Cookie 通常还会包含 `bili_jct`、`DedeUserID` 等字段。不要把 `Cookie` 请求头以外的 `Request Headers` 全部复制进配置文件。

配置格式示例（下面均为占位文字，不是真实凭证）：

```json
{
  "room_id": 1881592284,
  "identity_code": "",
  "cookie": "SESSDATA=替换为你的值; bili_jct=替换为你的值; DedeUserID=替换为你的UID; 其他Cookie字段=对应值",
  "skin": "google",
  "font_size": 14,
  "max_items": 100,
  "auto_scroll": true,
  "keep_above": true,
  "mouse_interactive": true
}
```

将 `cookie` 的内容替换成刚才复制的完整 Cookie 字符串，保持它在 JSON 中为**一行字符串**。如果 Cookie 值中含有双引号或反斜杠，需要按 JSON 规则转义；不要额外添加换行，也不要把 `Cookie:` 这个字段名写进去。保存后建议限制配置文件权限：

```bash
chmod 600 config.json
```

**不要使用浏览器控制台里的 `document.cookie` 代替上述方法。** `SESSDATA` 通常带有 `HttpOnly` 属性，JavaScript 无法通过 `document.cookie` 读取它。也不要把 Cookie 放进 Git 提交、公开 issue 或日志附件中。若曾经意外泄露，请在 B 站账号安全设置中退出其他登录/更换登录状态，并重新获取 Cookie。

---

### 昵称显示为 `***` 时

请确认 `config.json` 中的 `cookie` 不是空字符串，并且其中的 `SESSDATA` 仍然有效。Cookie 必须属于已登录的 B 站账号。启动后执行 `status`：若显示“未认证”，程序只能以游客身份连接，B 站会主动隐藏部分观众昵称；若显示登录账号 UID，则已使用登录账号身份发起弹幕连接。Cookie 过期后需要重新从浏览器复制。**不要将完整 Cookie 粘贴到公开群聊或发送给他人。**

---

## 常见问题排查

### 1. `pkg-config` 找不到 GTK4 或 gtk4-layer-shell

先检查依赖版本：

```bash
pkg-config --modversion gtk4 gtk4-layer-shell libcurl zlib
```

确认安装的是 GTK4 版本的 Layer Shell 开发包，而不是 GTK3 的 `gtk-layer-shell`。如果是从源码安装的库，检查其 `.pc` 文件是否位于 `pkg-config` 可搜索的目录；必要时按实际安装路径设置 `PKG_CONFIG_PATH`。

### 2. 编译时报 `curl_ws_recv` / `curl_ws_send` 未声明

通常表示找到的 libcurl 版本低于 7.86.0，或头文件与链接时使用的 libcurl 不是同一套安装。检查：

```bash
pkg-config --modversion libcurl
pkg-config --cflags --libs libcurl
```

Gentoo 用户还应确认 `net-misc/curl` 的 `websockets` USE flag 已启用。

### 3. 程序连接成功但其他观众昵称显示为 `***`

检查 `config.json` 中 `cookie` 是否为空、是否属于当前已登录的 B 站账号，以及 `SESSDATA` 是否过期。启动程序后执行 `status`，确认显示了登录账号 UID。若状态为未认证，请重新获取 Cookie。即使使用有效登录态，B 站仍可能对特定用户或事件执行隐私脱敏，客户端不能绕过平台限制。

### 4. 全屏覆盖或拖动效果与预期不同

Layer Shell 的全屏覆盖能力依赖 Wayland 会话和支持 Layer Shell 的合成器。X11 会话不提供相同的 Wayland Layer Shell 行为；不同桌面环境/合成器对 Overlay 层的支持也可能不同。可先确认当前会话类型：

```bash
echo "$XDG_SESSION_TYPE"
```

---

## 开源协议

本项目采用 **GPL-3.0-only** 许可证。详情请参阅根目录下的 [LICENSE](LICENSE) 文件。

---

## 开发者文档

需要了解模块职责、GTK 渲染树位置、网络/头像线程边界、JSON token 的借用生命周期，以及关键指针和 Cairo/GTK 资源的所有权规则，请阅读 [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)。该文档也记录了条件分支约定：`if` 直接嵌套最多两层，深层逻辑应提取为守卫条件或独立辅助函数。

可在修改后运行结构检查脚本，检查 C 函数文档覆盖率和 `if` 嵌套深度：

```bash
python3 scripts/check_source_structure.py
```

---

## 开发者、LLM 协作与测试人员名单

本项目由项目负责人主导规划与集成，并在开发过程中使用 LLM 编程助手协助完成界面定制、问题定位、缺陷修复和性能优化。以下名单按照实际参与的工作内容分别列出，感谢每一位参与开发、测试与反馈的贡献者。

### 项目主体架构与核心代码

- **Kuimuztion（你猜我在线不）** — 项目主体架构框架规划与核心源代码撰写。负责项目整体方向、模块职责划分、核心功能设计与实现，以及各功能模块的整合和持续维护，是本项目的主要开发者。

### AI 协作与专项开发支持

- **Gemini、Claude** — 参与皮肤定制与桌面 GTK 渲染相关工作的设计和代码协助，包括不同视觉主题的样式调整、气泡呈现效果，以及 GTK 桌面悬浮窗口渲染与交互体验的迭代完善。
- **GPT、Claude** — 参与历史缺陷排查、复杂 Bug 修复与关键性能重构，协助梳理网络接收和消息派发流程、优化复杂条件分支与代码组织，并完善关键函数、指针生命周期、线程边界及资源所有权方面的注释和说明。

> 注：Gemini、Claude 与 GPT 在此列为开发过程中使用的 LLM 协作工具，不代表其为项目的人类署名开发者；相关代码由项目负责人结合实际需求进行整合与维护。

### 测试人员（B 站用户）

- **181662208（枫月万年）** — 参与用户弹幕接收与桌面显示测试，协助验证普通弹幕的接收、昵称显示及悬浮窗口呈现情况。
- **3546838915353336（bili_23522435519）** — 参与用户弹幕测试与礼物投喂测试，协助验证普通弹幕、送礼事件、送礼人信息及礼物内容的识别与显示。

感谢以上测试人员提供真实直播场景下的测试支持与问题反馈。相关测试反馈帮助项目持续改进消息解析、用户信息展示和礼物事件处理等功能。

