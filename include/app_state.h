/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Copyright (C) 2026 Kuimuztion
 *
 * 桌面弹幕应用全局状态定义与数据结构
 */

#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include <gtk/gtk.h>

/*
 * 弹幕消息结构
 */
struct danmu_item {
	/* 纯值类型弹幕快照：跨线程时整体复制，不包含 GTK/GObject 指针。 */
	char uid[64];
	char uname[128];
	char text[512];
	char avatar_path[512];
	int64_t timestamp;
};

/*
 * 弹幕气泡皮肤主题枚举
 */
enum danmu_skin {
	DANMU_SKIN_GOOGLE = 0,
	DANMU_SKIN_OPENAI,
	DANMU_SKIN_ANTHROPIC,
	DANMU_SKIN_DEEPSEEK,
	DANMU_SKIN_COUNT
};

/*
 * 应用配置与全局状态结构
 */
struct app_state {
	/*
	 * app_state 是应用生命周期内的共享状态对象，由 main.c 的静态
	 * g_state 持有。网络/头像线程只借用它；GTK Widget 字段只能由
	 * GTK 主线程读写。lock 用于跨线程共享字段的短临界区保护。
	 */
	long long room_id;
	char identity_code[128];	/* 主播身份码(可选) */
	char cookie[4096];		/* 网页端 Cookie(可选) */
	int font_size;
	int max_items;
	bool auto_scroll;
	bool keep_above;
	bool mouse_interactive;
	enum danmu_skin current_skin;	/* 当前聊天气泡皮肤主题 */

	/* 当前登录账号与直播间房主信息 */
	long long account_uid;		/* Cookie 对应的已登录 B 站账号 UID */
	long long anchor_uid;		/* 直播间主播真实 UID */
	char anchor_uname[128];		/* 直播间主播真实昵称 */

	/* 窗口与拖拽状态 */
	bool is_layer_shell;	/* 是否使用 Layer Shell */
	int margin_x;		/* 窗口或容器水平位置 */
	int margin_y;		/* 窗口或容器垂直位置 */
	int drag_start_x;	/* 拖拽起始水平坐标 */
	int drag_start_y;	/* 拖拽起始垂直坐标 */
	int target_x;		/* 拖拽目标水平坐标 */
	int target_y;		/* 拖拽目标垂直坐标 */
	bool is_dragging;	/* 是否处于拖拽状态 */
	guint tick_callback_id;	/* 垂直同步帧回调标识 */

	gint running;
	bool connected;
	char status_text[128];

	/*
	 * GTK 界面组件指针（借用/由 GTK 对象树持有）：app 是应用实例；
	 * window 是顶层窗口；fixed 是全屏透明固定画布；danmu_container
	 * 是可拖动弹幕面板；scrolled_win/list_box 是滚动视口和弹幕行父容器。
	 * 这些指针不允许从网络或头像 worker 线程直接访问，也不可 free()。
	 */
	GtkApplication *app;
	GtkWidget *window;
	GtkWidget *fixed;		/* 全屏固定画布 (GtkFixed) */
	GtkWidget *danmu_container;	/* 弹幕独立容器 (GtkBox) */
	GtkWidget *scrolled_win;
	GtkWidget *list_box;
	guint scroll_tick_id;	/* 合并同一帧内的自动置底请求 */
	guint scroll_tick_pass;	/* 等待一次布局提交后再执行滚动 */

	/* 线程互斥锁 */
	GMutex lock;
};

#endif /* APP_STATE_H */
