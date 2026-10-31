# 代码结构、线程边界与指针所有权说明

本文档面向维护者，说明本项目各模块的职责、主要数据流、GTK 渲染树，以及 C 指针和资源的生命周期。源文件中的每个函数定义均配有 Doxygen 风格说明；关键所有权位置另有行内注释。

## 1. 模块职责

| 文件 | 职责 | 关键入口/函数 |
|---|---|---|
| `src/main.c` | 应用启动/退出、配置读取、终端命令控制、应用层回调 | `main()`、`app_activate()`、`app_shutdown()`、`handle_terminal_command()` |
| `src/bilibili.c` | HTTP 接口、WebSocket 长连接、协议包解析、消息派发 | `bili_worker_thread()`、`connect_stream()`、`process_packets()`、`handle_event()` |
| `src/avatar.c` | 用户资料卡补全、头像下载、圆形图像生成、缓存和 worker 队列 | `avatar_init()`、`avatar_worker_func()`、`process_avatar_download()` |
| `src/ui.c` | GTK4 透明窗口、Layer Shell、拖拽、皮肤、弹幕行与滚动 | `ui_init()`、`ui_render_danmu_bubble()`、`ui_on_avatar_updated()` |
| `src/util.c` | 字符串缓冲、轻量 JSON token 解析、文件 IO、MD5 和历史日志 | `sb_*()`、`json_*()`、`util_load_history_danmu()` |
| `include/` | 公共结构体和模块 API 声明 | `app_state.h`、`bilibili.h`、`avatar.h`、`ui.h`、`util.h` |

## 2. 线程模型与 GTK 边界

本项目有三个逻辑执行区域：

1. **GTK 主线程**：由 `g_application_run()` 驱动。所有 `GtkWidget`、`GtkAdjustment`、`GdkSurface` 的创建、读取、修改和销毁都必须发生在这里。
2. **B 站网络 worker**：由 `bilibili_start()` 启动。负责 HTTP、WebSocket、协议拆包、JSON 解析。它不直接调用 GTK API。
3. **头像 worker**：由 `avatar_init()` 启动。负责资料卡 HTTP、头像下载、Pixbuf 解码、Cairo 裁剪和磁盘缓存。它不直接操作 GTK Widget。

### 跨线程派发流程

```text
B 站 WebSocket worker
  └─ handle_event()/handle_danmu_msg()/handle_gift_msg()
       └─ dispatch_danmu()
            ├─ malloc 派发结构
            ├─ 复制 struct danmu_item（纯值结构，无 GTK 指针）
            └─ g_idle_add_full(G_PRIORITY_HIGH, on_danmu_idle, ...)
                  └─ GTK 主线程调用应用回调
                       └─ ui_add_danmu()/ui_render_danmu_bubble()

头像 worker
  └─ notify_avatar_updated()
       ├─ malloc 通知结构
       ├─ 复制 UID、头像路径、真实昵称
       └─ g_idle_add_full(..., on_avatar_downloaded_idle, ...)
             └─ GTK 主线程更新匹配的气泡头像和昵称
```

`on_danmu_idle()`、`on_status_idle()` 和 `on_avatar_downloaded_idle()` 是主线程回调。各自收到的 `user_data` 是此前 `malloc()` 的派发载荷；回调结束前必须 `free()`，不能由其他线程提前释放。

## 3. 指针与资源所有权约定

| 对象/指针 | 指向内容 | 所有权和释放规则 |
|---|---|---|
| `struct app_state *state` | 应用生命周期内的全局状态 | 通常是借用指针；由 `main.c` 管理生命周期，不能在子模块释放。跨线程访问共享字段时遵守 `state->lock`。GTK 字段仅主线程可访问。 |
| `GtkWidget *` / `GtkAdjustment *` | GTK 控件或滚动调整对象 | GTK/GObject 管理引用。局部变量一般是借用引用，不调用 `free()`；父容器挂载控件后负责持有子控件。 |
| `json_*_find_value()` 返回的 `const char *` | 原始 JSON 文本内部某个 token 的起始位置 | **借用指针**，不分配内存、不允许 `free()`；只在原 JSON 缓冲区存活期间有效。要跨线程/异步使用，必须复制成独立字符串。 |
| `char *` 输出缓冲区参数 | 调用者提供的可写内存 | 调用者分配和释放；被调函数只在配套容量范围内写入。 |
| `struct str_buf` 及其 `data` | 动态字节/字符串缓冲区 | `sb_init()` / append 系列管理内部容量；`sb_free()` 释放 `data`。append 可能 realloc，之前保存的 `data` 内部地址可能失效。 |
| `read_entire_file()` 返回值 | 完整文件内容的堆缓冲区 | 所有权转移给调用者，调用者必须 `free()`。 |
| `decompress_zlib()` 的输出指针 | 解压后的堆缓冲区 | 成功后由调用者持有，使用完 `free()`。 |
| `GdkPixbuf *` | 解码后的图像对象 | GObject 引用计数；谁获得新引用谁负责 `g_object_unref()`。传入像素读取函数的 pixbuf 通常只是借用。 |
| `cairo_surface_t *` / `cairo_t *` | Cairo 图像表面/绘图上下文 | 创建者必须分别调用 `cairo_surface_destroy()` / `cairo_destroy()`；函数若明确返回新建 Surface，则所有权转给调用者。 |
| `CURL *` / `curl_slist *` | libcurl 请求句柄/请求头链表 | 当前请求函数创建的句柄和链表由该函数清理；传入 helper 的 CURL 句柄通常只是借用。 |
| `struct avatar_task *` | 头像下载队列节点 | 节点入队后由队列/worker 持有；worker 摘链后负责处理并 `free()`。队列指针只在 `g_mgr.lock` 保护下修改。 |

### JSON token 的生命周期

`json_object_find_value()`、`json_find_value()`、`json_array_element()` 返回的指针都指向调用者原始 JSON 文本内部，不是新分配的字符串。禁止把这些指针直接放进异步任务。需要延迟处理时，应像 `dispatch_danmu()` 一样把值复制进结构体，或用 `g_strdup()` / `malloc()` 保存独立副本。

## 4. GTK 弹幕渲染树在哪里

**完整的单条弹幕渲染代码位于 `src/ui.c` 的 `ui_render_danmu_bubble()`。** `ui_add_danmu()` 和 `ui_restore_danmu()` 都会调用它；差别仅在于实时弹幕会写历史日志，恢复的历史弹幕不会再次写日志。

渲染树结构如下：

```text
state->list_box                 (弹幕列表父容器 GtkBox)
  └─ row                         (单条弹幕根行 GtkBox)
      ├─ avatar_img              (圆形头像 GtkImage)
      └─ bubble_border           (主题边框 GtkBox)
          └─ bubble_content       (气泡内容 GtkBox)
              ├─ uname_lbl       (昵称 GtkLabel)
              ├─ text_lbl        (弹幕/礼物文本 GtkLabel)
              └─ google_box      (右下角品牌标识 GtkBox)
                  └─ google_img  (当前主题 SVG GtkImage)
```

构建步骤：

1. 创建 `row`，设置方向、CSS class 和拖拽事件穿透属性。
2. 把 UID/hash 的**字符串副本**用 `g_object_set_data_full()` 关联到 `row`，并指定 `g_free` 作为销毁函数。
3. 创建头像、边框、内容、昵称、文本和品牌图标控件。
4. 用 `gtk_box_append()` 将子控件逐层挂入父容器，最终把 `row` 追加到 `state->list_box`。
5. 修剪超出 `state->max_items` 的最早行。
6. 通过 `schedule_scroll_to_bottom()` 在 GTK 后续帧完成布局后置底。

`row`、`avatar_img`、`bubble_border` 等局部变量只是指向 GTK 控件的借用指针。它们挂入父子对象树后由 GTK 管理，不能对这些局部指针调用 `free()`。保存在 `g_object_set_data()` 中的控件指针也只是快捷引用，不拥有额外引用；对应控件必须与 `row` 同生命周期。

## 5. B 站数据接收和解析

```text
connect_stream()
  ├─ 完成 WebSocket 握手与认证
  └─ handle_ws_incoming_data()
       ├─ 先排空 libcurl 内部已缓冲数据
       ├─ 重组 WebSocket frame / continuation frame
       └─ process_packets()
            ├─ 校验 16 字节 B 站协议头和包长
            ├─ version=2：zlib 解压后递归解析
            ├─ operation=8：更新连接状态
            └─ operation=5：复制 JSON 正文并 handle_event()
                 ├─ DANMU_MSG → handle_danmu_msg()
                 └─ SEND_GIFT / COMBO_SEND / GUARD_BUY → handle_gift_msg()
```

礼物解析使用逐级回退结构：`data` 直接字段 → `user_info` → `batch_combo_send` → `combo_send`。仅这些对象被视为送礼者来源；`receive_user_info` 是收礼者信息，不得用作发送者。礼物数量字段顺序由 `gift_read_count()` 统一维护。

## 6. 条件分支与函数复杂度约定

为便于审阅和避免继续出现截图中的多层 `if`：

- 一个函数内 `if` 的**直接嵌套深度最多为 2 层**；不得新增第三层嵌套。
- 参数检查和错误处理优先使用 guard clause（检查失败立即 `return` / `goto cleanup`）。
- 同类 JSON 字段回退、资源释放、状态映射等逻辑应提取成小型静态辅助函数。
- 多分支状态选择优先使用 `switch` 或明确的查表映射，不要堆叠多层 `if/else`。
- 资源创建与销毁应有统一清理出口，并在函数注释中说明谁拥有资源。
- 注释描述真实所有权/线程语义，修改行为时同步更新注释，不写无法验证的“保证零延迟”等绝对承诺。

当前源码的 braced-if 静态扫描结果：五个 C 源文件的 `if` 最大直接嵌套深度均为 **2**；礼物解析中的重复嵌套已经拆分为 `gift_copy_*`、`gift_read_sender_member()` 和 `gift_read_count()` 辅助函数。

## 7. 构建输出目录

项目根目录执行：

```bash
make clean
make -j"$(nproc)"
./bin/danmu_desktop
```

- `include/`：公共头文件。
- `src/`：C 源文件及 CSS。
- `build/`：`.o` 和自动生成的 `.d` 依赖文件。
- `bin/`：最终可执行文件。
