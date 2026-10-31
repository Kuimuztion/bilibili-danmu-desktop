/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Copyright (C) 2026 Kuimuztion
 *
 * B站直播间弹幕长连接与消息流解析
 */

#include "bilibili.h"
#include "avatar.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>
#include <curl/websockets.h>
#include <zlib.h>
#include <glib.h>
#include <gtk/gtk.h>

/*
 * 主线程派发数据结构
 */
struct status_update_data {
	/* state 指向 app_state 全局/应用生命周期对象，载荷只借用该指针。 */
	struct app_state *state;
	char status[128];
	bool connected;
};

struct danmu_dispatch_data {
	/* state 借用应用状态；item 是值拷贝，不引用网络接收缓冲区。 */
	struct app_state *state;
	struct danmu_item item;
};

static GThread *g_bili_thread; /* 网络 worker 线程句柄；bilibili_stop() join 后置 NULL。 */
static gint g_bili_running;    /* GLib 原子运行标志，供 worker 与 stop 协作。 */
static void (*g_danmu_cb)(struct app_state *state,
			  const struct danmu_item *item,
			  void *user_data);
static void *g_danmu_user_data;
static void (*g_status_cb)(struct app_state *state,
			   const char *status,
			   bool connected,
			   void *user_data);
static void *g_status_user_data;
static GMutex g_cb_lock;

/*
 * 主事件循环状态更新回调
 */
/**
 * @brief 在 GTK 主线程提交状态文本并调用注册的状态回调；随后释放派发载荷。
 * @param user_data 由 update_status() 分配并转交给 GLib source 的载荷；本回调负责 free。
 * @return GLib 回调控制值：G_SOURCE_REMOVE 表示移除源，G_SOURCE_CONTINUE 表示继续。
 * @thread GLib 主线程回调；负责消费并释放由网络线程分配的派发载荷。
 */
static gboolean on_status_idle(gpointer user_data)
{
	/* d 由 update_status() 的 malloc 创建并交给 GLib，当前回调负责 free。 */
	struct status_update_data *d = user_data;

	if (d->state) {
		g_mutex_lock(&d->state->lock);
		d->state->connected = d->connected;
		g_strlcpy(d->state->status_text, d->status,
			  sizeof(d->state->status_text));
		g_mutex_unlock(&d->state->lock);

		void (*cb)(struct app_state *, const char *, bool, void *);
		void *user_data;

		g_mutex_lock(&g_cb_lock);
		cb = g_status_cb;
		user_data = g_status_user_data;
		g_mutex_unlock(&g_cb_lock);

		if (cb)
			cb(d->state, d->status, d->connected, user_data);
	}

	free(d);
	return G_SOURCE_REMOVE;
}

/*
 * 向主线程派发状态更新
 */
/**
 * @brief 从网络线程创建状态快照并投递到 GLib 主循环，不直接访问 GTK 控件。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param status 只读借用指针；本函数不修改或释放所指向的对象。
 * @param connected 连接状态布尔值；true 表示长连接已建立，false 表示连接已断开。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 可由网络工作线程调用；只复制数据并投递主循环，不直接操作 GTK。
 */
static void update_status(struct app_state *state, const char *status,
			  bool connected)
{
	struct status_update_data *d;

	if (!state)
		return;

	d = malloc(sizeof(*d));
	if (!d)
		return;

	d->state = state;
	d->connected = connected;
	g_strlcpy(d->status, status, sizeof(d->status));

	g_idle_add_full(G_PRIORITY_DEFAULT, on_status_idle, d, NULL);
}

/*
 * 主事件循环弹幕到达回调
 */
/**
 * @brief 在 GTK 主线程调用弹幕回调；派发数据由本回调负责释放。
 * @param user_data 由 dispatch_danmu() 分配并转交给 GLib source 的载荷；本回调负责 free。
 * @return GLib 回调控制值：G_SOURCE_REMOVE 表示移除源，G_SOURCE_CONTINUE 表示继续。
 * @thread GLib 主线程回调；负责消费并释放由网络线程分配的派发载荷。
 */
static gboolean on_danmu_idle(gpointer user_data)
{
	/* d 是 dispatch_danmu() 分配的独立快照；回调完成后由此处释放。 */
	struct danmu_dispatch_data *d = user_data;

	void (*cb)(struct app_state *, const struct danmu_item *, void *);
	void *cb_data;

	g_mutex_lock(&g_cb_lock);
	cb = g_danmu_cb;
	cb_data = g_danmu_user_data;
	g_mutex_unlock(&g_cb_lock);

	if (cb)
		cb(d->state, &d->item, cb_data);

	free(d);
	return G_SOURCE_REMOVE;
}

/*
 * 向主线程派发新弹幕消息
 */
/**
 * @brief 复制一份弹幕值对象并投递到 GTK 主线程，避免跨线程传递栈内存。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param item 弹幕结构体；只读输入时不修改，异步派发前按值复制。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 可由网络工作线程调用；只复制数据并投递主循环，不直接操作 GTK。
 */
static void dispatch_danmu(struct app_state *state,
			   const struct danmu_item *item)
{
	struct danmu_dispatch_data *d;

	d = malloc(sizeof(*d));
	if (!d)
		return;

	d->state = state;
	memcpy(&d->item, item, sizeof(d->item));

	/* 弹幕派发比普通 idle/绘制后任务优先，减少主循环积压造成的观感延迟。 */
	g_idle_add_full(G_PRIORITY_HIGH, on_danmu_idle, d, NULL);
}

/*
 * 使用系统 zlib uncompress 解压 Deflate 压缩包体
 */
/**
 * @brief 将 zlib 压缩的 B 站协议包体解压到新分配的输出缓冲区。
 * @param source 调用者拥有的只读输入字节/文本缓冲区；仅在本次调用期间读取。
 * @param source_len source 指向的有效输入字节数；不会包含额外的 NUL 终止符。
 * @param output 输出参数：成功时接收本函数 malloc 的解压缓冲区，所有权转给调用者，调用者负责 free。
 * @param output_len 输出参数：写入解压后字节数，由调用者提供 size_t 存储。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static bool decompress_zlib(const unsigned char *source, size_t source_len,
			    unsigned char **output, size_t *output_len)
{
	unsigned long capacity;
	int attempt;

	if (source_len > 0xFFFFFFFFu)
		return false;

	capacity = 256 * 1024;
	for (attempt = 0; attempt < 5; attempt++) {
		unsigned char *buffer = malloc(capacity);
		unsigned long actual;
		int code;

		if (!buffer)
			return false;

		actual = capacity;
		code = uncompress(buffer, &actual, source,
				  (unsigned long)source_len);
		if (code == Z_OK) {
			*output = buffer;
			*output_len = actual;
			return true;
		}

		free(buffer);
		if (code != Z_BUF_ERROR)
			return false;

		capacity *= 2;
	}

	return false;
}

/*
 * curl HTTP 写入回调函数
 */
/**
 * @brief libcurl HTTP 响应写入回调，将收到的字节追加到 str_buf。
 * @param contents 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @param size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @param nmemb libcurl 本次回调提供的元素数量；与 size 相乘得到当前数据块字节数。
 * @param userp 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @return 返回处理或消耗的字节数。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static size_t bili_curl_write_cb(void *contents, size_t size, size_t nmemb,
				 void *userp)
{
	struct str_buf *sb = userp;
	size_t realsize = size * nmemb;

	if (!sb_append_n(sb, contents, realsize))
		return 0;

	return realsize;
}

/*
 * 执行轻量 HTTPS GET 请求
 */
/**
 * @brief 执行带超时/请求头的 HTTP GET，并把响应正文收集到调用者的字符串缓冲区。
 * @param host 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param path 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param cookie 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param status_out 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @return 成功时返回新分配的字符串，所有权交给调用者并由调用者 free；失败返回 NULL。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static char *http_get(const char *host, const char *path, const char *cookie,
		      long *status_out)
{
	CURL *curl;
	CURLcode res;
	char url[1024];
	struct str_buf sb;
	struct curl_slist *headers = NULL;
	long status = 0;

	curl = curl_easy_init();
	if (!curl)
		return NULL;

	sb_init(&sb, 8192);

	snprintf(url, sizeof(url), "https://%s%s", host, path);

	headers = curl_slist_append(headers,
		"User-Agent: Mozilla/5.0 (X11; Linux x86_64)");
	headers = curl_slist_append(headers,
		"Origin: https://live.bilibili.com");
	headers = curl_slist_append(headers,
		"Referer: https://live.bilibili.com/");

	if (cookie && cookie[0] != '\0') {
		char cookie_header[8192];

		snprintf(cookie_header, sizeof(cookie_header),
			 "Cookie: %s", cookie);
		headers = curl_slist_append(headers, cookie_header);
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, bili_curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sb);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

	res = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	if (status_out)
		*status_out = status;

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK || status < 200 || status >= 300) {
		sb_free(&sb);
		return NULL;
	}

	return sb.data;
}

/* B 站 Wbi 鉴权编码映射表 */
static const int MIXIN_TAB[64] = {
	46, 47, 18, 2,  53, 8,  23, 32, 15, 50, 10, 31, 58, 3,  45, 35,
	27, 43, 5,  49, 33, 9,  42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
	37, 48, 7,  16, 24, 55, 40, 61, 26, 17, 0,  1,  60, 51, 30, 4,
	22, 25, 54, 21, 56, 59, 6,  63, 57, 62, 11, 36, 20, 34, 44, 52
};

/*
 * 从 URL 中截取不含扩展名的文件标识
 */
/**
 * @brief 从 URL 生成稳定的本地缓存文件键，供请求结果缓存命名使用。
 * @param url 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param out 调用者拥有的可写输出缓冲区；本函数只写入有界结果，不释放该缓冲区。
 * @param size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static void file_key(const char *url, char *out, size_t size)
{
	const char *slash = strrchr(url, '/');
	const char *start = slash ? slash + 1 : url;
	const char *dot = strchr(start, '.');
	size_t len = dot ? (size_t)(dot - start) : strlen(start);

	if (len >= size)
		len = size - 1;
	memcpy(out, start, len);
	out[len] = '\0';
}

/*
 * 从 room_init 响应动态解析房主真实 UID，并通过登录态拉取昵称
 */
/**
 * @brief 解析房间信息响应，更新当前直播间房主 UID 与昵称。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param room_json 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param cookie 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static void update_anchor_info(struct app_state *state, const char *room_json,
			       const char *cookie)
{
	long long uid;
	char uid_str[64];
	char card_path[256];
	char *card;
	long status = 0;

	if (!state || !room_json)
		return;

	uid = json_get_integer(room_json, "uid", 0);
	if (uid <= 0)
		return;

	state->anchor_uid = uid;
	snprintf(uid_str, sizeof(uid_str), "%lld", uid);

	/* 预拉取房主卡片以获取未脱敏真实昵称并预加载头像 */
	snprintf(card_path, sizeof(card_path),
		 "/x/web-interface/card?mid=%s", uid_str);
	card = http_get("api.bilibili.com", card_path, cookie, &status);
	if (!card)
		return;

	json_get_string(card, "name", state->anchor_uname,
			sizeof(state->anchor_uname));
	free(card);
}

/*
 * 检索并获取直播间长连接令牌与服务器节点
 */
/**
 * @brief 调用 B 站接口发现弹幕服务器、房间认证令牌及登录账号身份信息。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param buvid 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @param buvid_size buvid 输出数组的总容量，单位为字节，包含 NUL 终止符空间。
 * @param host 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @param host_size host 输出数组的总容量，单位为字节，包含 NUL 终止符空间。
 * @param port 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @param token 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @param token_size token 输出数组的总容量，单位为字节，包含 NUL 终止符空间。
 * @param real_room 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static bool discover_danmu_info(struct app_state *state, char *buvid,
				size_t buvid_size, char *host,
				size_t host_size, int *port, char *token,
				size_t token_size, long long *real_room)
{
	long status = 0;
	long long req_room = state->room_id;
	char buvid4[256] = "";
	char cookie[8192] = "";
	char room_path[256];
	char *spi;
	char *room;
	char *nav;
	char img_url[512] = "";
	char sub_url[512] = "";
	char img[128];
	char sub[128];
	char source[256];
	char mixin[33];
	size_t source_len;
	int used = 0;
	int i;
	long long wts;
	char query[512];
	char signed_text[600];
	char rid[33];
	char danmu_path[768];
	char *danmu;
	long long code;
	bool ok;

	spi = http_get("api.bilibili.com", "/x/frontend/finger/spi",
		       NULL, &status);
	if (spi) {
		json_get_string(spi, "b_3", buvid, buvid_size);
		json_get_string(spi, "b_4", buvid4, sizeof(buvid4));
		free(spi);
	}

	if (buvid[0] == '\0') {
		snprintf(buvid, buvid_size,
			 "00000000-0000-4000-8000-%012llxinfoc",
			 (unsigned long long)time(NULL));
	}

	g_mutex_lock(&state->lock);
	state->account_uid = 0;
	g_mutex_unlock(&state->lock);
	if (state->cookie[0] != '\0')
		snprintf(cookie, sizeof(cookie),
			 "%s; buvid3=%s; buvid4=%s; CURRENT_FNVAL=4048",
			 state->cookie, buvid, buvid4);
	else
		snprintf(cookie, sizeof(cookie),
			 "buvid3=%s; buvid4=%s; CURRENT_FNVAL=4048",
			 buvid, buvid4);

	snprintf(room_path, sizeof(room_path),
		 "/room/v1/Room/room_init?id=%lld", req_room);
	room = http_get("api.live.bilibili.com", room_path, cookie, &status);
	if (!room)
		return false;

	*real_room = json_get_integer(room, "room_id", req_room);
	update_anchor_info(state, room, cookie);
	free(room);

	nav = http_get("api.bilibili.com", "/x/web-interface/nav",
		       cookie, &status);
	if (!nav)
		return false;

	json_get_string(nav, "img_url", img_url, sizeof(img_url));
	json_get_string(nav, "sub_url", sub_url, sizeof(sub_url));
	/* nav.data.mid 是当前 Cookie 对应的账号 UID；游客通常为 0。 */
	g_mutex_lock(&state->lock);
	state->account_uid = json_get_integer(nav, "mid", 0);
	g_mutex_unlock(&state->lock);
	free(nav);

	if (img_url[0] == '\0' || sub_url[0] == '\0')
		return false;

	file_key(img_url, img, sizeof(img));
	file_key(sub_url, sub, sizeof(sub));
	snprintf(source, sizeof(source), "%s%s", img, sub);
	source_len = strlen(source);

	for (i = 0; i < 64 && used < 32; i++) {
		if ((size_t)MIXIN_TAB[i] < source_len)
			mixin[used++] = source[MIXIN_TAB[i]];
	}
	mixin[used] = '\0';

	wts = (long long)time(NULL);
	snprintf(query, sizeof(query),
		 "id=%lld&type=0&web_location=444.8&wts=%lld",
		 *real_room, wts);
	snprintf(signed_text, sizeof(signed_text), "%s%s", query, mixin);

	md5_hex(signed_text, rid);

	snprintf(danmu_path, sizeof(danmu_path),
		 "/xlive/web-room/v1/index/getDanmuInfo?%s&w_rid=%s",
		 query, rid);

	danmu = http_get("api.live.bilibili.com", danmu_path, cookie, &status);
	if (!danmu)
		return false;

	code = json_get_integer(danmu, "code", -999);
	ok = (code == 0) &&
	     json_get_string(danmu, "token", token, token_size) &&
	     json_get_string(danmu, "host", host, host_size);

	*port = (int)json_get_integer(danmu, "wss_port", 443);
	free(danmu);

	return ok;
}

/*
 * 大端字节序辅助函数
 */
/**
 * @brief 将 16 位整数按网络字节序写入协议缓冲区。
 * @param p 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @param n 要编码写入协议缓冲区的无符号整数值；put_be16 使用 16 位，put_be32 使用 32 位。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static void put_be16(unsigned char *p, uint16_t n)
{
	p[0] = (unsigned char)(n >> 8);
	p[1] = (unsigned char)n;
}

/**
 * @brief 将 32 位整数按网络字节序写入协议缓冲区。
 * @param p 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @param n 要编码写入协议缓冲区的无符号整数值；put_be16 使用 16 位，put_be32 使用 32 位。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static void put_be32(unsigned char *p, uint32_t n)
{
	p[0] = (unsigned char)(n >> 24);
	p[1] = (unsigned char)(n >> 16);
	p[2] = (unsigned char)(n >> 8);
	p[3] = (unsigned char)n;
}

/**
 * @brief 从网络字节序缓冲区读取 16 位整数。
 * @param p 只读借用指针；本函数不修改或释放所指向的对象。
 * @return 返回从网络字节序解码的整数值。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static uint16_t get_be16(const unsigned char *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

/**
 * @brief 从网络字节序缓冲区读取 32 位整数。
 * @param p 只读借用指针；本函数不修改或释放所指向的对象。
 * @return 返回从网络字节序解码的整数值。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static uint32_t get_be32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) |
	       ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) |
	       p[3];
}

/*
 * 构造 B 站直播弹幕协议二进制包
 */
/**
 * @brief 按照 B 站直播协议构造带 16 字节包头的请求/心跳数据包。
 * @param operation B 站协议头中的 operation 操作码，用于区分认证、心跳、业务消息等包类型。
 * @param body 调用者拥有的只读输入字节/文本缓冲区；仅在本次调用期间读取。
 * @param length 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return 成功时返回新分配的字符串，所有权交给调用者并由调用者 free；失败返回 NULL。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static unsigned char *make_packet(uint32_t operation, const char *body,
				  size_t *length)
{
	size_t body_len = body ? strlen(body) : 0;
	unsigned char *packet;

	*length = 16 + body_len;
	packet = malloc(*length);
	if (!packet)
		return NULL;

	put_be32(packet, (uint32_t)*length);
	put_be16(packet + 4, 16);
	put_be16(packet + 6, 1);
	put_be32(packet + 8, operation);
	put_be32(packet + 12, 1);

	if (body_len)
		memcpy(packet + 16, body, body_len);

	return packet;
}

/*
 * 礼物消息富文本格式模板（高亮金色礼物名称，与 B 站原生样式保持一致）
 */
static const char GIFT_MSG_FMT[] =
	"%s <span foreground=\"#fbbf24\"><b>%s</b></span> x%lld";

/*
 * 去除首尾多余空白和引号并截取 ID 字符串
 */
/**
 * @brief 将 JSON UID token 转换为标准十进制字符串，兼容字符串与数字 token。
 * @param token 只读借用指针；本函数不修改或释放所指向的对象。
 * @param length 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @param out 调用者拥有的可写输出缓冲区；本函数只写入有界结果，不释放该缓冲区。
 * @param out_size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static void token_to_id(const char *token, size_t length, char *out,
			size_t out_size)
{
	while (length && isspace((unsigned char)*token)) {
		token++;
		length--;
	}

	while (length && isspace((unsigned char)token[length - 1]))
		length--;

	if (length >= 2 && token[0] == '\"' && token[length - 1] == '\"') {
		token++;
		length -= 2;
	}

	if (length >= out_size)
		length = out_size - 1;

	memcpy(out, token, length);
	out[length] = '\0';
}

/*
 * 解析 DANMU_MSG 的扩展用户对象。
 * 新协议把真实用户信息放在 info[0][15].user.base，
 * extra 则是另一个 JSON 字符串；info[0][7] 不是用户 UID 哈希。
 */
/**
 * @brief 解析 DANMU_MSG 新版用户元数据，提取真实 UID、昵称和头像直链。
 * @param info 只读借用指针；本函数不修改或释放所指向的对象。
 * @param item 弹幕结构体；只读输入时不修改，异步派发前按值复制。
 * @param face_out 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @param face_len face_out 输出数组的总容量，单位为字节，包含 NUL 终止符空间。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static void handle_danmu_msg_metadata(const char *info,
				      struct danmu_item *item,
				      char *face_out, size_t face_len)
{
	const char *meta = NULL;
	const char *meta_token = NULL;
	size_t meta_len = 0;
	char meta_buf[8192] = "";
	char extra_buf[4096] = "";
	char real_name[128] = "";
	char face[512] = "";
	char user_hash[128] = "";
	long long meta_uid;
	size_t i;
	bool valid_hash = true;

	if (!json_array_element(info, 0, &meta, &meta_len) ||
	    !json_array_element(meta, 15, &meta_token, &meta_len))
		return;

	if (meta_len >= sizeof(meta_buf))
		return;
	memcpy(meta_buf, meta_token, meta_len);
	meta_buf[meta_len] = '\0';

	/* 兼容扩展字段为对象或 JSON 字符串的两种服务端形态。 */
	if (meta_buf[0] == '"') {
		char decoded[sizeof(meta_buf)];

		if (json_token_string(meta_buf, strlen(meta_buf), decoded,
				      sizeof(decoded)))
			g_strlcpy(meta_buf, decoded, sizeof(meta_buf));
	}

	/* info[0][15].user.uid 在新版协议中比 info[2][0] 更可靠。 */
	meta_uid = json_get_integer(meta_buf, "uid", 0);
	if (meta_uid > 0) {
		snprintf(item->uid, sizeof(item->uid), "%lld", meta_uid);
	}

	/* base.name/base.face 与 origin_info 中的备用字段均在此对象内。 */
	json_get_string(meta_buf, "name", real_name, sizeof(real_name));
	json_get_string(meta_buf, "face", face, sizeof(face));
	if (face[0] != '\0' && face_out && face_len > 0)
		g_strlcpy(face_out, face, face_len);

	/* extra 是 JSON 字符串，user_hash 只用作本地匿名缓存键，不反推 UID。 */
	json_get_string(meta_buf, "extra", extra_buf, sizeof(extra_buf));
	if (extra_buf[0] != '\0') {
		if (!json_get_string(extra_buf, "user_hash", user_hash,
				     sizeof(user_hash))) {
			long long numeric_hash = json_get_integer(
				extra_buf, "user_hash", 0);

			if (numeric_hash > 0)
				snprintf(user_hash, sizeof(user_hash), "%lld",
					 numeric_hash);
		}
		if (face_out && face_len > 0 && face_out[0] == '\0')
			json_get_string(extra_buf, "face", face_out, face_len);
	}

	if (real_name[0] != '\0' && !strchr(real_name, '*'))
		g_strlcpy(item->uname, real_name, sizeof(item->uname));

	/*
	 * 没有真实 UID 时，使用 B 站提供的 user_hash 建立本地缓存键，
	 * 仅用于头像缓存关联；绝不把它当成 UID，也不尝试反查账号。
	 */
	if ((item->uid[0] == '\0' || strcmp(item->uid, "0") == 0) &&
	    user_hash[0] != '\0') {
		for (i = 0; user_hash[i] != '\0'; i++) {
			if (!g_ascii_isalnum(user_hash[i]) &&
			    user_hash[i] != '_' && user_hash[i] != '-') {
				valid_hash = false;
				break;
			}
		}
		if (valid_hash)
			snprintf(item->uid, sizeof(item->uid), "hash:%.50s",
				 user_hash);
	}
}

/*
 * 解析并派发 DANMU_MSG 实时弹幕消息
 */
/**
 * @brief 解析普通弹幕文本和发送者信息，补齐主播资料、头像并派发到 UI。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param json NUL 结尾的只读 JSON 文本；本函数不拥有其内存，解析 token 指针不得逃逸其生命周期。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static void handle_danmu_msg(struct app_state *state, const char *json)
{
	const char *info;
	const char *text_token = NULL;
	const char *user_array = NULL;
	const char *uid_token = NULL;
	const char *name_token = NULL;
	size_t text_len = 0;
	size_t user_len = 0;
	size_t uid_len = 0;
	size_t name_len = 0;
	char face_url[512] = "";
	struct danmu_item item;

	info = json_find_value(json, "info");
	if (!info ||
	    !json_array_element(info, 1, &text_token, &text_len) ||
	    !json_array_element(info, 2, &user_array, &user_len) ||
	    !json_array_element(user_array, 0, &uid_token, &uid_len) ||
	    !json_array_element(user_array, 1, &name_token, &name_len))
		return;

	memset(&item, 0, sizeof(item));
	json_token_string(text_token, text_len, item.text, sizeof(item.text));
	token_to_id(uid_token, uid_len, item.uid, sizeof(item.uid));
	json_token_string(name_token, name_len, item.uname,
			  sizeof(item.uname));

	handle_danmu_msg_metadata(info, &item, face_url,
				  sizeof(face_url));

	if (state && state->anchor_uid > 0 &&
	    strcmp(item.uid, "0") != 0 &&
	    atoll(item.uid) == state->anchor_uid &&
	    state->anchor_uname[0] != '\0')
		g_strlcpy(item.uname, state->anchor_uname, sizeof(item.uname));

	item.timestamp = (int64_t)time(NULL);

	/* 关联本地头像缓存路径（未缓存则自动加入后台异步下载） */
	avatar_get_or_fetch(item.uid, face_url[0] ? face_url : NULL,
			    item.uname, item.avatar_path,
			    sizeof(item.avatar_path));

	/* 安全派发至 GTK 主线程 */
	dispatch_danmu(state, &item);
}

/*
 * 礼物字段回退读取工具：只在目标缓冲区为空时从指定 JSON 对象取值。
 * object 是指向 JSON 对象起始字符的借用指针；out 始终指向调用者持有的缓冲区。
 */
/**
 * @brief 只在目标字符串为空时，从指定 JSON 对象读取一个字符串字段。
 * @param object 只读借用指针；本函数不修改或释放所指向的对象。
 * @param key 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param out 调用者拥有的可写输出缓冲区；本函数只写入有界结果，不释放该缓冲区。
 * @param out_size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static void gift_copy_string_if_empty(const char *object, const char *key,
				      char *out, size_t out_size)
{
	if (!object || !key || !out || out_size == 0 || out[0] != '\0')
		return;

	(void)json_object_get_string(object, key, out, out_size);
}

/*
 * 将 JSON 对象中的正整数 UID 转为十进制文本。
 * 不覆盖已经解析出的 UID，避免低优先级回退字段替换主字段。
 */
/**
 * @brief 只在 UID 输出为空时读取正整数 UID，并写为十进制文本。
 * @param object 只读借用指针；本函数不修改或释放所指向的对象。
 * @param uid 调用者拥有的可写输出缓冲区；本函数只写入有界结果，不释放该缓冲区。
 * @param uid_size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static void gift_copy_uid_if_empty(const char *object, char *uid,
				   size_t uid_size)
{
	long long value;

	if (!object || !uid || uid_size == 0 || uid[0] != '\0')
		return;

	value = json_object_get_integer(object, "uid", 0);
	if (value <= 0)
		return;

	(void)snprintf(uid, uid_size, "%lld", value);
}

/*
 * 从一个明确指定的送礼人对象中补齐缺失字段。
 * member 只能是 user_info、batch_combo_send 或 combo_send，绝不查询
 * receive_user_info；因此收礼主播的信息不会进入赠礼者字段。
 */
/**
 * @brief 从明确允许的送礼人子对象读取昵称、UID、礼物名或头像，不触碰收礼方字段。
 * @param data 指向输入 JSON 对象或文本的只读借用指针，不得释放或保存到调用之后。
 * @param member 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param uname 调用者拥有的可写输出缓冲区；本函数只写入有界结果，不释放该缓冲区。
 * @param uname_size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @param uid 调用者拥有的可写输出缓冲区；本函数只写入有界结果，不释放该缓冲区。
 * @param uid_size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @param gift_name 调用者拥有的可写输出缓冲区；本函数只写入有界结果，不释放该缓冲区。
 * @param gift_name_size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @param face_url 调用者拥有的可写输出缓冲区；本函数只写入有界结果，不释放该缓冲区。
 * @param face_url_size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @param read_gift_name 是否允许从该嵌套对象回退读取 gift_name 字段；true 表示读取，false 表示跳过。
 * @param read_face 是否允许从该嵌套对象回退读取 face 字段；true 表示读取，false 表示跳过。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static void gift_read_sender_member(const char *data, const char *member,
				    char *uname, size_t uname_size,
				    char *uid, size_t uid_size,
				    char *gift_name, size_t gift_name_size,
				    char *face_url, size_t face_url_size,
				    bool read_gift_name,
				    bool read_face)
{
	const char *sender;

	if (!data || !member)
		return;

	sender = json_object_find_value(data, member);
	if (!sender || *sender != '{')
		return;

	gift_copy_string_if_empty(sender, "uname", uname, uname_size);
	gift_copy_uid_if_empty(sender, uid, uid_size);

	if (read_gift_name)
		gift_copy_string_if_empty(sender, "gift_name", gift_name,
					  gift_name_size);
	if (read_face)
		gift_copy_string_if_empty(sender, "face", face_url,
					  face_url_size);
}

/*
 * 按 B 站礼物协议的字段优先级读取礼物数量。
 * 对所有服务端版本都保留旧有字段顺序，字段缺失或非正数时回退为 1。
 */
/**
 * @brief 按服务端兼容优先级寻找正数礼物数量，所有字段缺失时返回 1。
 * @param data 指向输入 JSON 对象或文本的只读借用指针，不得释放或保存到调用之后。
 * @return 返回解析出的整数，缺失/解析失败时使用 fallback。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static long long gift_read_count(const char *data)
{
	static const char *const keys[] = {
		"num", "gift_num", "combo_num", "batch_combo_num", "total_num"
	};
	size_t i;
	long long value;

	for (i = 0; i < G_N_ELEMENTS(keys); i++) {
		value = json_object_get_integer(data, keys[i], 0);
		if (value > 0)
			return value;
	}

	return 1;
}

/*
 * 从礼物事件中提取送礼人信息并构造统一弹幕项。
 *
 * @param state 当前应用状态；借用指针，用于读取主播 UID 与投递弹幕。
 * @param json  单条 B 站事件 JSON；借用只读字符串，函数不保留其地址。
 * @param command 已去除附加标记的事件名称，如 SEND_GIFT / GUARD_BUY。
 *
 * JSON 对象指针均只在本函数调用期间借用；item 是栈上值，派发前会被
 * dispatch_danmu() 按值复制，因此不会把栈地址交给 GTK 主线程。
 */
/**
 * @brief 解析真实送礼/大航海事件，按字段优先级提取赠礼者并构造统一礼物弹幕。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param json NUL 结尾的只读 JSON 文本；本函数不拥有其内存，解析 token 指针不得逃逸其生命周期。
 * @param command 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static void handle_gift_msg(struct app_state *state, const char *json,
			    const char *command)
{
	/* data 指向 json 内部的对象片段，只读借用，绝不能 free 或保存到异步任务。 */
	const char *data;
	char uname[128] = "";
	char gift_name[128] = "";
	char action[64] = "";
	char face_url[512] = "";
	char uid[64] = "";
	long long uid_val;
	long long num;
	struct danmu_item item;

	/* data 必须是对象；其他形态不是可解析的礼物交易事件。 */
	data = json_object_find_value(json, "data");
	if (!data || *data != '{')
		return;

	memset(&item, 0, sizeof(item));

	/* 第一优先级：data 顶层直接字段，保持既有服务端字段兼容顺序。 */
	gift_copy_string_if_empty(data, "uname", uname, sizeof(uname));
	gift_copy_string_if_empty(data, "username", uname, sizeof(uname));
	gift_copy_string_if_empty(data, "sender_name", uname, sizeof(uname));

	uid_val = json_object_get_integer(data, "uid", 0);
	if (uid_val > 0)
		(void)snprintf(uid, sizeof(uid), "%lld", uid_val);
	if (uid[0] == '\0') {
		uid_val = json_object_get_integer(data, "sender_uid", 0);
		if (uid_val > 0)
			(void)snprintf(uid, sizeof(uid), "%lld", uid_val);
	}

	gift_copy_string_if_empty(data, "giftName", gift_name,
				  sizeof(gift_name));
	gift_copy_string_if_empty(data, "gift_name", gift_name,
				  sizeof(gift_name));
	gift_copy_string_if_empty(data, "role_name", gift_name,
				  sizeof(gift_name));
	gift_copy_string_if_empty(data, "action", action, sizeof(action));
	gift_copy_string_if_empty(data, "face", face_url, sizeof(face_url));
	gift_copy_string_if_empty(data, "uface", face_url, sizeof(face_url));

	/* 第二优先级：新版 user_info 用户资料对象。 */
	gift_read_sender_member(data, "user_info", uname, sizeof(uname),
				uid, sizeof(uid), gift_name, sizeof(gift_name),
				face_url, sizeof(face_url), false, true);

	/* 第三、四优先级：旧版 combo 对象。只有身份字段不完整时才回退。 */
	if (uname[0] == '\0' || uid[0] == '\0')
		gift_read_sender_member(data, "batch_combo_send",
					uname, sizeof(uname), uid, sizeof(uid),
					gift_name, sizeof(gift_name),
					face_url, sizeof(face_url), true, false);

	if (uname[0] == '\0' || uid[0] == '\0')
		gift_read_sender_member(data, "combo_send", uname,
					sizeof(uname), uid, sizeof(uid),
					gift_name, sizeof(gift_name),
					face_url, sizeof(face_url), true, false);

	/* 主播 UID 是收礼方身份，不允许作为当前房间的送礼者。 */
	if (state && state->anchor_uid > 0 && uid[0] != '\0' &&
	    atoll(uid) == state->anchor_uid)
		return;

	/* 没有真实发送者标识的进度广播不能伪装成用户送礼。 */
	if (uname[0] == '\0' && uid[0] == '\0')
		return;

	if (uname[0] == '\0')
		g_strlcpy(uname, "B站用户", sizeof(uname));
	if (gift_name[0] == '\0')
		g_strlcpy(gift_name, "礼物", sizeof(gift_name));

	num = gift_read_count(data);
	if (action[0] == '\0') {
		const char *default_action =
			(strcmp(command, "GUARD_BUY") == 0) ? "开通" : "投喂";

		g_strlcpy(action, default_action, sizeof(action));
	}

	/* item 是本地弹幕值对象；dispatch_danmu 会复制后异步投递。 */
	g_strlcpy(item.uid, uid[0] ? uid : "0", sizeof(item.uid));
	g_strlcpy(item.uname, uname, sizeof(item.uname));
	(void)snprintf(item.text, sizeof(item.text), GIFT_MSG_FMT,
		       action, gift_name, num);
	item.timestamp = (int64_t)time(NULL);

	/* 头像工作线程只接收 UID/URL/昵称的复制值，不持有 JSON 指针。 */
	avatar_get_or_fetch(item.uid, face_url[0] ? face_url : NULL,
			    item.uname, item.avatar_path,
			    sizeof(item.avatar_path));
	dispatch_danmu(state, &item);
}

/*
 * 解析并处理单条 JSON 事件消息
 */
/**
 * @brief 按 cmd 字段分派 DANMU_MSG、SEND_GIFT、COMBO_SEND 和 GUARD_BUY 事件。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param json NUL 结尾的只读 JSON 文本；本函数不拥有其内存，解析 token 指针不得逃逸其生命周期。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static void handle_event(struct app_state *state, const char *json)
{
	char command[128] = "";
	char *colon;

	if (!json_get_string(json, "cmd", command, sizeof(command)))
		return;

	colon = strchr(command, ':');
	if (colon)
		*colon = '\0';

	if (strcmp(command, "DANMU_MSG") == 0) {
		handle_danmu_msg(state, json);
		return;
	}

	/* 仅接收真正代表赠礼/开通交易的事件。GIFT_STAR_PROCESS、
	 * ROOM_TOAST_MSG 等进度或提示广播不能伪装成用户礼物。 */
	if (strcmp(command, "SEND_GIFT") == 0 ||
	    strcmp(command, "COMBO_SEND") == 0 ||
	    strcmp(command, "GUARD_BUY") == 0) {
		handle_gift_msg(state, json, command);
		return;
	}
}

static size_t process_packets(struct app_state *state,
			     const unsigned char *data, size_t length);

/*
 * 解压并递归分发 Deflate 压缩的协议数据包
 */
/**
 * @brief 解压压缩包体后递归送入 B 站二进制包解析器，并释放临时缓冲区。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param body 调用者拥有的只读输入字节/文本缓冲区；仅在本次调用期间读取。
 * @param body_len body 指向的数据包体有效长度，单位为字节。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static void process_compressed_packet(struct app_state *state,
				      const unsigned char *body,
				      size_t body_len)
{
	/* plain 是 decompress_zlib() 分配的堆缓冲，处理完必须 free。 */
	unsigned char *plain = NULL;
	size_t plain_len = 0;

	if (!decompress_zlib(body, body_len, &plain, &plain_len))
		return;

	(void)process_packets(state, plain, plain_len);
	free(plain);
}

/*
 * 提取并派发普通 JSON 格式事件包
 */
/**
 * @brief 把 JSON 包体复制成 NUL 结尾字符串，解析后释放临时副本。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param body 调用者拥有的只读输入字节/文本缓冲区；仅在本次调用期间读取。
 * @param body_len body 指向的数据包体有效长度，单位为字节。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static void process_json_packet(struct app_state *state,
				const unsigned char *body,
				size_t body_len)
{
	/* json 是从二进制包体复制出的临时 NUL 结尾文本，调用后由本函数 free。 */
	char *json;

	json = malloc(body_len + 1);
	if (!json)
		return;

	memcpy(json, body, body_len);
	json[body_len] = '\0';
	handle_event(state, json);
	free(json);
}

/*
 * 递归解析处理连续二进制数据包流
 */
/**
 * @brief 按包头长度、版本和操作码解析一个或多个连续 B 站协议包。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param data 只读借用指针；本函数不修改或释放所指向的对象。
 * @param length 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return 返回处理或消耗的字节数。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static size_t process_packets(struct app_state *state,
			     const unsigned char *data, size_t length)
{
	const size_t max_packet_size = 16U * 1024U * 1024U;
	size_t offset = 0;

	while (offset + 16 <= length) {
		uint32_t packet_len = get_be32(data + offset);
		uint16_t header_len = get_be16(data + offset + 4);
		uint16_t version = get_be16(data + offset + 6);
		uint32_t operation = get_be32(data + offset + 8);
		const unsigned char *body;
		size_t body_len;

		/* 非法包头丢弃当前缓冲；不完整包体留给下一次接收。 */
		if (packet_len < 16 || header_len < 16 ||
		    packet_len < header_len || packet_len > max_packet_size)
			return length;
		if ((size_t)packet_len > length - offset)
			break;

		body = data + offset + header_len;
		body_len = packet_len - header_len;

		if (version == 2)
			process_compressed_packet(state, body, body_len);
		else if (operation == 8)
			update_status(state,
					      "已连接 B 站直播弹幕长连接", true);
		else if (operation == 5 && (version == 0 || version == 1))
			process_json_packet(state, body, body_len);

		offset += packet_len;
	}

	return offset;
}

/*
 * 完整发送一条 WebSocket 数据帧。
 * CONNECT_ONLY 模式下 curl_ws_send 可能只消费部分数据或返回 CURLE_AGAIN，
 * 必须等待 socket 可写并继续发送剩余部分，不能把部分发送当作成功。
 */
/**
 * @brief 处理非阻塞 WebSocket 发送，直到完整发送负载或遇到不可恢复错误。
 * @param curl 已创建的 CURL easy handle；本函数临时使用，不在此处销毁句柄。
 * @param sockfd 与 CURL WebSocket 连接对应的 socket 文件描述符，仅用于 poll 等待可读/可写。
 * @param payload 调用者拥有的只读输入字节/文本缓冲区；仅在本次调用期间读取。
 * @param payload_len payload 指向的待发送 WebSocket 负载长度，单位为字节。
 * @param flags 传递给 libcurl WebSocket 发送接口的帧标志位，例如文本/二进制帧及帧结束标志。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static bool send_ws_payload(CURL *curl, curl_socket_t sockfd,
			    const unsigned char *payload, size_t payload_len,
			    unsigned int flags)
{
	size_t offset = 0;

	while (offset < payload_len) {
		struct pollfd pfd;
		size_t sent = 0;
		CURLcode res;
		int poll_res;

		res = curl_ws_send(curl, payload + offset,
				   payload_len - offset, &sent, 0, flags);
		if (sent > payload_len - offset)
			return false;
		offset += sent;

		if (res == CURLE_OK) {
			if (sent == 0 && offset < payload_len)
				return false;
			continue;
		}
		if (res != CURLE_AGAIN)
			return false;

		pfd.fd = sockfd;
		pfd.events = POLLOUT;
		pfd.revents = 0;
		do {
			poll_res = poll(&pfd, 1, 1000);
		} while (poll_res < 0 && errno == EINTR);
		if (poll_res <= 0 ||
		    (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
			return false;
	}

	return true;
}

/*
 * 定期向 B 站弹幕服务器发送心跳保持包
 */
/**
 * @brief 按 B 站协议发送心跳包，并处理发送缓冲区与重试状态。
 * @param curl 已创建的 CURL easy handle；本函数临时使用，不在此处销毁句柄。
 * @param sockfd 与 CURL WebSocket 连接对应的 socket 文件描述符，仅用于 poll 等待可读/可写。
 * @param last_heartbeat 调用者提供的 time_t 存储；本函数读取并更新上次心跳时间，不取得其所有权。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static bool send_heartbeat(CURL *curl, curl_socket_t sockfd,
			   time_t *last_heartbeat)
{
	time_t now = time(NULL);
	unsigned char *hb_pkt;
	size_t hb_len = 0;

	if (now - *last_heartbeat < 30)
		return true;

	*last_heartbeat = now;
	hb_pkt = make_packet(2, "[object Object]", &hb_len);
	if (!hb_pkt)
		return false;

	{
		bool ok = send_ws_payload(curl, sockfd, hb_pkt, hb_len,
					   CURLWS_BINARY);

		free(hb_pkt);
		return ok;
	}
}

/*
 * 接收并分发 WebSocket 数据帧
 */
/**
 * @brief 先排空 libcurl WebSocket 缓冲，重组帧并将完整协议字节交给解析器。
 * @param curl 已创建的 CURL easy handle；本函数临时使用，不在此处销毁句柄。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param received 调用者拥有的 str_buf；本函数追加 WebSocket 消息数据并重置已消费长度，不转移缓冲所有权。
 * @param packet_stream 调用者拥有的 str_buf；本函数追加数据、移除已解析包并保留不完整协议残包。
 * @return 0 表示 libcurl 当前缓冲已排空、可回到 poll 等待；-1 表示连接关闭、协议数据异常或接收失败。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static int handle_ws_incoming_data(CURL *curl, struct app_state *state,
				   struct str_buf *received,
				   struct str_buf *packet_stream)
{
	/* chunk 是本次 recv 的栈缓冲；bytes 消费后立即复制进 received。 */
	unsigned char chunk[65536];
	/* 限制单条 WebSocket 消息及待处理协议流，防止异常输入无限占用内存。 */
	const size_t max_message_size = 16U * 1024U * 1024U;

	/*
	 * Keep draining libcurl until it explicitly reports CURLE_AGAIN.
	 * A single poll() readiness event may correspond to multiple chunks
	 * already buffered inside libcurl. Returning after one curl_ws_recv()
	 * can leave those bytes unread until another network event arrives.
	 */
	for (;;) {
		/* meta 由 libcurl 管理，仅在当前 curl_ws_recv() 结果期间借用。 */
		const struct curl_ws_frame *meta = NULL;
		size_t read_bytes = 0;
		CURLcode res;

		res = curl_ws_recv(curl, chunk, sizeof(chunk), &read_bytes,
				   &meta);
		if (res == CURLE_AGAIN)
			return 0;
		if (res != CURLE_OK || !meta)
			return -1;

		if (meta->flags & CURLWS_CLOSE)
			return -1;

		/* libcurl automatically answers PING; control frames are not
		 * part of the Bilibili binary application message. */
		if (meta->flags & (CURLWS_PING | CURLWS_PONG))
			continue;

		if (read_bytes > 0) {
			if (received->len > max_message_size ||
			    read_bytes > max_message_size - received->len ||
			    !sb_append_n(received, (const char *)chunk,
					 read_bytes)) {
				/* A malformed or excessive message must not poison the
				 * next packet. Reset and reconnect cleanly. */
				received->len = 0;
				if (received->data)
					received->data[0] = '\0';
				return -1;
			}
		}

		/* More bytes remain in this frame: immediately ask libcurl for
		 * them. It returns CURLE_AGAIN only when socket readiness is
		 * needed, at which point the outer poll() waits. */
		if (meta->bytesleft > 0)
			continue;

		/* CURLWS_CONT marks a non-final fragment. Reassemble the full
		 * WebSocket message before parsing Bilibili protocol packets. */
		if (meta->flags & CURLWS_CONT)
			continue;

		if (received->len > 0) {
			size_t consumed;

			/* WebSocket 消息边界不一定与 B 站协议包边界重合。 */
			if (packet_stream->len > max_message_size ||
			    received->len > max_message_size - packet_stream->len ||
			    !sb_append_n(packet_stream, received->data,
					 received->len)) {
				packet_stream->len = 0;
				if (packet_stream->data)
					packet_stream->data[0] = '\0';
				received->len = 0;
				if (received->data)
					received->data[0] = '\0';
				return -1;
			}

			received->len = 0;
			if (received->data)
				received->data[0] = '\0';

			consumed = process_packets(state,
				(const unsigned char *)packet_stream->data,
				packet_stream->len);
			if (consumed > 0) {
				size_t remaining = packet_stream->len - consumed;

				memmove(packet_stream->data,
					packet_stream->data + consumed, remaining);
				packet_stream->len = remaining;
				packet_stream->data[remaining] = '\0';
			}
		}
		/* Continue draining: more complete WebSocket messages can
		 * already be waiting in libcurl's internal receive buffer. */
	}
}

/*
 * 建立 WebSocket 长连接并进行事件接收与心跳维护
 */
/**
 * @brief 建立直播 WebSocket 长连接，完成协议认证、收包、心跳和断线清理。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param host 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param port B 站弹幕 WebSocket 服务端提供的 TCP 端口号。
 * @param token 只读借用指针；本函数不修改或释放所指向的对象。
 * @param buvid 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param room_id B 站直播间房间号；使用真实房间号而非短链或用户 UID。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static bool connect_stream(struct app_state *state, const char *host, int port,
			   const char *token, const char *buvid,
			   long long room_id)
{
	char ws_url[512];
	/* curl、headers、auth_packet 均由本连接函数创建并在统一清理路径释放。 */
	CURL *curl;
	CURLcode res;
	struct curl_slist *headers = NULL;
	curl_socket_t sockfd = CURL_SOCKET_BAD; /* 从 CURL 获取的连接 socket，不自行 close。 */
	char auth[2048];
	size_t auth_len = 0;
	unsigned char *auth_packet; /* make_packet() 分配，发送后由本函数 free。 */
	/* received 暂存当前 WS 消息；packet_stream 保存尚未组成完整 B 站包的残字节。 */
	struct str_buf received = { 0 };
	struct str_buf packet_stream = { 0 };
	time_t last_heartbeat;

	if (port && port != 443) {
		snprintf(ws_url, sizeof(ws_url), "wss://%s:%d/sub",
			 host, port);
	} else {
		snprintf(ws_url, sizeof(ws_url), "wss://%s/sub", host);
	}

	curl = curl_easy_init();
	if (!curl)
		return false;

	curl_easy_setopt(curl, CURLOPT_URL, ws_url);
	curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);

	headers = curl_slist_append(headers,
		"Origin: https://live.bilibili.com");
	headers = curl_slist_append(headers,
		"User-Agent: Mozilla/5.0 (X11; Linux x86_64)");

	if (state->cookie[0] != '\0') {
		char cookie_hdr[8192];

		snprintf(cookie_hdr, sizeof(cookie_hdr), "Cookie: %s",
			 state->cookie);
		headers = curl_slist_append(headers, cookie_hdr);
	}

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	res = curl_easy_perform(curl);
	if (res != CURLE_OK)
		/* 连接失败，跳转清理资源 */
		goto err_cleanup_curl;

	curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd);
	if (sockfd == CURL_SOCKET_BAD)
		/* 获取套接字失败，跳转清理资源 */
		goto err_cleanup_curl;

	/* 构造并发送鉴权数据包 */
	snprintf(auth, sizeof(auth),
		 "{\"uid\":%lld,\"roomid\":%lld,\"protover\":2,\"buvid\":\"%s\","
		 "\"platform\":\"web\",\"type\":2,\"key\":\"%s\"}",
		 state->account_uid, room_id, buvid, token);

	auth_packet = make_packet(7, auth, &auth_len);
	if (!auth_packet)
		/* 生成鉴权报文失败，跳转清理资源 */
		goto err_cleanup_curl;

	if (!send_ws_payload(curl, sockfd, auth_packet, auth_len,
			      CURLWS_BINARY)) {
		free(auth_packet);
		/* 发送鉴权报文失败，跳转清理资源 */
		goto err_cleanup_curl;
	}
	free(auth_packet);

	sb_init(&received, 65536);
	if (!received.data)
		goto err_cleanup_curl;
	sb_init(&packet_stream, 65536);
	if (!packet_stream.data)
		goto err_cleanup_curl;
	last_heartbeat = time(NULL);

	/* 事件驱动接收与心跳循环 */
	while (g_atomic_int_get(&g_bili_running) &&
	       g_atomic_int_get(&state->running)) {
		struct pollfd pfd;
		int poll_res;

		/*
		 * 先排空 libcurl 内部缓冲，再等待 socket。CONNECT_ONLY
		 * 握手期间可能已预读首批 WS 数据，这些数据不一定会再次
		 * 触发底层 socket 可读事件。
		 */
		if (handle_ws_incoming_data(curl, state, &received,
					     &packet_stream) < 0)
			break;

		pfd.fd = sockfd;
		pfd.events = POLLIN | POLLPRI;
		pfd.revents = 0;
		poll_res = poll(&pfd, 1, 1000);

		if (poll_res < 0 && errno == EINTR)
			continue;
		if (poll_res < 0)
			break;

		if (!send_heartbeat(curl, sockfd, &last_heartbeat))
			break;

		if (poll_res == 0)
			continue;

		if ((pfd.revents & (POLLIN | POLLPRI)) &&
		    handle_ws_incoming_data(curl, state, &received,
					    &packet_stream) < 0)
			break;

		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			break;
	}

/* 错误与连接分层清理：释放协议缓存、请求头与 curl 句柄 */
err_cleanup_curl:
	sb_free(&received);
	sb_free(&packet_stream);
	if (headers)
		curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return false;
}

/*
 * 可中断式休眠指定毫秒
 */
/**
 * @brief 以短间隔检查运行标志的可中断休眠，避免退出时等待完整退避周期。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @param total_ms 本次拖拽/延迟测试的总耗时，单位为毫秒。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static void interruptible_sleep_ms(struct app_state *state, int total_ms)
{
	int i;
	int steps = total_ms / 100;

	for (i = 0; i < steps &&
	     g_atomic_int_get(&g_bili_running) &&
	     g_atomic_int_get(&state->running); i++)
		g_usleep(100000);
}

/*
 * 后台长连接主线程工作循环
 */
/**
 * @brief 后台网络工作线程：发现服务器、重连直播间并驱动 WebSocket 收包循环。
 * @param parameter 线程入口参数；指向启动线程时传入的应用状态或控制上下文，按函数实现借用。
 * @return 线程返回指针；当前实现返回 NULL，不转移业务对象所有权。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
static gpointer bili_worker_thread(gpointer parameter)
{
	struct app_state *state = parameter;

	while (g_atomic_int_get(&g_bili_running) &&
	       g_atomic_int_get(&state->running)) {
		char buvid[256] = "";
		char host[256] = "";
		char token[1024] = "";
		int port = 443;
		long long room = state->room_id;
		char status_msg[512];

		if (room <= 0) {
			interruptible_sleep_ms(state, 500);
			continue;
		}

		update_status(state, "正在获取直播间弹幕配置...", false);

		if (!discover_danmu_info(state, buvid, sizeof(buvid),
					 host, sizeof(host), &port,
					 token, sizeof(token), &room)) {
			update_status(state,
				      "获取弹幕令牌失败，5 秒后重试", false);
			interruptible_sleep_ms(state, 5000);
			continue;
		}

		if (state->account_uid > 0)
			snprintf(status_msg, sizeof(status_msg),
				 "已登录 UID:%lld，正在连接 %s:%d",
				 state->account_uid, host, port);
		else
			snprintf(status_msg, sizeof(status_msg),
				 "游客模式（昵称可能脱敏），正在连接 %s:%d",
				 host, port);
		update_status(state, status_msg, false);

		connect_stream(state, host, port, token, buvid, room);

		update_status(state, "长连接已断开，5 秒后尝试重连", false);
		interruptible_sleep_ms(state, 5000);
	}

	update_status(state, "已停止监听", false);
	return NULL;
}

/*
 * 启动 B 站直播间长连接监听线程
 */
/**
 * @brief 设置共享状态并启动唯一的 B 站后台网络线程。
 * @param state 应用共享状态对象；借用指针，不转移所有权。GTK 控件成员只能在 GTK 主线程访问。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
bool bilibili_start(struct app_state *state)
{
	if (!state || g_atomic_int_get(&g_bili_running))
		return false;

	g_atomic_int_set(&g_bili_running, 1);
	g_bili_thread = g_thread_new("bilibili_client",
				     bili_worker_thread, state);
	if (!g_bili_thread) {
		g_atomic_int_set(&g_bili_running, 0);
		return false;
	}
	return true;
}

/*
 * 停止并安全退出长连接线程
 */
/**
 * @brief 设置停止标志并等待 B 站网络线程安全退出。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
void bilibili_stop(void)
{
	g_atomic_int_set(&g_bili_running, 0);
	if (g_bili_thread) {
		g_thread_join(g_bili_thread);
		g_bili_thread = NULL;
	}
}

/*
 * 注册弹幕到达回调函数
 */
/**
 * @brief 在线程锁保护下注册弹幕派发回调及其不透明用户数据。
 * @param cb 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
void bilibili_set_danmu_callback(void (*cb)(struct app_state *state,
					    const struct danmu_item *item,
					    void *user_data),
				 void *user_data)
{
	g_mutex_lock(&g_cb_lock);
	g_danmu_cb = cb;
	g_danmu_user_data = user_data;
	g_mutex_unlock(&g_cb_lock);
}

/*
 * 注册直播连接状态更新回调函数
 */
/**
 * @brief 在线程锁保护下注册连接状态回调及其不透明用户数据。
 * @param cb 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 网络工作线程/协议处理路径；JSON token 指针只在输入包缓冲有效期间使用。
 */
void bilibili_set_status_callback(void (*cb)(struct app_state *state,
					     const char *status,
					     bool connected,
					     void *user_data),
				  void *user_data)
{
	g_mutex_lock(&g_cb_lock);
	g_status_cb = cb;
	g_status_user_data = user_data;
	g_mutex_unlock(&g_cb_lock);
}
