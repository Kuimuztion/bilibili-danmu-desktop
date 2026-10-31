#define _POSIX_C_SOURCE 200809L

/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Copyright (C) 2026 Kuimuztion
 *
 * B站桌面弹幕客户端程序入口、终端交互控制器与生命周期管理
 */

#include "app_state.h"
#include "avatar.h"
#include "bilibili.h"
#include "ui.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <gtk/gtk.h>

struct mock_user {
	const char *uid;
	const char *uname;
	const char *text;
};

/* 轮换测试用户数据（使用真实存在且拥有头像的 B 站 UID） */
static const struct mock_user g_presets[] = {
	{
		"3546843237582920",
		"你猜我在线不",
		"GTK4 原生悬浮弹幕已就绪，全透明浮窗直视桌面！"
	},
	{
		"2",
		"碧诗",
		"哔哩哔哩 (゜-゜)つロ 乾杯~ 真实头像加载成功！"
	},
	{
		"946974",
		"哔哩哔哩弹幕网",
		"欢迎来到直播间！Linux 内核纯 C 高性能渲染中。"
	},
	{
		"473837611",
		"B站直播姬",
		"小姬来巡房啦~ 终端交互控制模式已启动！"
	},
	{
		"3546843237582920",
		"枫月万年",
		"投喂 <span foreground=\"#fbbf24\"><b>粉丝团灯牌</b></span> x66"
	}
};
static int g_preset_idx;

/* g_state 由进程持有到 GTK 应用退出，是所有模块借用的共享状态对象。 */
static struct app_state g_state;
static bool g_opt_test;
static guint g_term_watch_id; /* GLib IO source ID，用于 shutdown 时移除监视器。 */
static GIOChannel *g_term_chan; /* 标准输入通道对象；由 setup/shutdown 生命周期管理。 */

/*
 * 从当前可执行文件位置定位源码树根目录。
 * 构建树中的可执行文件位于 bin/，切换到项目根目录后，config、
 * assets、CSS 与 scripts 均使用一致的相对路径。系统安装副本不会
 * 因为同名 bin 目录而误切换到不存在的项目目录。
 */
/**
 * @brief 根据当前可执行文件的位置定位项目根目录，统一解析 config.json、CSS 与 assets 路径。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void set_project_working_directory(void)
{
	char exe_path[PATH_MAX];
	char *assets_path;
	char *css_path;
	char *slash;
	char *base;
	ssize_t length;

	length = readlink("/proc/self/exe", exe_path,
			  sizeof(exe_path) - 1);
	if (length <= 0 || (size_t)length >= sizeof(exe_path) - 1)
		return;
	exe_path[length] = '\0';

	slash = strrchr(exe_path, '/');
	if (!slash)
		return;
	*slash = '\0';

	base = strrchr(exe_path, '/');
	if (base && strcmp(base + 1, "bin") == 0)
		*base = '\0';

	if (exe_path[0] == '\0')
		g_strlcpy(exe_path, "/", sizeof(exe_path));

	assets_path = g_build_filename(exe_path, "assets", NULL);
	css_path = g_build_filename(exe_path, "src", "style.css", NULL);
	if (access(assets_path, R_OK) == 0 &&
	    access(css_path, R_OK) == 0)
		(void)chdir(exe_path);
	g_free(css_path);
	g_free(assets_path);
}

/*
 * 过滤底层 GTK / GLib 框架警告日志，保持终端控制台清爽
 */
/**
 * @brief 过滤 GTK/GLib 非关键日志，将终端输出保留给应用自己的状态与命令反馈。
 * @param log_level GLib 日志级别枚举值，用于决定该条框架日志是否输出。
 * @param fields 只读借用指针；本函数不修改或释放所指向的对象。
 * @param n_fields 本条 GLib 日志记录包含的字段数量。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return 返回值用于表示本函数计算结果，具体见函数说明。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static GLogWriterOutput suppress_gtk_log_writer(GLogLevelFlags log_level,
						const GLogField *fields,
						gsize n_fields,
						gpointer user_data)
{
	(void)log_level;
	(void)fields;
	(void)n_fields;
	(void)user_data;
	return G_LOG_WRITER_HANDLED;
}

/*
 * 输出命令行选项帮助说明
 */
/**
 * @brief 输出终端交互模式支持的命令、参数和简要用法。
 * @param prog 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void print_help(const char *prog)
{
	printf("用法: %s [选项]\n\n", prog);
	printf("选项:\n");
	printf("  -r, --room <房号>   指定B站直播间号 "
	       "(默认从 config.json 读取)\n");
	printf("  --test              启动后立即注入模拟弹幕用于即时预览\n");
	printf("  -h, --help          显示此帮助信息并退出\n\n");
	printf("交互控制:\n");
	printf("  启动后直接在终端输入 help 查看控制命令。\n");
}

/*
 * 检测当前系统是否已配置全屏防遮挡 OSD 规则
 */
/**
 * @brief 检查 KDE/KWin 全屏 Overlay 规则是否已经安装或启用。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static bool is_overlay_rule_enabled(void)
{
	char val[32];
	FILE *fp;

	memset(val, 0, sizeof(val));
	fp = popen("kreadconfig6 --file kwinrulesrc "
		   "--group \"Bilibili Danmu Overlay\" "
		   "--key type 2>/dev/null", "r");
	if (!fp)
		return false;

	if (fgets(val, sizeof(val), fp)) {
		pclose(fp);
		return (atoi(val) == 16);
	}

	pclose(fp);
	return false;
}

/*
 * 输出终端控制交互说明条幅
 */
/**
 * @brief 读取应用状态并打印房间号、悬浮层模式、日志路径和皮肤等启动信息。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void print_terminal_banner(struct app_state *state)
{
	bool overlay_active;

	overlay_active = !state->is_layer_shell && is_overlay_rule_enabled();

	printf("\n");
	printf("======================================================\n");
	printf("  B站直播桌面弹幕客户端 (终端控制交互模式)\n");
	if (state->room_id > 0) {
		printf("  直播间: %lld | 全透明桌面悬浮窗 | 极简微光气泡\n",
		       state->room_id);
	} else {
		printf("  直播间: 未配置 (请在 config.json 设置 room_id 或"
		       "输入 room <房号>)\n");
	}
	printf("  弹幕日志: /tmp/bilibili_danmu_history.log (持久化保存)\n");
	if (state->is_layer_shell) {
		printf("  全屏防遮挡: 已开启 (原生 Wayland Layer Shell "
		       "Overlay，跨越全屏游戏/视频/F11)\n");
	} else if (overlay_active) {
		printf("  全屏防遮挡: 已开启 "
		       "(KWin OSD浮层，跨越全屏游戏/视频)\n");
	} else {
		printf("  全屏防遮挡: 未开启 (输入 overlay on 启用)\n");
	}
	printf("  气泡皮肤: %s\n", ui_skin_desc(state->current_skin));
	printf("======================================================\n");
	printf("可用命令:\n");
	printf("  help                       - 显示此帮助信息\n");
	printf("  log / history              - 查看弹幕日志统计与近期记录\n");
	printf("  test [内容]                - 发送测试弹幕 (真实用户)\n");
	printf("  test <UID> <用户名> <内容> - 使用指定B站UID发送测试\n");
	printf("  clear                      - 清空桌面弹幕列表\n");
	printf("  skin [名称]                - 一键更换气泡皮肤 "
	       "(google/openai/anthropic/deepseek)\n");
	printf("  scroll [on|off]            - 切换/设置自动置底滚动\n");
	printf("  top [on|off]               - 切换/设置窗口置顶防全屏遮挡\n");
	printf("  overlay [on|off]           - 切换全屏防遮挡 "
	       "(KWin OSD浮动层)\n");
	printf("  mouse [on|off]             - 切换鼠标交互(on:可拖拽 / "
	       "off:鼠标穿透)\n");
	printf("                               (别名: interactive, click, "
	       "drag)\n");
	printf("  drag_test [dx] [dy] [步数]  - "
	       "模拟桌面拖动测试并输出延迟分析报告\n");
	printf("  room <房号>                - 切换连接到新的B站直播间\n");
	printf("  status                     - 查看当前连接状态与统计\n");
	printf("  quit / exit                - 退出程序\n");
	printf("======================================================\n");
	printf("danmu> ");
	fflush(stdout);
}

/*
 * 从本地配置文件读取并覆盖初始设置
 */
/**
 * @brief 从项目配置文件读取房间号、Cookie、皮肤和窗口行为，并为缺失项设置默认值。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void load_config(struct app_state *state)
{
	char *json;
	char skin_buf[64];
	enum danmu_skin skin;
	long long room;

	/* 默认气泡皮肤设定为 Google */
	state->current_skin = DANMU_SKIN_GOOGLE;

	json = read_entire_file("config.json", NULL);

	if (!json)
		return;

	room = json_get_integer(json, "room_id", 0);
	if (room > 0)
		state->room_id = room;

	json_get_string(json, "identity_code", state->identity_code,
			sizeof(state->identity_code));
	json_get_string(json, "cookie", state->cookie, sizeof(state->cookie));
	state->font_size = (int)json_get_integer(json, "font_size", 14);
	state->max_items = (int)json_get_integer(json, "max_items", 100);
	state->auto_scroll = json_get_boolean(json, "auto_scroll", true);
	state->keep_above = json_get_boolean(json, "keep_above", true);
	state->mouse_interactive = json_get_boolean(json, "mouse_interactive",
						    true);

	skin_buf[0] = '\0';
	if (json_get_string(json, "skin", skin_buf, sizeof(skin_buf))) {
		skin = ui_parse_skin(skin_buf);
		if (skin < DANMU_SKIN_COUNT)
			state->current_skin = skin;
	}

	free(json);
}

/*
 * 头像下载完成通知回调
 */
/**
 * @brief 接收头像线程派发的更新通知，并把头像路径/真实昵称交给 GTK 列表更新。
 * @param uid 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param avatar_path 只读借用指针；本函数不修改或释放所指向的对象。
 * @param real_uname 只读借用指针；本函数不修改或释放所指向的对象。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void on_avatar_update(const char *uid, const char *avatar_path,
			     const char *real_uname, void *user_data)
{
	struct app_state *state = user_data;

	ui_on_avatar_updated(state, uid, avatar_path, real_uname);
}

/*
 * 新弹幕到达分发回调
 */
/**
 * @brief 应用层弹幕回调：将收到的弹幕交给 UI 即时渲染。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param item 弹幕结构体；只读输入时不修改，异步派发前按值复制。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void on_danmu(struct app_state *state, const struct danmu_item *item,
		     void *user_data)
{
	(void)user_data;
	ui_add_danmu(state, item);
	printf("\r[弹幕] %s(UID:%s): %s\ndanmu> ",
	       item->uname, item->uid, item->text);
	fflush(stdout);
}

/*
 * 直播长连接状态变更分发回调
 */
/**
 * @brief 应用层连接状态回调：更新终端状态提示及连接状态展示。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param status 只读借用指针；本函数不修改或释放所指向的对象。
 * @param connected 连接状态布尔值；true 表示长连接已建立，false 表示连接已断开。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void on_status_update(struct app_state *state, const char *status,
			     bool connected, void *user_data)
{
	static char last_status[128] = "";
	static bool last_conn = false;

	(void)state;
	(void)user_data;
	if (!status)
		return;

	if (last_conn == connected && strcmp(last_status, status) == 0)
		return;

	last_conn = connected;
	g_strlcpy(last_status, status, sizeof(last_status));
	printf("\r[状态] %s%s\033[0m\ndanmu> ",
	       connected ? "\033[32m" : "\033[33m", status);
	fflush(stdout);
}

/*
 * 处理终端 test 测试弹幕命令
 */
/**
 * @brief 解析 test 命令并构造本地测试弹幕，不改变真实直播间消息来源。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param args 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void cmd_test(struct app_state *state, const char *args)
{
	struct danmu_item item;
	int total = sizeof(g_presets) / sizeof(g_presets[0]);
	int cur = g_preset_idx % total;
	char u_uid[64] = "";
	char u_name[128] = "";
	char u_text[512] = "";
	int matched;

	memset(&item, 0, sizeof(item));
	while (*args == ' ')
		args++;

	if (args[0] == '\0') {
		g_strlcpy(item.uid, g_presets[cur].uid, sizeof(item.uid));
		g_strlcpy(item.uname, g_presets[cur].uname, sizeof(item.uname));
		g_strlcpy(item.text, g_presets[cur].text, sizeof(item.text));
		g_preset_idx++;
	} else {
		matched = sscanf(args, "%63s %127s %511[^\n]",
				 u_uid, u_name, u_text);
		if (matched == 3) {
			g_strlcpy(item.uid, u_uid, sizeof(item.uid));
			g_strlcpy(item.uname, u_name, sizeof(item.uname));
			g_strlcpy(item.text, u_text, sizeof(item.text));
		} else {
			g_strlcpy(item.uid, g_presets[cur].uid,
				  sizeof(item.uid));
			g_strlcpy(item.uname, g_presets[cur].uname,
				  sizeof(item.uname));
			g_strlcpy(item.text, args, sizeof(item.text));
			g_preset_idx++;
		}
	}

	item.timestamp = (int64_t)time(NULL);
	avatar_get(item.uid, item.avatar_path, sizeof(item.avatar_path));
	ui_add_danmu(state, &item);
	printf("[终端] 已发送测试弹幕: %s(UID:%s): %s\n",
	       item.uname, item.uid, item.text);
}

/*
 * 显示持久化弹幕历史日志路径、大小、行数及近期记录
 */
/**
 * @brief 读取并打印本地弹幕历史日志的统计信息及近期记录。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void cmd_log(void)
{
	struct stat st;
	FILE *fp;
	char line[1024];
	char recent[5][512];
	long long total_lines = 0;
	size_t len;
	int danmu_count = 0;
	int count;
	int start;
	int i;

	if (stat(DANMU_LOG_FILE, &st) != 0) {
		printf("[终端] 弹幕日志文件尚未生成: %s\n",
		       DANMU_LOG_FILE);
		return;
	}

	fp = fopen(DANMU_LOG_FILE, "r");
	if (!fp) {
		printf("[终端] 无法打开弹幕日志文件: %s\n",
		       DANMU_LOG_FILE);
		return;
	}

	while (fgets(line, sizeof(line), fp)) {
		total_lines++;
		len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' ||
				   line[len - 1] == '\r'))
			line[--len] = '\0';

		if (!strstr(line, "[用户:"))
			continue;

		g_strlcpy(recent[danmu_count % 5], line,
			  sizeof(recent[0]));
		danmu_count++;
	}
	fclose(fp);

	printf("[终端] 弹幕持久化日志信息:\n");
	printf("  文件路径: %s\n", DANMU_LOG_FILE);
	printf("  文件大小: %lld 字节 (%.2f KB)\n",
	       (long long)st.st_size, (double)st.st_size / 1024.0);
	printf("  日志总行数: %lld 行 (弹幕记录: %d 条)\n",
	       total_lines, danmu_count);
	printf("  最近 %d 条历史弹幕:\n",
	       danmu_count < 5 ? danmu_count : 5);

	if (danmu_count == 0) {
		printf("    (暂无弹幕记录)\n");
		return;
	}

	count = danmu_count < 5 ? danmu_count : 5;
	start = danmu_count - count;
	for (i = start; i < danmu_count; i++)
		printf("    %s\n", recent[i % 5]);
}

/*
 * 执行模拟桌面拖拽基准测试
 */
/**
 * @brief 解析拖动测试参数并调用 UI 层执行窗口位移基准测试。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param args 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void cmd_drag_test(struct app_state *state, const char *args)
{
	int dx = 300;
	int dy = 200;
	int steps = 60;

	if (!state || !state->fixed) {
		printf("[终端] 错误: 界面未就绪，无法执行拖动测试。\n");
		return;
	}

	while (isspace((unsigned char)*args))
		args++;

	if (*args != '\0')
		sscanf(args, "%d %d %d", &dx, &dy, &steps);

	if (steps <= 0)
		steps = 60;

	printf("[终端] 正在执行模拟拖拽基准测试 "
	       "(位移: %d, %d, 步数: %d)...\n",
	       dx, dy, steps);
	ui_simulate_drag(state, dx, dy, steps);
	printf("[终端] 模拟拖拽基准测试完成。\n");
}

/*
 * 处理终端 skin / theme 皮肤更换命令
 */
/**
 * @brief 解析 skin 命令、校验主题名称并切换所有现存气泡的皮肤。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param args 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void cmd_skin(struct app_state *state, const char *args)
{
	enum danmu_skin target;

	if (!args || *args == '\0') {
		printf("[终端] 当前气泡皮肤: %s\n",
		       ui_skin_desc(state->current_skin));
		printf("       可用皮肤列表:\n");
		printf("         1. google    - %s%s\n",
		       ui_skin_desc(DANMU_SKIN_GOOGLE),
		       state->current_skin == DANMU_SKIN_GOOGLE ?
		       " [当前生效]" : "");
		printf("         2. openai    - %s%s\n",
		       ui_skin_desc(DANMU_SKIN_OPENAI),
		       state->current_skin == DANMU_SKIN_OPENAI ?
		       " [当前生效]" : "");
		printf("         3. anthropic - %s%s\n",
		       ui_skin_desc(DANMU_SKIN_ANTHROPIC),
		       state->current_skin == DANMU_SKIN_ANTHROPIC ?
		       " [当前生效]" : "");
		printf("         4. deepseek  - %s%s\n",
		       ui_skin_desc(DANMU_SKIN_DEEPSEEK),
		       state->current_skin == DANMU_SKIN_DEEPSEEK ?
		       " [当前生效]" : "");
		printf("       输入 'skin <名称>' 立即一键切换 "
		       "(如: skin openai)。\n");
		return;
	}

	target = ui_parse_skin(args);
	if (target >= DANMU_SKIN_COUNT) {
		printf("[终端] 未知皮肤名称 '%s'。\n", args);
		printf("       可选皮肤: google, openai, anthropic, "
		       "deepseek\n");
		return;
	}

	ui_set_skin(state, target);
	printf("[终端] 气泡皮肤已一键切换为: %s\n", ui_skin_desc(target));
}

/*
 * 解析并执行终端交互控制命令
 */
/**
 * @brief 分派终端命令到对应处理函数，集中管理命令别名和参数。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param cmd_line 只读借用指针；本函数不修改或释放所指向的对象。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void handle_terminal_command(struct app_state *state,
				    const char *cmd_line)
{
	const char *p = cmd_line;
	const char *args;
	long long account_uid;

	while (isspace((unsigned char)*p))
		p++;

	if (*p == '\0')
		return;

	if (strcmp(p, "help") == 0 || strcmp(p, "?") == 0) {
		print_terminal_banner(state);
		return;
	}

	if (strcmp(p, "log") == 0 || strcmp(p, "history") == 0) {
		cmd_log();
		return;
	}

	if (strncmp(p, "test", 4) == 0 && (p[4] == ' ' || p[4] == '\0')) {
		args = p + 4;
		cmd_test(state, args);
		return;
	}

	if (strcmp(p, "clear") == 0) {
		ui_clear_danmu(state);
		printf("\033[2J\033[H");
		print_terminal_banner(state);
		printf("[终端] 已清空桌面弹幕列表与控制台屏幕。\n");
		return;
	}

	if ((strncmp(p, "skin", 4) == 0 && (p[4] == ' ' || p[4] == '\0')) ||
	    (strncmp(p, "theme", 5) == 0 && (p[5] == ' ' || p[5] == '\0'))) {
		if (strncmp(p, "theme", 5) == 0)
			args = p + 5;
		else
			args = p + 4;

		while (isspace((unsigned char)*args))
			args++;

		cmd_skin(state, args);
		return;
	}

	if (strncmp(p, "scroll", 6) == 0) {
		args = p + 6;
		while (isspace((unsigned char)*args))
			args++;
		if (strcmp(args, "on") == 0)
			state->auto_scroll = true;
		else if (strcmp(args, "off") == 0)
			state->auto_scroll = false;
		else
			state->auto_scroll = !state->auto_scroll;
		printf("[终端] 自动置底滚动已%s。\n",
		       state->auto_scroll ? "开启" : "关闭");
		return;
	}

	if (strncmp(p, "top", 3) == 0 || strncmp(p, "ontop", 5) == 0) {
		bool target;

		if (strncmp(p, "ontop", 5) == 0)
			args = p + 5;
		else
			args = p + 3;

		while (isspace((unsigned char)*args))
			args++;

		if (strcmp(args, "on") == 0)
			target = true;
		else if (strcmp(args, "off") == 0)
			target = false;
		else
			target = !state->keep_above;

		ui_set_keep_above(state, target);
		printf("[终端] 窗口置顶已%s (全屏防遮挡模式)。\n",
		       state->keep_above ? "开启" : "关闭");
		return;
	}

	if (strncmp(p, "overlay", 7) == 0) {
		bool active;

		/* Wayland Layer Shell 原生置顶，无需修改 KWin 规则 */
		if (state->is_layer_shell) {
			printf("[终端] 当前正在运行原生 Wayland Layer Shell "
			       "Overlay 模式，\n");
			printf("       天然绝对居于全屏应用之上，"
			       "无需修改 KWin 规则。\n");
			return;
		}

		args = p + 7;
		while (isspace((unsigned char)*args))
			args++;

		if (strcmp(args, "on") == 0) {
			system("./scripts/setup_kwin_overlay_rule.sh "
			       ">/dev/null 2>&1");
			printf("[终端] 全屏防遮挡已开启 (KWin OSD浮层)。\n");
			return;
		}

		if (strcmp(args, "off") == 0) {
			system("./scripts/remove_kwin_overlay_rule.sh "
			       ">/dev/null 2>&1");
			printf("[终端] 全屏防遮挡已关闭，恢复基础置顶。\n");
			return;
		}

		active = is_overlay_rule_enabled();
		printf("[终端] 全屏防遮挡状态: %s\n",
		       active ?
			       "已开启 (OSD浮层)" :
			       "未开启 (基础置顶)");
		printf("       输入 'overlay on' 开启，"
		       "'overlay off' 关闭。\n");
		return;
	}

	if (strncmp(p, "drag_test", 9) == 0 &&
	    (p[9] == ' ' || p[9] == '\0')) {
		cmd_drag_test(state, p + 9);
		return;
	}

	if (strncmp(p, "dragtest", 8) == 0 &&
	    (p[8] == ' ' || p[8] == '\0')) {
		cmd_drag_test(state, p + 8);
		return;
	}

	if (strncmp(p, "mouse", 5) == 0 ||
	    strncmp(p, "interactive", 11) == 0 ||
	    strncmp(p, "click", 5) == 0 ||
	    strncmp(p, "drag", 4) == 0) {
		bool target;

		if (strncmp(p, "interactive", 11) == 0)
			args = p + 11;
		else if (strncmp(p, "mouse", 5) == 0)
			args = p + 5;
		else if (strncmp(p, "click", 5) == 0)
			args = p + 5;
		else
			args = p + 4;

		while (isspace((unsigned char)*args))
			args++;

		if (strcmp(args, "on") == 0)
			target = true;
		else if (strcmp(args, "off") == 0)
			target = false;
		else
			target = !state->mouse_interactive;

		ui_set_mouse_interactive(state, target);
		printf("[终端] 鼠标交互模式已%s (%s)。\n",
		       state->mouse_interactive ? "开启" : "关闭",
		       state->mouse_interactive ?
			       "可拖拽窗口，遮挡下方" :
			       "鼠标完全穿透，窗口锁定不可拖拽");
		return;
	}

	if (strncmp(p, "room", 4) == 0) {
		long long new_room;

		args = p + 4;
		while (isspace((unsigned char)*args))
			args++;
		new_room = atoll(args);
		if (new_room <= 0) {
			printf("[终端] 错误: 请输入有效正整数房号 "
			       "(如: room 123456)\n");
			return;
		}
		printf("[终端] 正在切换至直播间 %lld ...\n", new_room);
		bilibili_stop();
		state->room_id = new_room;
		bilibili_start(state);
		return;
	}

	if (strcmp(p, "status") == 0) {
		printf("[终端] 房号: %lld | 状态: %s\n",
		       state->room_id,
		       state->connected ? "已连接" : "未连接/重连中");
		if (state->identity_code[0] != '\0')
			printf("       身份码: %s\n", state->identity_code);
		g_mutex_lock(&state->lock);
		account_uid = state->account_uid;
		g_mutex_unlock(&state->lock);
		if (account_uid > 0)
			printf("       B站登录账号 UID: %lld\n", account_uid);
		else
			printf("       B站登录账号: 未认证（昵称可能被脱敏；请配置有效 Cookie）\n");
		printf("       详情: %s | 自动置底: %s | 置顶: %s\n",
		       state->status_text,
		       state->auto_scroll ? "开启" : "关闭",
		       state->keep_above ? "开启" : "关闭");
		printf("  全屏置顶模式: %s\n",
		       state->is_layer_shell ?
			       "Wayland Layer Shell Overlay (最高物理图层)" :
			       (state->keep_above ? "KWin 常驻置顶" :
						    "普通图层"));
		printf("       气泡皮肤: %s\n",
		       ui_skin_desc(state->current_skin));
		printf("       鼠标模式: %s (%s)\n",
		       state->mouse_interactive ? "开启" : "关闭",
		       state->mouse_interactive ?
			       "可拖拽窗口/遮挡下方" :
			       "鼠标完全穿透/锁定不可拖拽");
		return;
	}

	if ((strncmp(p, "quit", 4) == 0 && (p[4] == ' ' || p[4] == '\0')) ||
	    (strncmp(p, "exit", 4) == 0 && (p[4] == ' ' || p[4] == '\0')) ||
	    strcmp(p, "q") == 0) {
		printf("[终端] 正在退出程序...\n");
		g_atomic_int_set(&state->running, 0);
		if (state->app)
			g_application_quit(G_APPLICATION(state->app));
		return;
	}

	printf("[终端] 未知命令: \"%s\"。输入 \"help\" 查看命令列表。\n", p);
}

/*
 * 标准输入事件监听回调
 */
/**
 * @brief 处理标准输入文件描述符的 GLib IO 事件，并把一行输入交给命令分派器。
 * @param source 当前标准输入对应的 GIOChannel；由 GLib IO source 管理，本回调仅借用。
 * @param cond GLib IO 条件位掩码，表示标准输入当前可读、错误、挂断或无效等状态。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return GLib 回调控制值：G_SOURCE_REMOVE 表示移除源，G_SOURCE_CONTINUE 表示继续。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static gboolean on_terminal_input(GIOChannel *source, GIOCondition cond,
				  gpointer user_data)
{
	struct app_state *state = user_data;
	char *line = NULL;
	gsize len = 0;
	gsize term = 0;
	GError *err = NULL;
	GIOStatus status;

	while ((status = g_io_channel_read_line(source, &line, &len,
						&term, &err)) ==
	       G_IO_STATUS_NORMAL) {
		if (!line)
			continue;

		while (len > 0 &&
		       (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';

		handle_terminal_command(state, line);
		g_free(line);
		line = NULL;

		if (!g_atomic_int_get(&state->running)) {
			g_term_watch_id = 0;
			return G_SOURCE_REMOVE;
		}

		printf("danmu> ");
		fflush(stdout);
	}

	if (err)
		g_error_free(err);

	if (status == G_IO_STATUS_EOF || (cond & (G_IO_HUP | G_IO_ERR))) {
		g_term_watch_id = 0;
		return G_SOURCE_REMOVE;
	}

	return G_SOURCE_CONTINUE;
}

/*
 * 初始化终端控制器
 */
/**
 * @brief 创建终端输入通道并注册主循环回调，使 GTK 窗口运行时仍可交互。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void setup_terminal_controller(struct app_state *state)
{
	print_terminal_banner(state);

	g_term_chan = g_io_channel_unix_new(STDIN_FILENO);
	g_io_channel_set_encoding(g_term_chan, NULL, NULL);
	g_io_channel_set_flags(g_term_chan, G_IO_FLAG_NONBLOCK, NULL);
	g_term_watch_id = g_io_add_watch(g_term_chan,
					 G_IO_IN | G_IO_HUP | G_IO_ERR,
					 on_terminal_input, state);
}

/*
 * 注入初始测试弹幕空闲回调
 */
/**
 * @brief 在 GTK 主线程空闲回调中注入一条测试弹幕。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return GLib 回调控制值：G_SOURCE_REMOVE 表示移除源，G_SOURCE_CONTINUE 表示继续。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static gboolean inject_test_danmu_idle(gpointer user_data)
{
	struct app_state *state = user_data;
	struct danmu_item item1;
	struct danmu_item item2;

	memset(&item1, 0, sizeof(item1));
	g_strlcpy(item1.uid, "2", sizeof(item1.uid));
	g_strlcpy(item1.uname, "碧诗", sizeof(item1.uname));
	g_strlcpy(item1.text, "哔哩哔哩 (゜-゜)つロ 乾杯~", sizeof(item1.text));
	item1.timestamp = (int64_t)time(NULL);
	avatar_get(item1.uid, item1.avatar_path, sizeof(item1.avatar_path));
	ui_add_danmu(state, &item1);

	memset(&item2, 0, sizeof(item2));
	g_strlcpy(item2.uid, "3546843237582920", sizeof(item2.uid));
	g_strlcpy(item2.uname, "你猜我在线不", sizeof(item2.uname));
	g_strlcpy(item2.text,
		  "GTK4 原生悬浮弹幕就绪，全透明悬浮气泡直视桌面！",
		  sizeof(item2.text));
	item2.timestamp = (int64_t)time(NULL);
	avatar_get(item2.uid, item2.avatar_path, sizeof(item2.avatar_path));
	ui_add_danmu(state, &item2);

	return G_SOURCE_REMOVE;
}

/*
 * 从本地持久化日志还原近期历史弹幕至桌面窗口（严禁重复写回日志）
 */
/**
 * @brief 从持久化日志读取近期弹幕，并仅渲染历史记录、不重复写回日志。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void restore_history_danmu(struct app_state *state)
{
	struct danmu_item *items;
	int max_load;
	int count;
	int i;

	if (!state)
		return;

	max_load = state->max_items > 0 ? state->max_items : 100;
	items = calloc((size_t)max_load, sizeof(*items));
	if (!items)
		return;

	count = util_load_history_danmu(DANMU_LOG_FILE, items, max_load);
	for (i = 0; i < count; i++) {
		avatar_get(items[i].uid, items[i].avatar_path,
			   sizeof(items[i].avatar_path));
		ui_restore_danmu(state, &items[i]);
	}

	free(items);

	if (count > 0)
		printf("[历史] 已从本地日志还原 %d 条历史弹幕至桌面窗口。\n",
		       count);
}

/*
 * GTK 应用程序激活启动回调
 */
/**
 * @brief GtkApplication 激活入口：初始化 UI、头像模块、B 站连接和历史弹幕。
 * @param app GtkApplication 实例；由 GTK 应用框架持有，本函数仅借用。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void app_activate(GtkApplication *app, gpointer user_data)
{
	struct app_state *state = user_data;

	state->app = app;
	avatar_init(NULL, state->cookie);

	if (!ui_init(state, app)) {
		fprintf(stderr, "错误: 初始化 GTK4 界面窗口失败\n");
		return;
	}

	avatar_set_update_callback(on_avatar_update, state);
	bilibili_set_danmu_callback(on_danmu, state);
	bilibili_set_status_callback(on_status_update, state);

	restore_history_danmu(state);

	util_log_session_start(state->room_id);

	setup_terminal_controller(state);

	if (g_opt_test)
		g_idle_add(inject_test_danmu_idle, state);

	if (state->room_id > 0) {
		bilibili_start(state);
	} else {
		g_strlcpy(state->status_text,
			  "未配置直播间号 (等待输入 room <房号>)",
			  sizeof(state->status_text));
		state->connected = false;
	}
}

/*
 * GTK 应用程序退出清理回调
 */
/**
 * @brief 应用退出入口：停止网络与头像线程，释放 GTK/GLib 相关资源。
 * @param app GtkApplication 实例；由 GTK 应用框架持有，本函数仅借用。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
static void app_shutdown(GtkApplication *app, gpointer user_data)
{
	struct app_state *state = user_data;

	(void)app;
	g_atomic_int_set(&state->running, 0);
	if (g_term_watch_id > 0) {
		g_source_remove(g_term_watch_id);
		g_term_watch_id = 0;
	}
	if (g_term_chan) {
		g_io_channel_unref(g_term_chan);
		g_term_chan = NULL;
	}
	bilibili_stop();
	bilibili_set_danmu_callback(NULL, NULL);
	bilibili_set_status_callback(NULL, NULL);
	avatar_set_update_callback(NULL, NULL);
	avatar_cleanup();
}

/*
 * 桌面弹幕应用主入口
 */
/**
 * @brief 初始化应用状态、参数和回调，启动 GTK 应用主循环并在退出后清理状态。
 * @param argc 命令行参数数量，遵循 C main() 约定。
 * @param argv 命令行参数字符串数组；由运行时提供，本函数只读。
 * @return 返回条目数量、解析结果或状态码，具体语义见函数说明。
 * @thread GTK 主线程调用；不得从网络/头像工作线程直接访问 GTK Widget。
 */
int main(int argc, char **argv)
{
	GtkApplication *app;
	int status;
	int i;

	/* 屏蔽 GTK / GDK / GLib 底层杂乱警告输出，保证终端交互界面纯净 */
	g_log_set_writer_func(suppress_gtk_log_writer, NULL, NULL);

	set_project_working_directory();
	memset(&g_state, 0, sizeof(g_state));
	g_mutex_init(&g_state.lock);
	g_state.room_id = 0LL;
	g_state.auto_scroll = true;
	g_state.keep_above = true;
	g_state.mouse_interactive = true;
	g_state.max_items = 100;
	g_state.font_size = 14;
	g_atomic_int_set(&g_state.running, 1);

	load_config(&g_state);

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 ||
		    strcmp(argv[i], "--help") == 0) {
			print_help(argv[0]);
			g_mutex_clear(&g_state.lock);
			return 0;
		}

		if ((strcmp(argv[i], "-r") == 0 ||
		     strcmp(argv[i], "--room") == 0) && i + 1 < argc) {
			g_state.room_id = atoll(argv[++i]);
			continue;
		}

		if (strcmp(argv[i], "--test") == 0) {
			g_opt_test = true;
			continue;
		}
	}

	app = gtk_application_new("com.kuimuztion.bilibili_danmu_desktop",
				  G_APPLICATION_NON_UNIQUE);

	g_signal_connect(app, "activate", G_CALLBACK(app_activate), &g_state);
	g_signal_connect(app, "shutdown", G_CALLBACK(app_shutdown), &g_state);

	status = g_application_run(G_APPLICATION(app), 0, NULL);

	g_object_unref(app);
	g_mutex_clear(&g_state.lock);

	return status;
}
