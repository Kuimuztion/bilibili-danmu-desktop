/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Copyright (C) 2026 Kuimuztion
 *
 * B站用户头像异步下载、圆形裁剪与本地磁盘缓存管理
 */

#include "avatar.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>
#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cairo.h>

/*
 * 待下载头像队列节点
 */
struct avatar_task {
	/* 队列节点由 malloc 分配；成功入队后所有权转移给 avatar_manager。 */
	char uid[64];
	char face_url[512];
	char uname[128];
	struct avatar_task *next; /* 指向下一个任务节点；由 g_mgr.lock 保护队列链接。 */
};

/*
 * 传递给主线程 idle 回调的数据包装
 */
struct avatar_notify_data {
	/* GLib 主线程派发快照，不含借用的临时字符串指针。 */
	char uid[64];
	char path[1024];
	char real_uname[128];
};

/*
 * 头像模块上下文结构
 */
struct avatar_manager {
	/* 模块级单例；队列、回调和 running 字段通过 lock 同步。 */
	char cache_dir[512];
	char default_avatar[512];
	char cookie[4096];

	/* 任务链表队列与同步原语 */
	struct avatar_task *queue_head;
	struct avatar_task *queue_tail;
	GMutex lock;
	GCond cond;
	GThread *worker_thread;
	bool running;

	/* 更新完成回调 */
	void (*update_cb)(const char *uid, const char *path,
			  const char *real_uname, void *data);
	void *update_data;
};

static struct avatar_manager g_mgr;

/*
 * curl 数据写入回调函数
 */
/**
 * @brief libcurl 图片/资料卡响应回调，将网络字节追加到指定 str_buf。
 * @param ptr 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @param size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @param nmemb libcurl 本次回调提供的元素数量；与 size 相乘得到当前数据块字节数。
 * @param userdata 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @return 返回处理或消耗的字节数。
 * @thread 头像工作线程路径（初始化/清理 API 除外）；共享队列字段由 g_mgr.lock 保护。
 */
static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb,
			    void *userdata)
{
	struct str_buf *sb = userdata;
	size_t total = size * nmemb;

	if (!sb_append_n(sb, ptr, total))
		return 0;

	return total;
}

/*
 * 将 GdkPixbuf (RGBA) 转换为 cairo_surface_t (ARGB32)
 */
/**
 * @brief 把 GdkPixbuf 像素转换为 Cairo ARGB32 图像表面，返回新建表面所有权。
 * @param pixbuf GdkPixbuf 图像对象；本函数只读像素数据，不接管调用方引用。
 * @return 成功时返回新建 Cairo Surface，调用者负责 cairo_surface_destroy；失败返回 NULL。
 * @thread 头像工作线程路径（初始化/清理 API 除外）；共享队列字段由 g_mgr.lock 保护。
 */
static cairo_surface_t *pixbuf_to_cairo_surface(GdkPixbuf *pixbuf)
{
	int width = gdk_pixbuf_get_width(pixbuf);
	int height = gdk_pixbuf_get_height(pixbuf);
	int n_channels = gdk_pixbuf_get_n_channels(pixbuf);
	int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
	/* pixels 是 GdkPixbuf 内部像素区的借用指针，不得释放或跨越 pixbuf 生命周期。 */
	const guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);
	/* surface 由本函数创建并在成功时转移给调用者。 */
	cairo_surface_t *surface;
	/* cairo_data 指向 surface 自己管理的像素存储，不单独 free。 */
	unsigned char *cairo_data;
	int cairo_stride;
	int y;
	int x;

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
					     width, height);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
		return NULL;

	cairo_data = cairo_image_surface_get_data(surface);
	cairo_stride = cairo_image_surface_get_stride(surface);

	cairo_surface_flush(surface);
	for (y = 0; y < height; y++) {
		const guchar *src = pixels + y * rowstride;
		uint32_t *dst = (uint32_t *)(cairo_data + y * cairo_stride);

		for (x = 0; x < width; x++) {
			uint8_t r = src[0];
			uint8_t g = src[1];
			uint8_t b = src[2];
			uint8_t a = (n_channels == 4) ? src[3] : 255;
			uint32_t pr = (r * a + 127) / 255;
			uint32_t pg = (g * a + 127) / 255;
			uint32_t pb = (b * a + 127) / 255;

			dst[x] = (a << 24) | (pr << 16) | (pg << 8) | pb;
			src += n_channels;
		}
	}
	cairo_surface_mark_dirty(surface);
	return surface;
}

/*
 * 将 Pixbuf 进行高质量双线性缩放、圆形遮罩裁剪并保存为 PNG
 */
/**
 * @brief 缩放图片、创建圆形裁剪遮罩并把头像写入 PNG 文件。
 * @param pixbuf GdkPixbuf 图像对象；本函数只读像素数据，不接管调用方引用。
 * @param out_path 只读借用指针；本函数不修改或释放所指向的对象。
 * @param size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 头像工作线程路径（初始化/清理 API 除外）；共享队列字段由 g_mgr.lock 保护。
 */
static bool crop_circle_and_save(GdkPixbuf *pixbuf, const char *out_path,
				 int size)
{
	/* scaled 是 scale_simple() 返回的新引用，必须在转换后 unref。 */
	GdkPixbuf *scaled;
	/* src_surf/dst_surf/cr 均由本函数创建，并在正常/错误出口销毁。 */
	cairo_surface_t *src_surf;
	cairo_surface_t *dst_surf;
	cairo_t *cr;
	cairo_status_t status;
	double radius;

	if (!pixbuf || !out_path || size <= 0)
		return false;

	scaled = gdk_pixbuf_scale_simple(pixbuf, size, size,
					 GDK_INTERP_BILINEAR);
	if (!scaled)
		return false;

	src_surf = pixbuf_to_cairo_surface(scaled);
	g_object_unref(scaled);
	if (!src_surf)
		return false;

	dst_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	if (cairo_surface_status(dst_surf) != CAIRO_STATUS_SUCCESS)
		/* 目标表面创建失败，清理源表面退出 */
		goto err_free_src;

	cr = cairo_create(dst_surf);
	if (cairo_status(cr) != CAIRO_STATUS_SUCCESS)
		/* Cairo 绘图上下文创建失败，清理表面退出 */
		goto err_free_dst;

	radius = (double)size / 2.0;
	cairo_arc(cr, radius, radius, radius, 0.0, 2.0 * G_PI);
	cairo_clip(cr);

	cairo_set_source_surface(cr, src_surf, 0.0, 0.0);
	cairo_paint(cr);

	status = cairo_surface_write_to_png(dst_surf, out_path);

	cairo_destroy(cr);
	cairo_surface_destroy(dst_surf);
	cairo_surface_destroy(src_surf);
	return status == CAIRO_STATUS_SUCCESS;

/* 错误分层清理：释放目标 surface */
err_free_dst:
	cairo_surface_destroy(dst_surf);
/* 错误分层清理：释放源 surface */
err_free_src:
	cairo_surface_destroy(src_surf);
	return false;
}

/*
 * GTK 主线程空闲回调分发
 */
/**
 * @brief 在 GTK 主线程执行头像更新回调，随后释放通知载荷。
 * @param user_data 由 notify_avatar_updated() 分配并转交给 GLib source 的通知结构；本回调负责 free。
 * @return GLib 回调控制值：G_SOURCE_REMOVE 表示移除源，G_SOURCE_CONTINUE 表示继续。
 * @thread GLib 主线程回调；调用 UI 通知后释放 heap 分配的通知结构。
 */
static gboolean on_avatar_downloaded_idle(gpointer user_data)
{
	/* user_data 是 notify_avatar_updated() 分配并转移给 GLib source 的载荷；本回调负责 free。 */
	struct avatar_notify_data *data = user_data;

	if (g_mgr.update_cb)
		g_mgr.update_cb(data->uid, data->path, data->real_uname,
				g_mgr.update_data);

	free(data);
	return G_SOURCE_REMOVE;
}

/*
 * 向主事件循环派发头像下载完成通知
 */
/**
 * @brief 复制 UID、头像路径和昵称后投递主线程通知；不跨线程传递临时字符串。
 * @param uid 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param path 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param real_uname 只读借用指针；本函数不修改或释放所指向的对象。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 可由头像工作线程调用；仅复制通知数据并排入 GLib 主循环。
 */
static void notify_avatar_updated(const char *uid, const char *path,
				  const char *real_uname)
{
	struct avatar_notify_data *data;

	if (!g_mgr.update_cb)
		return;

	data = malloc(sizeof(*data));
	if (!data)
		return;

	g_strlcpy(data->uid, uid, sizeof(data->uid));
	g_strlcpy(data->path, path, sizeof(data->path));
	if (real_uname)
		g_strlcpy(data->real_uname, real_uname,
			  sizeof(data->real_uname));
	else
		data->real_uname[0] = '\0';

	g_idle_add_full(G_PRIORITY_DEFAULT,
				 on_avatar_downloaded_idle, data, NULL);
}

/*
 * 校验 UID 是否为合法的真实账号标识或哈希标识
 */
/**
 * @brief 检查 UID 是否为非零数字账号标识或内部 hash 标识。
 * @param uid 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 头像工作线程路径（初始化/清理 API 除外）；共享队列字段由 g_mgr.lock 保护。
 */
static bool is_valid_uid(const char *uid)
{
	const char *p;

	if (!uid || uid[0] == '\0' || strcmp(uid, "0") == 0)
		return false;

	if (strncmp(uid, "hash:", 5) == 0)
		return true;

	for (p = uid; *p; p++) {
		if (!isdigit((unsigned char)*p))
			return false;
	}

	return true;
}

/*
 * 解码图片数据并裁剪为圆形 PNG 保存
 */
/**
 * @brief 从内存字节解码图片并调用圆形裁剪器保存到缓存路径。
 * @param data 调用者提供的只读原始字节缓冲区；本函数不取得其所有权。
 * @param len 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @param out_path 只读借用指针；本函数不修改或释放所指向的对象。
 * @param size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 头像工作线程路径（初始化/清理 API 除外）；共享队列字段由 g_mgr.lock 保护。
 */
static bool decode_and_save_avatar(const void *data, size_t len,
				   const char *out_path, int size)
{
	GdkPixbufLoader *loader;
	GdkPixbuf *pixbuf;
	bool ok = false;

	if (!data || len == 0 || !out_path)
		return false;

	loader = gdk_pixbuf_loader_new();
	if (!loader)
		return false;

	/* 写入图片数据失败，释放加载器退出 */
	if (!gdk_pixbuf_loader_write(loader, (const guchar *)data, len, NULL))
		goto out_unref_loader;

	/* 关闭加载器失败，释放加载器退出 */
	if (!gdk_pixbuf_loader_close(loader, NULL))
		goto out_unref_loader;

	pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
	/* 获取解码 Pixbuf 失败，释放加载器退出 */
	if (!pixbuf)
		goto out_unref_loader;

	ok = crop_circle_and_save(pixbuf, out_path, size);

/* 统一清理出口：释放 Pixbuf 加载器对象 */
out_unref_loader:
	g_object_unref(loader);
	return ok;
}

/*
 * 读取本地缓存的头像直链记录（辅助静态函数）
 */
/**
 * @brief 读取某 UID 上次使用的头像直链，输出写入调用者缓冲区。
 * @param path 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param buf 调用者拥有的可写输出缓冲区；本函数只写入有界结果，不释放该缓冲区。
 * @param size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 头像工作线程路径（初始化/清理 API 除外）；共享队列字段由 g_mgr.lock 保护。
 */
static void read_cached_url(const char *path, char *buf, size_t size)
{
	FILE *fp;
	char *nl;

	if (!path || !buf || size == 0)
		return;

	buf[0] = '\0';
	fp = fopen(path, "r");
	if (!fp)
		return;

	if (fgets(buf, size, fp)) {
		nl = strchr(buf, '\n');
		if (nl)
			*nl = '\0';
	}
	fclose(fp);
}

/*
 * 写入本地头像直链记录（辅助静态函数）
 */
/**
 * @brief 将头像 URL 写入缓存旁路文件，以便识别头像变更。
 * @param path 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param url 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 头像工作线程路径（初始化/清理 API 除外）；共享队列字段由 g_mgr.lock 保护。
 */
static void write_cached_url(const char *path, const char *url)
{
	FILE *fp;

	if (!path || !url || url[0] == '\0')
		return;

	fp = fopen(path, "w");
	if (!fp)
		return;

	fputs(url, fp);
	fclose(fp);
}

/*
 * 判断用户名是否仍是 B 站的脱敏形式
 */
/**
 * @brief 识别星号脱敏或程序占位昵称，供真实昵称补全流程使用。
 * @param uname 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 头像工作线程路径（初始化/清理 API 除外）；共享队列字段由 g_mgr.lock 保护。
 */
static bool username_is_masked(const char *uname)
{
	if (!uname || uname[0] == '\0' || strchr(uname, '*') != NULL)
		return true;

	return strcmp(uname, "B站用户") == 0 ||
	       strcmp(uname, "热心观众") == 0;
}

/*
 * 执行单个 UID 的卡片查询、头像下载与圆形裁剪。
 * 昵称补全与头像下载分离：命中头像缓存时仍可刷新被脱敏的昵称。
 */
/**
 * @brief 后台处理单个头像任务：查询用户卡片、下载头像、缓存并派发更新。
 * @param task 当前头像任务节点；只读借用，节点由头像队列工作线程在处理完成后释放。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 头像工作线程路径（初始化/清理 API 除外）；共享队列字段由 g_mgr.lock 保护。
 */
static void process_avatar_download(const struct avatar_task *task)
{
	char cache_path[1024];
	char url_path[1024];
	char card_url[256];
	char face_url[512] = "";
	char full_face_url[768];
	char img_url[1024];
	char real_uname[128] = "";
	char card_uname[128] = "";
	char old_url[512] = "";
	char cookie_header[4200];
	/* 两个响应缓冲分别拥有资料卡 JSON 和头像原始字节，统一 sb_free。 */
	struct str_buf card_sb;
	struct str_buf img_sb;
	struct stat st;
	time_t now;
	/* task 由工作线程从队列取出并借用；调用者在本函数返回后释放 task。 */
	/* curl/headers 由当前任务拥有，统一在 err_cleanup_curl 路径释放。 */
	CURL *curl = NULL;
	CURLcode res;
	struct curl_slist *headers = NULL;
	bool success = false;
	bool cache_valid = false;
	bool need_name_lookup;
	bool is_hash_uid;

	if (!task || !is_valid_uid(task->uid))
		return;

	now = time(NULL);
	snprintf(cache_path, sizeof(cache_path), "%s/%s.png",
		 g_mgr.cache_dir, task->uid);
	snprintf(url_path, sizeof(url_path), "%s/%s.url",
		 g_mgr.cache_dir, task->uid);

	if (task->uname[0] != '\0')
		g_strlcpy(real_uname, task->uname, sizeof(real_uname));
	is_hash_uid = strncmp(task->uid, "hash:", 5) == 0;
	need_name_lookup = !is_hash_uid && username_is_masked(real_uname);

	/* 检查用户是否更换了头像直链，若变动立即作废旧缓存 */
	if (task->face_url[0] != '\0') {
		read_cached_url(url_path, old_url, sizeof(old_url));
		if (strcmp(old_url, task->face_url) != 0)
			unlink(cache_path);
		g_strlcpy(face_url, task->face_url, sizeof(face_url));
	}

	/* 检查有效头像缓存；即使缓存新鲜，脱敏昵称仍需单独补全 */
	if (stat(cache_path, &st) == 0 && st.st_size >= 1024 &&
	    difftime(now, st.st_mtime) <= 30.0)
		cache_valid = true;

	if (cache_valid && !need_name_lookup)
		return;

	sb_init(&card_sb, 1024);
	sb_init(&img_sb, 16384);

	curl = curl_easy_init();
	if (!curl)
		goto err_free_sb;

	headers = curl_slist_append(headers,
		"User-Agent: Mozilla/5.0 (X11; Linux x86_64)");
	headers = curl_slist_append(headers,
		"Referer: https://www.bilibili.com/");
	if (g_mgr.cookie[0] != '\0') {
		snprintf(cookie_header, sizeof(cookie_header), "Cookie: %s",
			 g_mgr.cookie);
		headers = curl_slist_append(headers, cookie_header);
	}

	/* 数字 UID 可通过用户资料卡补全昵称；匿名 hash 不能反查 */
	if (!is_hash_uid && (need_name_lookup || face_url[0] == '\0')) {
		snprintf(card_url, sizeof(card_url),
			 "https://api.bilibili.com/x/web-interface/card?mid=%s",
			 task->uid);

		curl_easy_setopt(curl, CURLOPT_URL, card_url);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &card_sb);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 6L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

		res = curl_easy_perform(curl);
		if (res == CURLE_OK && card_sb.data) {
			if (json_get_string(card_sb.data, "name", card_uname,
					    sizeof(card_uname)) &&
			    !username_is_masked(card_uname))
				g_strlcpy(real_uname, card_uname,
					 sizeof(real_uname));
			if (face_url[0] == '\0')
				json_get_string(card_sb.data, "face", face_url,
						 sizeof(face_url));
		}
	}

	/* 头像缓存有效时，仅派发昵称更新，无需重复下载头像 */
	if (cache_valid) {
		if (!username_is_masked(real_uname))
			notify_avatar_updated(task->uid, cache_path, real_uname);
		goto err_cleanup_curl;
	}

	/* 匿名 hash 没有资料卡；无直链时只能使用默认头像 */
	if (face_url[0] == '\0')
		goto err_cleanup_curl;

	/* 直接使用事件或资料卡给出的头像地址 */
	if (strncmp(face_url, "//", 2) == 0)
		snprintf(full_face_url, sizeof(full_face_url),
			 "https:%s", face_url);
	else
		g_strlcpy(full_face_url, face_url, sizeof(full_face_url));

	if (strstr(full_face_url, "@"))
		g_strlcpy(img_url, full_face_url, sizeof(img_url));
	else
		snprintf(img_url, sizeof(img_url), "%s@96w_96h.png",
			 full_face_url);

	curl_easy_reset(curl);
	curl_easy_setopt(curl, CURLOPT_URL, img_url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &img_sb);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

	res = curl_easy_perform(curl);
	if (res != CURLE_OK || !img_sb.data || img_sb.len == 0)
		goto err_cleanup_curl;

	success = decode_and_save_avatar(img_sb.data, img_sb.len,
					 cache_path, 96);

err_cleanup_curl:
	if (headers)
		curl_slist_free_all(headers);
	if (curl)
		curl_easy_cleanup(curl);

err_free_sb:
	sb_free(&card_sb);
	sb_free(&img_sb);

	if (success) {
		write_cached_url(url_path, face_url);
		notify_avatar_updated(task->uid, cache_path, real_uname);
	} else {
		/* 头像请求失败时也不丢弃已查到的真实昵称；图片退回默认头像。 */
		if (!username_is_masked(real_uname)) {
			const char *notify_path = g_mgr.default_avatar;

			if (stat(cache_path, &st) == 0 && st.st_size >= 1024)
				notify_path = cache_path;
			notify_avatar_updated(task->uid, notify_path, real_uname);
		}

		if (stat(cache_path, &st) == 0 && st.st_size < 1024)
			unlink(cache_path);
	}
}

/*
 * 后台异步下载工作线程循环
 */
/**
 * @brief 等待任务队列并串行处理头像任务，消费队列节点并释放其堆内存。
 * @param data GLib 线程入口的 user_data 上下文指针；当前 worker 不取得其所有权。
 * @return 线程返回指针；当前实现返回 NULL，不转移业务对象所有权。
 * @thread 头像工作线程路径（初始化/清理 API 除外）；共享队列字段由 g_mgr.lock 保护。
 */
static gpointer avatar_worker_func(gpointer data)
{
	(void)data;

	while (true) {
		/* task 从队列头摘除后归当前 worker 所有，完成处理后由 worker free。 */
		struct avatar_task *task = NULL;

		g_mutex_lock(&g_mgr.lock);
		while (g_mgr.running && !g_mgr.queue_head)
			g_cond_wait(&g_mgr.cond, &g_mgr.lock);

		if (!g_mgr.running && !g_mgr.queue_head) {
			g_mutex_unlock(&g_mgr.lock);
			break;
		}

		task = g_mgr.queue_head;
		if (task) {
			g_mgr.queue_head = task->next;
			if (!g_mgr.queue_head)
				g_mgr.queue_tail = NULL;
		}
		g_mutex_unlock(&g_mgr.lock);

		if (!task)
			continue;

		process_avatar_download(task);
		free(task);
	}

	return NULL;
}

/*
 * 初始化头像管理模块与后台下载线程
 */
/**
 * @brief 初始化头像缓存目录、同步原语、Cookie 和后台工作线程。
 * @param cache_dir 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param cookie 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 头像工作线程路径（初始化/清理 API 除外）；共享队列字段由 g_mgr.lock 保护。
 */
bool avatar_init(const char *cache_dir, const char *cookie)
{
	const char *user_cache;

	memset(&g_mgr, 0, sizeof(g_mgr));
	if (cookie)
		g_strlcpy(g_mgr.cookie, cookie, sizeof(g_mgr.cookie));

	if (cache_dir && cache_dir[0] != '\0') {
		g_strlcpy(g_mgr.cache_dir, cache_dir,
			  sizeof(g_mgr.cache_dir));
	} else {
		user_cache = g_get_user_cache_dir();
		snprintf(g_mgr.cache_dir, sizeof(g_mgr.cache_dir),
			 "%s/bilibili-danmu-desktop/avatars", user_cache);
	}

	g_mkdir_with_parents(g_mgr.cache_dir, 0755);

	/* 解析默认头像路径 */
	g_strlcpy(g_mgr.default_avatar, "assets/default_avatar.png",
		  sizeof(g_mgr.default_avatar));

	g_mutex_init(&g_mgr.lock);
	g_cond_init(&g_mgr.cond);
	g_mgr.running = true;

	curl_global_init(CURL_GLOBAL_DEFAULT);

	g_mgr.worker_thread = g_thread_new("avatar_worker",
					   avatar_worker_func, NULL);
	return g_mgr.worker_thread != NULL;
}

/*
 * 查询头像本地缓存路径；若传入直链或未缓存则压入异步下载队列
 */
/**
 * @brief 查询有效头像缓存；未命中时将 UID/直链/昵称复制进后台任务队列。
 * @param uid 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param face_url 只读借用指针；本函数不修改或释放所指向的对象。
 * @param uname 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param path_out 调用者拥有的可写输出缓冲区；本函数只写入有界结果，不释放该缓冲区。
 * @param out_size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 头像工作线程路径（初始化/清理 API 除外）；共享队列字段由 g_mgr.lock 保护。
 */
bool avatar_get_or_fetch(const char *uid, const char *face_url,
			 const char *uname, char *path_out, size_t out_size)
{
	char cache_path[1024];
	struct stat st;
	struct avatar_task *cur;
	struct avatar_task *new_task;
	time_t now;
	bool cached_valid = false;

	if (!path_out || out_size == 0)
		return false;

	if (!is_valid_uid(uid)) {
		g_strlcpy(path_out, g_mgr.default_avatar, out_size);
		return false;
	}

	now = time(NULL);
	snprintf(cache_path, sizeof(cache_path), "%s/%s.png",
		 g_mgr.cache_dir, uid);

	/* 检查本地有效缓存，若在时效内直接返回 */
	if (stat(cache_path, &st) == 0 && st.st_size >= 1024) {
		g_strlcpy(path_out, cache_path, out_size);
		cached_valid = true;

		if ((!face_url || face_url[0] == '\0') &&
		    difftime(now, st.st_mtime) <= 30.0 &&
		    (strncmp(uid, "hash:", 5) == 0 ||
		     !username_is_masked(uname)))
			return true;
	} else {
		g_strlcpy(path_out, g_mgr.default_avatar, out_size);
	}

	/* 缓存失效或不存在，检查并压入异步下载队列 */
	g_mutex_lock(&g_mgr.lock);
	if (!g_mgr.running) {
		g_mutex_unlock(&g_mgr.lock);
		return cached_valid;
	}

	/* 检查是否已在队列中，若已存在则直接返回 */
	for (cur = g_mgr.queue_head; cur; cur = cur->next) {
		if (strcmp(cur->uid, uid) == 0) {
			if (face_url && face_url[0] != '\0')
				g_strlcpy(cur->face_url, face_url,
					 sizeof(cur->face_url));
			if (uname && uname[0] != '\0' &&
			    username_is_masked(cur->uname))
				g_strlcpy(cur->uname, uname,
					 sizeof(cur->uname));
			g_mutex_unlock(&g_mgr.lock);
			return cached_valid;
		}
	}

	new_task = malloc(sizeof(*new_task));
	if (!new_task) {
		g_mutex_unlock(&g_mgr.lock);
		return cached_valid;
	}

	g_strlcpy(new_task->uid, uid, sizeof(new_task->uid));
	if (face_url)
		g_strlcpy(new_task->face_url, face_url,
			  sizeof(new_task->face_url));
	else
		new_task->face_url[0] = '\0';

	if (uname)
		g_strlcpy(new_task->uname, uname, sizeof(new_task->uname));
	else
		new_task->uname[0] = '\0';

	new_task->next = NULL;

	if (g_mgr.queue_tail) {
		g_mgr.queue_tail->next = new_task;
		g_mgr.queue_tail = new_task;
	} else {
		g_mgr.queue_head = new_task;
		g_mgr.queue_tail = new_task;
	}

	g_cond_signal(&g_mgr.cond);
	g_mutex_unlock(&g_mgr.lock);

	return cached_valid;
}

/*
 * 查询头像本地缓存路径兼容入口
 */
/**
 * @brief 兼容接口：只查询本地头像并在必要时安排后台下载。
 * @param uid 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param path_out 调用者拥有的可写输出缓冲区；本函数只写入有界结果，不释放该缓冲区。
 * @param out_size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 头像工作线程路径（初始化/清理 API 除外）；共享队列字段由 g_mgr.lock 保护。
 */
bool avatar_get(const char *uid, char *path_out, size_t out_size)
{
	return avatar_get_or_fetch(uid, NULL, NULL, path_out, out_size);
}

/*
 * 注册头像异步下载完成的回调函数
 */
/**
 * @brief 在互斥锁下设置头像更新回调与不透明上下文指针。
 * @param cb 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @param user_data 回调上下文不透明指针；由注册回调的一方决定类型和生命周期，本函数不释放。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 头像工作线程路径（初始化/清理 API 除外）；共享队列字段由 g_mgr.lock 保护。
 */
void avatar_set_update_callback(void (*cb)(const char *uid,
					   const char *avatar_path,
					   const char *real_uname,
					   void *user_data),
				void *user_data)
{
	g_mutex_lock(&g_mgr.lock);
	g_mgr.update_cb = cb;
	g_mgr.update_data = user_data;
	g_mutex_unlock(&g_mgr.lock);
}

/*
 * 终止后台下载线程并清理模块资源
 */
/**
 * @brief 停止并 join 头像线程，释放剩余队列节点、锁和条件变量。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 头像工作线程路径（初始化/清理 API 除外）；共享队列字段由 g_mgr.lock 保护。
 */
void avatar_cleanup(void)
{
	struct avatar_task *task;
	struct avatar_task *tmp;

	g_mutex_lock(&g_mgr.lock);
	g_mgr.running = false;
	g_cond_broadcast(&g_mgr.cond);
	g_mutex_unlock(&g_mgr.lock);

	if (g_mgr.worker_thread) {
		g_thread_join(g_mgr.worker_thread);
		g_mgr.worker_thread = NULL;
	}

	/* 清理剩余队列任务 */
	g_mutex_lock(&g_mgr.lock);
	task = g_mgr.queue_head;
	while (task) {
		tmp = task->next;
		free(task);
		task = tmp;
	}
	g_mgr.queue_head = NULL;
	g_mgr.queue_tail = NULL;
	g_mutex_unlock(&g_mgr.lock);

	g_mutex_clear(&g_mgr.lock);
	g_cond_clear(&g_mgr.cond);

	curl_global_cleanup();
}
