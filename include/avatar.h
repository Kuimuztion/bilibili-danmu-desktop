/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Copyright (C) 2026 Kuimuztion
 *
 * B站用户头像异步下载、圆形裁剪与本地磁盘缓存管理
 */

#ifndef AVATAR_H
#define AVATAR_H

/*
 * 头像模块 API。
 * path_out 是调用方提供的可写缓冲区；callback 收到的字符串仅在本次回调
 * 期间保证有效。avatar_cleanup() 会等待 worker 退出后再返回。
 */
#include <stdbool.h>
#include <stddef.h>

bool avatar_init(const char *cache_dir, const char *cookie);
void avatar_cleanup(void);

bool avatar_get(const char *uid, char *path_out, size_t out_size);
bool avatar_get_or_fetch(const char *uid, const char *face_url,
			 const char *uname, char *path_out, size_t out_size);
void avatar_set_update_callback(void (*cb)(const char *uid,
					   const char *avatar_path,
					   const char *real_uname,
					   void *user_data),
				void *user_data);

#endif /* AVATAR_H */
