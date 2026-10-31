/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Copyright (C) 2026 Kuimuztion
 *
 * B站直播间弹幕长连接与消息流解析
 */

#ifndef BILIBILI_H
#define BILIBILI_H

/*
 * B 站网络模块 API。
 * state 与 callback user_data 均为借用指针，调用方必须保证其生命周期覆盖
 * 网络线程运行及排队中的 GLib 主线程回调；GTK 控件不会在网络线程访问。
 */
#include <stdbool.h>
#include "app_state.h"

bool bilibili_start(struct app_state *state);
void bilibili_stop(void);

void bilibili_set_danmu_callback(void (*cb)(struct app_state *state,
					    const struct danmu_item *item,
					    void *user_data),
				 void *user_data);

void bilibili_set_status_callback(void (*cb)(struct app_state *state,
					     const char *status,
					     bool connected,
					     void *user_data),
				  void *user_data);

#endif /* BILIBILI_H */
