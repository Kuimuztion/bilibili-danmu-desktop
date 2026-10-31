/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Copyright (C) 2026 Kuimuztion
 *
 * GTK4 原生桌面弹幕悬浮窗口与气泡渲染实现
 */

#ifndef UI_H
#define UI_H

/*
 * GTK UI 模块 API。
 * 所有函数均要求在 GTK 主线程调用。state/item/uid 等输入指针由调用方
 * 持有；GTK Widget 的生命周期由 app_state 与 GTK 父子对象树共同管理。
 */
#include <stdbool.h>
#include "app_state.h"

bool ui_init(struct app_state *state, GtkApplication *app);
void ui_add_danmu(struct app_state *state, const struct danmu_item *item);
void ui_restore_danmu(struct app_state *state, const struct danmu_item *item);
void ui_clear_danmu(struct app_state *state);
void ui_on_avatar_updated(struct app_state *state, const char *uid,
			  const char *avatar_path, const char *real_uname);
bool ui_set_keep_above(struct app_state *state, bool enable);
bool ui_set_mouse_interactive(struct app_state *state, bool enable);
void ui_simulate_drag(struct app_state *state, int delta_x, int delta_y,
		      int steps);

/* 皮肤主题管理相关函数声明 */
const char *ui_skin_name(enum danmu_skin skin);
const char *ui_skin_desc(enum danmu_skin skin);
const char *ui_skin_icon_path(enum danmu_skin skin);
enum danmu_skin ui_parse_skin(const char *name);
bool ui_set_skin(struct app_state *state, enum danmu_skin skin);

#endif /* UI_H */
