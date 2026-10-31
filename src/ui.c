/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Copyright (C) 2026 Kuimuztion
 *
 * GTK4 原生桌面弹幕悬浮窗口与气泡渲染实现
 */

#include "ui.h"
#include "avatar.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <cairo.h>
#include <zlib.h>
#include <gtk/gtk.h>
#ifdef HAVE_GTK4_LAYER_SHELL
#include <gtk4-layer-shell.h>
#endif

/*
 * 计算 UID 的 8 位十六进制 CRC32 哈希，若本身为哈希格式直接提取
 */
/**
 * @brief 计算 UID 的 CRC32 标签，兼容已经带 hash: 前缀的匿名标识。
 * @param uid 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param out 调用者拥有的可写输出缓冲区；本函数只写入有界结果，不释放该缓冲区。
 * @param out_size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void get_uid_crc32(const char *uid, char *out, size_t out_size)
{
	uLong crc;

	if (!uid || !out || out_size == 0)
		return;

	out[0] = '\0';
	if (strncmp(uid, "hash:", 5) == 0) {
		g_strlcpy(out, uid + 5, out_size);
		return;
	}

	crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, (const Bytef *)uid, (uInt)strlen(uid));
	snprintf(out, out_size, "%08x", (unsigned int)crc);
}

/*
 * GTK 完成本帧布局后再置底，避免在旧的 adjustment 范围内滚动，
 * 导致最新一条弹幕被裁在视口底边之外。
 */
/**
 * @brief 在 GTK 帧时钟回调中等待布局提交，再设置垂直滚动条的最终值。
 * @param widget GTK Widget 指针；由 GTK 父子容器/对象系统管理，本函数仅借用并在 GTK 主线程操作。
 * @param frame_clock GTK/GDK 对象指针；由 GTK/GDK 对象系统管理，本函数仅借用。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return GLib 回调控制值：G_SOURCE_REMOVE 表示移除源，G_SOURCE_CONTINUE 表示继续。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static gboolean scroll_to_bottom_tick(GtkWidget *widget,
				      GdkFrameClock *frame_clock,
				      gpointer user_data)
{
	struct app_state *state = user_data;
	GtkAdjustment *vadj;
	double upper;
	double page;
	double lower;

	(void)frame_clock;
	if (!state)
		return G_SOURCE_REMOVE;

	if (!state->auto_scroll || !GTK_IS_SCROLLED_WINDOW(widget)) {
		state->scroll_tick_id = 0;
		state->scroll_tick_pass = 0;
		return G_SOURCE_REMOVE;
	}

	/* 第一帧只让 GTK 提交新增行的测量/布局，下一帧读取最终范围。 */
	if (state->scroll_tick_pass++ == 0) {
		gtk_widget_queue_draw(widget);
		return G_SOURCE_CONTINUE;
	}

	state->scroll_tick_id = 0;
	state->scroll_tick_pass = 0;
	vadj = gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(widget));
	if (!vadj)
		return G_SOURCE_REMOVE;

	lower = gtk_adjustment_get_lower(vadj);
	upper = gtk_adjustment_get_upper(vadj);
	page = gtk_adjustment_get_page_size(vadj);
	gtk_adjustment_set_value(vadj, MAX(lower, upper - page));

	return G_SOURCE_REMOVE;
}

/* 合并同一帧内的多次置底请求，避免弹幕密集时重复滚动。 */
/**
 * @brief 合并当前帧内的滚动请求，避免弹幕密集时反复注册 tick 回调。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void schedule_scroll_to_bottom(struct app_state *state)
{
	if (!state || !state->auto_scroll || !state->scrolled_win ||
	    state->scroll_tick_id != 0)
		return;

	state->scroll_tick_pass = 0;
	state->scroll_tick_id = gtk_widget_add_tick_callback(
		state->scrolled_win, scroll_to_bottom_tick, state, NULL);
}

/* 内容高度发生变化时，延后至下一帧统一滚动。 */
/**
 * @brief 响应列表内容高度变化，并安排在后续布局帧自动置底。
 * @param adj GTK 滚动调整对象；由 GtkScrolledWindow 管理，本函数只借用。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void on_vadjustment_changed(GtkAdjustment *adj, gpointer user_data)
{
	struct app_state *state = user_data;

	(void)adj;
	schedule_scroll_to_bottom(state);
}

/*
 * 清空所有已显示的弹幕气泡
 */
/**
 * @brief 销毁旧列表容器并创建空列表，同时将滚动位置复位到顶部。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
void ui_clear_danmu(struct app_state *state)
{
	GtkWidget *new_box;
	GtkAdjustment *vadj;

	if (!state || !state->scrolled_win)
		return;

	/* 创建全新空白列表容器并替换旧列表，彻底销毁旧渲染树缓存 */
	new_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_add_css_class(new_box, "danmu-list-box");

	state->list_box = new_box;
	gtk_scrolled_window_set_child(
		GTK_SCROLLED_WINDOW(state->scrolled_win), new_box);

	vadj = gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(state->scrolled_win));
	if (vadj)
		gtk_adjustment_set_value(vadj, 0.0);

	gtk_widget_queue_resize(state->scrolled_win);
	if (state->window) {
		gtk_widget_queue_resize(state->window);
		gtk_widget_queue_draw(state->window);
	}
}

/*
 * 加载应用全局 CSS 样式表（以最高用户优先级应用以穿透默认主题）
 */
/**
 * @brief 加载项目 CSS 文件并将样式提供器注册到当前 GDK Display。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void load_css(void)
{
	GtkCssProvider *provider;
	static const char *css_path = "src/style.css";

	provider = gtk_css_provider_new();
	if (access(css_path, R_OK) == 0)
		gtk_css_provider_load_from_path(provider, css_path);

	gtk_style_context_add_provider_for_display(
		gdk_display_get_default(),
		GTK_STYLE_PROVIDER(provider),
		GTK_STYLE_PROVIDER_PRIORITY_USER);
	g_object_unref(provider);
}

/*
 * 设置桌面窗口是否保持置顶（通过 KWin 脚本实现全屏防遮挡与窗口属性配置）
 */
/**
 * @brief 通过 Layer Shell 或 KWin 脚本设置悬浮窗置顶行为。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param enable 功能开关；true 表示启用对应窗口行为，false 表示关闭。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
bool ui_set_keep_above(struct app_state *state, bool enable)
{
	static const char js_path[] = "/tmp/kwin_danmu_top.js";
	char script[2048];
	char cmd[1024];
	const char *val;
	const char *raise_cmd;
	const char *listeners;
	int ret;

	if (state && state->is_layer_shell) {
		state->keep_above = enable;
		return true;
	}

	/* 无论开启还是关闭，均先卸载先前的常驻脚本实例 */
	system("busctl --user call org.kde.KWin /Scripting "
	       "org.kde.kwin.Scripting unloadScript s "
	       "bilibili_danmu_keep_above >/dev/null 2>&1");

	val = enable ? "true" : "false";
	raise_cmd = enable ?
		"            workspace.raiseWindow(w);\n" : "";
	listeners = enable ?
		"workspace.windowActivated.connect("
		"function(w) { enforceDanmuTop(); });\n"
		"workspace.windowAdded.connect("
		"function(w) { enforceDanmuTop(); });\n" : "";

	snprintf(script, sizeof(script),
		 "function enforceDanmuTop() {\n"
		 "    var wins = workspace.windowList();\n"
		 "    for (var i = 0; i < wins.length; i++) {\n"
		 "        var w = wins[i];\n"
		 "        if (!w)\n"
		 "            continue;\n"
		 "        if (w.caption === 'Bilibili 桌面弹幕' ||\n"
		 "            w.resourceClass === "
		 "'com.kuimuztion.bilibili_danmu_desktop' ||\n"
		 "            w.resourceName === 'danmu_desktop') {\n"
		 "            w.keepAbove = %s;\n"
		 "            w.skipTaskbar = %s;\n"
		 "            w.skipPager = %s;\n"
		 "            w.skipSwitcher = %s;\n"
		 "%s"
		 "        }\n"
		 "    }\n"
		 "}\n"
		 "%s"
		 "enforceDanmuTop();\n",
		 val, val, val, val, raise_cmd, listeners);

	if (!g_file_set_contents(js_path, script, -1, NULL))
		return false;

	snprintf(cmd, sizeof(cmd),
		 "busctl --user call org.kde.KWin /Scripting "
		 "org.kde.kwin.Scripting loadScript ss "
		 "\"%s\" \"bilibili_danmu_keep_above\" >/dev/null 2>&1 && "
		 "busctl --user call org.kde.KWin /Scripting "
		 "org.kde.kwin.Scripting start >/dev/null 2>&1",
		 js_path);

	ret = system(cmd);
	unlink(js_path);

	if (state)
		state->keep_above = enable;

	return (ret == 0);
}

/*
 * 窗口呈现后的初始置顶配置延时回调
 */
/**
 * @brief 延迟回调：窗口呈现后应用初始置顶策略。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return GLib 回调控制值：G_SOURCE_REMOVE 表示移除源，G_SOURCE_CONTINUE 表示继续。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static gboolean apply_keep_above_idle(gpointer user_data)
{
	struct app_state *state = user_data;

	ui_set_keep_above(state, true);
	return G_SOURCE_REMOVE;
}

/*
 * 同步更新 Wayland/X11 窗口的鼠标输入交互区域（穿透或接收）
 */
/**
 * @brief 更新 GdkSurface 输入区域，实现全穿透、局部交互或常规窗口输入。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void update_input_region(struct app_state *state)
{
	GtkNative *native;
	GdkSurface *surface;
	cairo_region_t *region;
	cairo_rectangle_int_t rect;

	if (!state || !state->window)
		return;

	native = gtk_widget_get_native(state->window);
	surface = native ? gtk_native_get_surface(native) : NULL;
	if (!surface)
		return;

	/* 穿透模式：创建空区域，全屏对鼠标完全穿透 */
	if (!state->mouse_interactive) {
		region = cairo_region_create();
		gdk_surface_set_input_region(surface, region);
		cairo_region_destroy(region);
		gdk_surface_request_layout(surface);
		gtk_widget_queue_draw(state->window);
		return;
	}

	/* 交互模式：Layer Shell 下仅弹幕容器矩形区域接收输入 */
	if (state->is_layer_shell) {
		rect.x = state->margin_x;
		rect.y = state->margin_y;
		rect.width = 420;
		rect.height = 750;
		region = cairo_region_create_rectangle(&rect);
		gdk_surface_set_input_region(surface, region);
		cairo_region_destroy(region);
		gdk_surface_request_layout(surface);
		gtk_widget_queue_draw(state->window);
		return;
	}

	/* 兼容降级模式：恢复整个常规窗口接收鼠标输入 */
	gdk_surface_set_input_region(surface, NULL);
	gdk_surface_request_layout(surface);
	gtk_widget_queue_draw(state->window);
}

/*
 * 设置桌面窗口鼠标交互与穿透模式（开启允许拖拽，关闭完全穿透）
 */
/**
 * @brief 切换窗口鼠标可交互状态，并同步 GTK 目标事件和底层输入区域。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param enable 功能开关；true 表示启用对应窗口行为，false 表示关闭。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
bool ui_set_mouse_interactive(struct app_state *state, bool enable)
{
	GtkNative *native;
	GdkSurface *surface;

	if (!state || !state->window)
		return false;

	state->mouse_interactive = enable;
	gtk_widget_set_can_target(state->window, enable ? TRUE : FALSE);

	update_input_region(state);

	native = gtk_widget_get_native(state->window);
	surface = native ? gtk_native_get_surface(native) : NULL;
	if (surface)
		gdk_surface_request_layout(surface);

	gtk_widget_queue_draw(state->window);
	return true;
}

/*
 * 窗口呈现后的初始鼠标交互模式延时回调
 */
/**
 * @brief 延迟回调：窗口创建后应用初始鼠标交互模式。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return GLib 回调控制值：G_SOURCE_REMOVE 表示移除源，G_SOURCE_CONTINUE 表示继续。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static gboolean apply_mouse_interactive_idle(gpointer user_data)
{
	struct app_state *state = user_data;

	ui_set_mouse_interactive(state, state->mouse_interactive);
	return G_SOURCE_REMOVE;
}

/*
 * 鼠标拖拽起始处理 (碰撞检测与独占手势序列声明)
 */
/**
 * @brief 记录拖拽起点并校验当前输入模式，开始移动固定画布中的弹幕容器。
 * @param gesture GTK/GDK 对象指针；由 GTK/GDK 对象系统管理，本函数仅借用。
 * @param x 目标窗口/拖拽点的水平坐标，单位为 GTK 逻辑像素。
 * @param y 目标窗口/拖拽点的垂直坐标，单位为 GTK 逻辑像素。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void on_layer_drag_begin(GtkGestureDrag *gesture, double x, double y,
				gpointer user_data)
{
	struct app_state *state = user_data;

	if (!state || !state->mouse_interactive)
		return;

	/* 碰撞检测：必须严格点击在弹幕容器矩形范围之内，否则不启动拖拽 */
	if (x < state->margin_x || x > state->margin_x + 420 ||
	    y < state->margin_y || y > state->margin_y + 750) {
		gtk_gesture_set_state(GTK_GESTURE(gesture),
				      GTK_EVENT_SEQUENCE_DENIED);
		return;
	}

	/* 右侧纵向滚动条区域点击保留给滚动条操作，不抢占为窗口拖拽 */
	if (x >= state->margin_x + 420 - 24) {
		gtk_gesture_set_state(GTK_GESTURE(gesture),
				      GTK_EVENT_SEQUENCE_DENIED);
		return;
	}

	/* 独占声明该指针手势序列，启动平滑拖拽 */
	gtk_gesture_set_state(GTK_GESTURE(gesture),
			      GTK_EVENT_SEQUENCE_CLAIMED);

	state->drag_start_x = state->margin_x;
	state->drag_start_y = state->margin_y;
	state->is_dragging = true;
}

/*
 * 鼠标拖拽平移处理（实时平移固定容器位置，零 IPC 阻塞、零抖动）
 */
/**
 * @brief 根据 GtkGestureDrag 相对起点的偏移更新容器位置并刷新输入区域。
 * @param gesture GTK/GDK 对象指针；由 GTK/GDK 对象系统管理，本函数仅借用。
 * @param offset_x 相对起始位置的水平偏移量，单位为 GTK 逻辑像素。
 * @param offset_y 相对起始位置的垂直偏移量，单位为 GTK 逻辑像素。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void on_layer_drag_update(GtkGestureDrag *gesture, double offset_x,
				 double offset_y, gpointer user_data)
{
	struct app_state *state = user_data;
	int nx;
	int ny;

	(void)gesture;
	if (!state || !state->mouse_interactive || !state->is_dragging)
		return;

	nx = state->drag_start_x + (int)offset_x;
	ny = state->drag_start_y + (int)offset_y;

	if (nx < 0)
		nx = 0;
	if (ny < 0)
		ny = 0;

	state->margin_x = nx;
	state->margin_y = ny;

	if (state->fixed && state->danmu_container)
		gtk_fixed_move(GTK_FIXED(state->fixed), state->danmu_container,
			       nx, ny);

	if (state->window)
		gtk_widget_queue_draw(state->window);
}

/*
 * 鼠标拖拽结束处理（单次更新输入区域，保证拖拽全程零卡顿）
 */
/**
 * @brief 结束拖拽手势，提交最终窗口位置并清理拖拽状态。
 * @param gesture GTK/GDK 对象指针；由 GTK/GDK 对象系统管理，本函数仅借用。
 * @param offset_x 相对起始位置的水平偏移量，单位为 GTK 逻辑像素。
 * @param offset_y 相对起始位置的垂直偏移量，单位为 GTK 逻辑像素。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void on_layer_drag_end(GtkGestureDrag *gesture, double offset_x,
			      double offset_y, gpointer user_data)
{
	struct app_state *state = user_data;

	(void)gesture;
	(void)offset_x;
	(void)offset_y;
	if (!state)
		return;

	state->is_dragging = false;
	update_input_region(state);
}

/*
 * 皮肤图标相对与绝对路径配置
 */
static const char * const s_skin_rel_icons[DANMU_SKIN_COUNT] = {
	[DANMU_SKIN_GOOGLE] = "assets/google_icon.svg",
	[DANMU_SKIN_OPENAI] = "assets/openai_icon.svg",
	[DANMU_SKIN_ANTHROPIC] = "assets/anthropic_icon.svg",
	[DANMU_SKIN_DEEPSEEK] = "assets/deepseek_icon.svg"
};



/*
 * 获取指定皮肤主题的唯一英文标识名称
 */
/**
 * @brief 将皮肤枚举转换为配置/终端使用的稳定名称。
 * @param skin 皮肤主题枚举值，必须是 DANMU_SKIN_* 范围内的有效成员。
 * @return 返回指向静态只读主题字符串的指针；字符串由本模块静态存储，调用者不得 free。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
const char *ui_skin_name(enum danmu_skin skin)
{
	switch (skin) {
	case DANMU_SKIN_GOOGLE:
		return "google";
	case DANMU_SKIN_OPENAI:
		return "openai";
	case DANMU_SKIN_ANTHROPIC:
		return "anthropic";
	case DANMU_SKIN_DEEPSEEK:
		return "deepseek";
	default:
		return "google";
	}
}

/*
 * 获取指定皮肤主题的中文展示描述名称
 */
/**
 * @brief 返回皮肤主题的中文描述文本。
 * @param skin 皮肤主题枚举值，必须是 DANMU_SKIN_* 范围内的有效成员。
 * @return 返回指向静态只读主题字符串的指针；字符串由本模块静态存储，调用者不得 free。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
const char *ui_skin_desc(enum danmu_skin skin)
{
	switch (skin) {
	case DANMU_SKIN_GOOGLE:
		return "Google (经典四色微光)";
	case DANMU_SKIN_OPENAI:
		return "OpenAI (极客黑曜石绿白)";
	case DANMU_SKIN_ANTHROPIC:
		return "Anthropic (暖陶土与珊瑚沙岩)";
	case DANMU_SKIN_DEEPSEEK:
		return "DeepSeek (深海电光蓝与跃动鲸蓝)";
	default:
		return "Google (经典四色微光)";
	}
}

/*
 * 获取指定皮肤对应的品牌矢量图标路径
 */
/**
 * @brief 返回皮肤主题对应 SVG 图标的项目内路径。
 * @param skin 皮肤主题枚举值，必须是 DANMU_SKIN_* 范围内的有效成员。
 * @return 返回指向静态只读主题字符串的指针；字符串由本模块静态存储，调用者不得 free。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
const char *ui_skin_icon_path(enum danmu_skin skin)
{
	if (skin >= DANMU_SKIN_COUNT)
		skin = DANMU_SKIN_GOOGLE;

	if (access(s_skin_rel_icons[skin], R_OK) == 0)
		return s_skin_rel_icons[skin];

	return s_skin_rel_icons[skin];
}

/*
 * 解析皮肤名称字符串并返回对应的枚举类型（不区分大小写）
 */
/**
 * @brief 将用户输入的主题名称解析为皮肤枚举，未知值回退到默认主题。
 * @param name 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return 返回对应皮肤主题枚举值。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
enum danmu_skin ui_parse_skin(const char *name)
{
	if (!name || name[0] == '\0')
		return DANMU_SKIN_COUNT;

	if (g_ascii_strcasecmp(name, "google") == 0)
		return DANMU_SKIN_GOOGLE;

	if (g_ascii_strcasecmp(name, "openai") == 0 ||
	    g_ascii_strcasecmp(name, "gpt") == 0 ||
	    g_ascii_strcasecmp(name, "chatgpt") == 0)
		return DANMU_SKIN_OPENAI;

	if (g_ascii_strcasecmp(name, "anthropic") == 0 ||
	    g_ascii_strcasecmp(name, "claude") == 0)
		return DANMU_SKIN_ANTHROPIC;

	if (g_ascii_strcasecmp(name, "deepseek") == 0 ||
	    g_ascii_strcasecmp(name, "r1") == 0)
		return DANMU_SKIN_DEEPSEEK;

	return DANMU_SKIN_COUNT;
}

/*
 * 更新单行弹幕气泡中的品牌图标
 */
/**
 * @brief 替换单条气泡内品牌图标，同时沿用当前 GTK Widget 层级。
 * @param row GTK Widget 指针；由 GTK 父子容器/对象系统管理，本函数仅借用并在 GTK 主线程操作。
 * @param icon_path 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void update_row_skin_icon(GtkWidget *row, const char *icon_path)
{
	GtkWidget *img;

	img = g_object_get_data(G_OBJECT(row), "brand_img");
	if (img && GTK_IS_IMAGE(img))
		gtk_image_set_from_file(GTK_IMAGE(img), icon_path);
}

/*
 * 更新全部已显示气泡的品牌图标
 */
/**
 * @brief 遍历弹幕列表中的现有行并逐个刷新主题图标。
 * @param list_box GTK Widget 指针；由 GTK 父子容器/对象系统管理，本函数仅借用并在 GTK 主线程操作。
 * @param icon_path 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void update_all_bubbles_skin(GtkWidget *list_box, const char *icon_path)
{
	GtkWidget *child;

	if (!list_box)
		return;

	child = gtk_widget_get_first_child(list_box);
	while (child) {
		update_row_skin_icon(child, icon_path);
		child = gtk_widget_get_next_sibling(child);
	}
}

/*
 * 更新容器上的皮肤 CSS 样式类
 */
/**
 * @brief 更新弹幕容器的主题 CSS class，供 CSS 重新选择外观。
 * @param container GTK Widget 指针；由 GTK 父子容器/对象系统管理，本函数仅借用并在 GTK 主线程操作。
 * @param skin 皮肤主题枚举值，必须是 DANMU_SKIN_* 范围内的有效成员。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void update_container_skin_class(GtkWidget *container,
					enum danmu_skin skin)
{
	static const char * const skin_classes[] = {
		"skin-google",
		"skin-openai",
		"skin-anthropic",
		"skin-deepseek"
	};
	size_t i;

	if (!container)
		return;

	for (i = 0; i < sizeof(skin_classes) / sizeof(skin_classes[0]); i++)
		gtk_widget_remove_css_class(container, skin_classes[i]);

	if (skin < DANMU_SKIN_COUNT)
		gtk_widget_add_css_class(container, skin_classes[skin]);
}

/*
 * 切换全局弹幕皮肤主题并即时刷新当前所有气泡图标与容器样式
 */
/**
 * @brief 更新应用主题状态并刷新容器 class、现有气泡图标和窗口绘制。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param skin 皮肤主题枚举值，必须是 DANMU_SKIN_* 范围内的有效成员。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
bool ui_set_skin(struct app_state *state, enum danmu_skin skin)
{
	const char *icon_path;

	if (!state || skin >= DANMU_SKIN_COUNT)
		return false;

	state->current_skin = skin;
	icon_path = ui_skin_icon_path(skin);

	update_container_skin_class(state->danmu_container, skin);
	update_all_bubbles_skin(state->list_box, icon_path);

	if (state->window)
		gtk_widget_queue_draw(state->window);

	return true;
}

/*
 * 初始化并构建 GTK4 桌面弹幕无边框透明主窗口
 */
/**
 * @brief 创建透明 GTK 主窗口、Layer Shell Overlay、弹幕列表和拖拽控制器。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param app GtkApplication 实例；由 GTK 应用框架持有，本函数仅借用。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
bool ui_init(struct app_state *state, GtkApplication *app)
{
	/* drag/vadj 均为 GTK 管理的对象指针；本函数只借用，不手工释放。 */
	GtkGesture *drag;       /* 绑定到固定全屏画布的 GtkGestureDrag 控制器。 */
	GtkAdjustment *vadj;    /* scrolled_win 的垂直滚动调整对象。 */

	if (!state || !app)
		return false;

	load_css();

	state->window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(state->window),
			     "Bilibili 桌面弹幕");
	gtk_window_set_default_size(GTK_WINDOW(state->window), 420, 750);

	/* 移除系统标题栏与装饰按钮 */
	gtk_window_set_decorated(GTK_WINDOW(state->window), FALSE);

#ifdef HAVE_GTK4_LAYER_SHELL
	if (gtk_layer_is_supported()) {
		state->is_layer_shell = true;
		gtk_layer_init_for_window(GTK_WINDOW(state->window));
		gtk_layer_set_layer(GTK_WINDOW(state->window),
				    GTK_LAYER_SHELL_LAYER_OVERLAY);
		gtk_layer_set_keyboard_mode(
			GTK_WINDOW(state->window),
			GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
		gtk_layer_set_namespace(GTK_WINDOW(state->window),
					"bilibili-danmu-desktop");
		gtk_layer_set_anchor(GTK_WINDOW(state->window),
				     GTK_LAYER_SHELL_EDGE_TOP, TRUE);
		gtk_layer_set_anchor(GTK_WINDOW(state->window),
				     GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
		gtk_layer_set_anchor(GTK_WINDOW(state->window),
				     GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
		gtk_layer_set_anchor(GTK_WINDOW(state->window),
				     GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
	}
#endif

	if (state->margin_x <= 0)
		state->margin_x = 40;
	if (state->margin_y <= 0)
		state->margin_y = 60;

	state->target_x = state->margin_x;
	state->target_y = state->margin_y;
	state->is_dragging = false;

	/* 主窗口挂载全屏透明 Overlay 固定画布 */
	state->fixed = gtk_fixed_new();
	gtk_widget_set_hexpand(state->fixed, TRUE);
	gtk_widget_set_vexpand(state->fixed, TRUE);
	gtk_window_set_child(GTK_WINDOW(state->window), state->fixed);

	/* 创建独立弹幕悬浮容器，尺寸设为 420x750 */
	state->danmu_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_size_request(state->danmu_container, 420, 750);
	gtk_widget_add_css_class(state->danmu_container, "danmu-container");
	ui_set_skin(state, state->current_skin);

	/* 弹幕滚动列表区域 */
	state->scrolled_win = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(state->scrolled_win),
				       GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_kinetic_scrolling(
		GTK_SCROLLED_WINDOW(state->scrolled_win), FALSE);
	gtk_widget_add_css_class(state->scrolled_win, "danmu-scrolled-win");
	gtk_widget_set_vexpand(state->scrolled_win, TRUE);
	gtk_widget_set_hexpand(state->scrolled_win, TRUE);

	state->list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_add_css_class(state->list_box, "danmu-list-box");
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(state->scrolled_win),
				      state->list_box);

	vadj = gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(state->scrolled_win));
	if (vadj)
		g_signal_connect(vadj, "changed",
				 G_CALLBACK(on_vadjustment_changed), state);

	gtk_box_append(GTK_BOX(state->danmu_container), state->scrolled_win);

	/* 挂载弹幕容器至固定画布的 (margin_x, margin_y) 坐标 */
	gtk_fixed_put(GTK_FIXED(state->fixed), state->danmu_container,
		      state->margin_x, state->margin_y);

	/*
	 * 拖拽手势挂在固定不动的全屏画布上，而不是随之移动的弹幕容器。
	 * GtkGestureDrag 的偏移量相对挂载控件的本地坐标计算，挂在移动
	 * 对象上时，容器每次挪动都会抵消下一次的偏移，表现为位置只跟
	 * 随一半位移并在两个位置间来回跳动。挂在画布上坐标系不变，偏移
	 * 量即为指针的真实位移。捕获阶段仍覆盖整个弹幕区域，任意位置
	 * 均可按住拖拽。
	 */
	drag = gtk_gesture_drag_new();
	gtk_event_controller_set_propagation_phase(
		GTK_EVENT_CONTROLLER(drag), GTK_PHASE_CAPTURE);
	g_signal_connect(drag, "drag-begin",
			 G_CALLBACK(on_layer_drag_begin), state);
	g_signal_connect(drag, "drag-update",
			 G_CALLBACK(on_layer_drag_update), state);
	g_signal_connect(drag, "drag-end",
			 G_CALLBACK(on_layer_drag_end), state);
	g_signal_connect(drag, "cancel",
			 G_CALLBACK(on_layer_drag_end), state);
	gtk_widget_add_controller(state->fixed,
				  GTK_EVENT_CONTROLLER(drag));

	gtk_window_present(GTK_WINDOW(state->window));
	update_input_region(state);

	if (state->keep_above) {
		g_timeout_add(300, apply_keep_above_idle, state);
		g_timeout_add(1200, apply_keep_above_idle, state);
	}

	g_timeout_add(150, apply_mouse_interactive_idle, state);
	g_timeout_add(600, apply_mouse_interactive_idle, state);

	return true;
}

/*
 * 模拟窗口拖拽位移并测试 GTK 渲染树重排延迟与耗时
 */
/**
 * @brief 按指定步数模拟窗口位移，并测量 GTK 重排/移动调用的耗时。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param delta_x 模拟拖拽的水平总位移，单位为 GTK 逻辑像素。
 * @param delta_y 模拟拖拽的垂直总位移，单位为 GTK 逻辑像素。
 * @param steps 将总位移拆分模拟的步数；值越大，单步位移越小。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
void ui_simulate_drag(struct app_state *state, int delta_x, int delta_y,
		      int steps)
{
	struct timespec t_start;
	struct timespec t_end;
	int64_t total_ns = 0;
	int64_t max_ns = 0;
	int64_t diff_ns;
	int start_x;
	int start_y;
	int cur_x;
	int cur_y;
	int i;

	if (!state || !state->fixed || !state->danmu_container)
		return;

	if (steps <= 0)
		steps = 1;

	start_x = state->margin_x;
	start_y = state->margin_y;

	for (i = 0; i < steps; i++) {
		cur_x = start_x + (delta_x * (i + 1)) / steps;
		cur_y = start_y + (delta_y * (i + 1)) / steps;
		if (cur_x < 0)
			cur_x = 0;
		if (cur_y < 0)
			cur_y = 0;

		clock_gettime(CLOCK_MONOTONIC, &t_start);

		state->margin_x = cur_x;
		state->margin_y = cur_y;
		gtk_fixed_move(GTK_FIXED(state->fixed), state->danmu_container,
			       cur_x, cur_y);
		if (state->window)
			gtk_widget_queue_draw(state->window);

		clock_gettime(CLOCK_MONOTONIC, &t_end);

		diff_ns = (int64_t)(t_end.tv_sec - t_start.tv_sec) *
			  1000000000LL + (t_end.tv_nsec - t_start.tv_nsec);
		total_ns += diff_ns;
		if (diff_ns > max_ns)
			max_ns = diff_ns;
	}

	update_input_region(state);

	printf("[ui_simulate_drag] %d 步模拟拖拽完成: 位移 (%d, %d), "
	       "平均耗时: %.2f us, 最大耗时: %.2f us\n",
	       steps, delta_x, delta_y,
	       (double)total_ns / (double)steps / 1000.0,
	       (double)max_ns / 1000.0);
	fflush(stdout);
}

/*
 * 构造并挂载单条高质感弹幕气泡至列表容器
 */
/**
 * @brief 构建单条弹幕 GTK Widget 树、挂载至列表、修剪历史行并安排置底。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param item 弹幕结构体；只读输入时不修改，异步派发前按值复制。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void ui_render_danmu_bubble(struct app_state *state,
				   const struct danmu_item *item)
{
	/*
	 * 以下 GtkWidget* 均为 GTK 构建/遍历得到的对象指针，不由 C free()。
	 * row 是最终挂入 list_box 的根节点；其子控件经 gtk_box_append()
	 * 挂入父容器后由 GTK 对象树持有引用。
	 * avatar_img/uname_lbl/google_img 通过 g_object_set_data() 保存为
	 * row 的非拥有型快捷引用，生命周期不超过 row 的子树。
	 * child/first 是 GTK 遍历 API 返回的借用指针，移除后立即失效。
	 */
	GtkWidget *row;
	GtkWidget *avatar_img;
	GtkWidget *bubble_border;
	GtkWidget *bubble_content;
	GtkWidget *uname_lbl;
	GtkWidget *text_lbl;
	GtkWidget *google_box;
	GtkWidget *google_img;
	GtkWidget *child;
	GtkWidget *first;
	char hash_buf[32] = "";
	int item_count = 0;

	if (!state || !state->list_box || !item)
		return;

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_widget_add_css_class(row, "danmu-row");
	gtk_widget_set_valign(row, GTK_ALIGN_START);

	/* 气泡全区域不单独截获事件，使鼠标放在气泡上任意位置都能直接拖拽 */
	gtk_widget_set_can_target(row, FALSE);

	/*
	 * UID/hash 以 g_strdup() 创建副本，并通过 g_object_set_data_full()
	 * 指定 g_free；row 被销毁时这些字符串会自动释放。
	 */
	g_object_set_data_full(G_OBJECT(row), "danmu_uid",
			       g_strdup(item->uid), g_free);
	get_uid_crc32(item->uid, hash_buf, sizeof(hash_buf));
	if (hash_buf[0] != '\0')
		g_object_set_data_full(G_OBJECT(row), "danmu_hash",
				       g_strdup(hash_buf), g_free);

	/*
	 * GTK 渲染树（从左到右）：row -> avatar_img + bubble_border ->
	 * bubble_content -> uname_lbl、text_lbl、google_box -> google_img。
	 * gtk_box_append() 把子控件挂入父控件，父控件负责保持其对象引用。
	 */
	avatar_img = gtk_image_new_from_file(item->avatar_path);
	gtk_image_set_pixel_size(GTK_IMAGE(avatar_img), 46);
	gtk_widget_set_size_request(avatar_img, 46, 46);
	gtk_widget_set_valign(avatar_img, GTK_ALIGN_START);
	gtk_widget_set_halign(avatar_img, GTK_ALIGN_START);
	gtk_widget_set_hexpand(avatar_img, FALSE);
	gtk_widget_set_vexpand(avatar_img, FALSE);
	gtk_widget_add_css_class(avatar_img, "danmu-avatar");
	g_object_set_data(G_OBJECT(row), "avatar_widget", avatar_img);
	gtk_box_append(GTK_BOX(row), avatar_img);

	/* 气泡外圈：Google 四色环绕渐变边框容器 */
	bubble_border = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(bubble_border, "danmu-bubble-border");
	gtk_widget_set_hexpand(bubble_border, TRUE);

	/* 气泡内层：深色微透毛玻璃内容容器 */
	bubble_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
	gtk_widget_add_css_class(bubble_content, "danmu-bubble-content");
	gtk_widget_set_hexpand(bubble_content, TRUE);

	uname_lbl = gtk_label_new(item->uname);
	gtk_widget_add_css_class(uname_lbl, "danmu-uname");
	gtk_label_set_xalign(GTK_LABEL(uname_lbl), 0.0);
	g_object_set_data(G_OBJECT(row), "uname_label", uname_lbl);
	gtk_box_append(GTK_BOX(bubble_content), uname_lbl);

	if (strstr(item->text, "<span")) {
		text_lbl = gtk_label_new(NULL);
		gtk_label_set_markup(GTK_LABEL(text_lbl), item->text);
	} else {
		text_lbl = gtk_label_new(item->text);
	}
	gtk_widget_add_css_class(text_lbl, "danmu-text");
	gtk_label_set_wrap(GTK_LABEL(text_lbl), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(text_lbl), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_xalign(GTK_LABEL(text_lbl), 0.0);
	gtk_box_append(GTK_BOX(bubble_content), text_lbl);

	/* 气泡右下角品牌彩色标识（兼容保留 google 样式类） */
	google_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(google_box, "danmu-brand-box");
	gtk_widget_add_css_class(google_box, "danmu-google-box");
	gtk_widget_set_halign(google_box, GTK_ALIGN_END);
	gtk_widget_set_valign(google_box, GTK_ALIGN_END);

	google_img = gtk_image_new_from_file(
		ui_skin_icon_path(state->current_skin));
	gtk_image_set_pixel_size(GTK_IMAGE(google_img), 16);
	gtk_widget_add_css_class(google_img, "danmu-brand-icon");
	gtk_widget_add_css_class(google_img, "danmu-google-icon");
	g_object_set_data(G_OBJECT(row), "brand_img", google_img);
	gtk_box_append(GTK_BOX(google_box), google_img);

	gtk_box_append(GTK_BOX(bubble_content), google_box);

	gtk_box_append(GTK_BOX(bubble_border), bubble_content);
	gtk_box_append(GTK_BOX(row), bubble_border);
	gtk_box_append(GTK_BOX(state->list_box), row);

	/* 控制历史弹幕数量，超出阈值时从顶部修剪 */
	child = gtk_widget_get_first_child(state->list_box);
	while (child) {
		item_count++;
		child = gtk_widget_get_next_sibling(child);
	}

	if (state->max_items <= 0)
		state->max_items = 100;

	while (item_count > state->max_items) {
		first = gtk_widget_get_first_child(state->list_box);
		if (!first)
			break;
		gtk_box_remove(GTK_BOX(state->list_box), first);
		item_count--;
	}

	/* 新弹幕追加完成后，在 GTK 下一帧布局完成时精确置底。 */
	schedule_scroll_to_bottom(state);
}

/*
 * 向弹幕列表追加一条即时弹幕气泡并持久化记录至本地日志
 */
/**
 * @brief 渲染实时弹幕并将其追加持久化日志。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param item 弹幕结构体；只读输入时不修改，异步派发前按值复制。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
void ui_add_danmu(struct app_state *state, const struct danmu_item *item)
{
	if (!state || !item)
		return;

	/* 无论何种来源的即时弹幕，第一时间持久化记录至本地日志文件 */
	util_log_danmu(state->room_id, item->uname, item->uid, item->text,
		       item->timestamp);

	ui_render_danmu_bubble(state, item);
}

/*
 * 恢复渲染一条历史弹幕（严禁调用 util_log_danmu，防止重启重复记录）
 */
/**
 * @brief 仅渲染从历史日志恢复的弹幕，不重复写入持久化日志。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param item 弹幕结构体；只读输入时不修改，异步派发前按值复制。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
void ui_restore_danmu(struct app_state *state, const struct danmu_item *item)
{
	if (!state || !item)
		return;

	ui_render_danmu_bubble(state, item);
}

/*
 * 更新单行弹幕气泡的头像与真实用户名（辅助静态函数）
 */
/**
 * @brief 匹配 UID/hash 对应的气泡行，更新头像图片及可被补全的昵称标签。
 * @param row GTK Widget 指针；由 GTK 父子容器/对象系统管理，本函数仅借用并在 GTK 主线程操作。
 * @param uid 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param avatar_path 只读借用指针；本函数不修改或释放所指向的对象。
 * @param real_uname 只读借用指针；本函数不修改或释放所指向的对象。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void update_row_avatar(GtkWidget *row, const char *uid,
			      const char *avatar_path, const char *real_uname)
{
	/* item_uid/item_hash 是 row 上由 g_object_set_data_full 管理的借用字符串。 */
	const char *item_uid;
	const char *item_hash;
	char target_hash[32] = "";
	/* img/lbl 是 row 子树中的 GTK 控件借用指针，不能 free/unref。 */
	GtkWidget *img;
	GtkWidget *lbl;
	const char *cur; /* gtk_label_get_text() 返回 GTK 持有的借用文本。 */
	GFile *file;     /* g_file_new_for_path() 新引用，必须 g_object_unref。 */
	GdkTexture *tex; /* gdk_texture_new_from_file() 新引用，使用后 unref。 */
	bool matched = false;

	item_uid = g_object_get_data(G_OBJECT(row), "danmu_uid");
	item_hash = g_object_get_data(G_OBJECT(row), "danmu_hash");
	get_uid_crc32(uid, target_hash, sizeof(target_hash));

	if (item_uid && strcmp(item_uid, uid) == 0)
		matched = true;
	else if (item_hash && target_hash[0] != '\0' &&
		 g_ascii_strcasecmp(item_hash, target_hash) == 0)
		matched = true;

	if (!matched)
		return;

	img = g_object_get_data(G_OBJECT(row), "avatar_widget");
	if (img && GTK_IS_IMAGE(img)) {
		file = g_file_new_for_path(avatar_path);
		tex = gdk_texture_new_from_file(file, NULL);
		g_object_unref(file);
		if (tex) {
			gtk_image_set_from_paintable(GTK_IMAGE(img),
						     GDK_PAINTABLE(tex));
			g_object_unref(tex);
		} else {
			gtk_image_set_from_file(GTK_IMAGE(img), avatar_path);
		}
	}

	if (real_uname && real_uname[0] != '\0') {
		lbl = g_object_get_data(G_OBJECT(row), "uname_label");
		if (lbl && GTK_IS_LABEL(lbl)) {
			cur = gtk_label_get_text(GTK_LABEL(lbl));
			if (cur && (strchr(cur, '*') ||
				    strcmp(cur, "B站用户") == 0 ||
				    strcmp(cur, "热心观众") == 0))
				gtk_label_set_text(GTK_LABEL(lbl), real_uname);
		}
	}

	/* 若之前记录的是临时哈希而本次为数字 UID，顺带补全真实 UID */
	if (item_uid && strncmp(item_uid, "hash:", 5) == 0 &&
	    strncmp(uid, "hash:", 5) != 0)
		g_object_set_data_full(G_OBJECT(row), "danmu_uid",
				       g_strdup(uid), g_free);
}

/*
 * 响应头像下载完成通知，动态更新气泡头像与真实用户名
 */
/**
 * @brief 遍历当前 GTK 列表，将后台头像/昵称更新通知应用到匹配行。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param uid 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param avatar_path 只读借用指针；本函数不修改或释放所指向的对象。
 * @param real_uname 只读借用指针；本函数不修改或释放所指向的对象。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
void ui_on_avatar_updated(struct app_state *state, const char *uid,
			  const char *avatar_path, const char *real_uname)
{
	GtkWidget *child;

	if (!state || !state->list_box || !uid || !avatar_path)
		return;

	child = gtk_widget_get_first_child(state->list_box);
	while (child) {
		update_row_avatar(child, uid, avatar_path, real_uname);
		child = gtk_widget_get_next_sibling(child);
	}
}
