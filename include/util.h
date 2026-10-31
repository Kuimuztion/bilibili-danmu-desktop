/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Copyright (C) 2026 Kuimuztion
 *
 * 通用字符串缓冲、自包含轻量 JSON 解析与 MD5 算法工具集
 */

#ifndef UTIL_H
#define UTIL_H

/*
 * 通用工具 API。
 * json_*_find_value/json_array_element 返回的 token 指针借用原始 JSON 内存；
 * read_entire_file 返回的 char* 则是新分配的堆缓冲区，调用者必须 free。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 动态增长的字符串缓冲区
 */
struct str_buf {
	char *data; /* 结构体拥有的堆缓冲；调用 sb_free() 释放。 */
	size_t len;
	size_t cap;
};

void sb_init(struct str_buf *sb, size_t initial);
void sb_free(struct str_buf *sb);
bool sb_append(struct str_buf *sb, const char *text);
bool sb_append_n(struct str_buf *sb, const char *text, size_t len);
bool sb_printf(struct str_buf *sb, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
bool sb_json_string(struct str_buf *sb, const char *text);

const char *json_find_value(const char *json, const char *key);
const char *json_object_find_value(const char *json, const char *key);
bool json_object_get_string(const char *json, const char *key, char *out,
			    size_t out_size);
long long json_object_get_integer(const char *json, const char *key,
				  long long fallback);
bool json_get_string(const char *json, const char *key, char *out,
		     size_t out_size);
long long json_get_integer(const char *json, const char *key,
			   long long fallback);
bool json_get_boolean(const char *json, const char *key, bool fallback);
bool json_array_element(const char *array, int index, const char **start,
			size_t *length);
bool json_token_string(const char *token, size_t length, char *out,
		       size_t out_size);

char *read_entire_file(const char *path, size_t *size_out);
bool write_entire_file(const char *path, const char *data, size_t length);

void md5_hex(const char *text, char out[33]);

#define DANMU_LOG_FILE "/tmp/bilibili_danmu_history.log"

struct danmu_item;

void util_strip_tags(const char *in, char *out, size_t out_size);
void util_log_session_start(long long room_id);
void util_log_danmu(long long room_id, const char *uname, const char *uid,
		    const char *text, int64_t timestamp);
int util_load_history_danmu(const char *path, struct danmu_item *items,
			    int max_items);

#endif /* UTIL_H */
