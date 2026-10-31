/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Copyright (C) 2026 Kuimuztion
 *
 * 通用字符串缓冲、自包含轻量 JSON 解析与 MD5 算法工具集实现
 */

#define _POSIX_C_SOURCE 200809L

#include "util.h"
#include "app_state.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * 预留追加缓冲区容量
 */
/**
 * @brief 确保 str_buf 至少拥有追加指定字节所需的容量，并处理容量溢出。
 * @param sb 调用者拥有的 str_buf；本函数可修改其 data/len/cap，但不接管结构体所有权。
 * @param extra 需要额外预留的有效负载字节数；函数会额外为结尾 NUL 保留 1 字节。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
static bool sb_reserve(struct str_buf *sb, size_t extra)
{
	size_t next;
	/* grown 是 realloc 返回的新地址；只有成功后才覆盖原 data 指针。 */
	char *grown;

	if (sb->len + extra + 1 <= sb->cap)
		return true;

	next = sb->cap ? sb->cap : 256;
	while (next < sb->len + extra + 1)
		next *= 2;

	grown = realloc(sb->data, next);
	if (!grown)
		return false;

	sb->data = grown;
	sb->cap = next;
	return true;
}

/*
 * 初始化字符串缓冲区
 */
/**
 * @brief 初始化字符串缓冲区的指针、长度和初始容量。
 * @param sb 调用者拥有的 str_buf；本函数可修改其 data/len/cap，但不接管结构体所有权。
 * @param initial 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
void sb_init(struct str_buf *sb, size_t initial)
{
	if (!sb)
		return;

	memset(sb, 0, sizeof(*sb));
	if (initial < 32)
		initial = 32;

	sb->data = malloc(initial);
	if (sb->data) {
		sb->cap = initial;
		sb->data[0] = '\0';
	}
}

/*
 * 释放字符串缓冲区
 */
/**
 * @brief 释放字符串缓冲区拥有的堆内存并将结构重置为空状态。
 * @param sb 调用者拥有的 str_buf；本函数可修改其 data/len/cap，但不接管结构体所有权。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
void sb_free(struct str_buf *sb)
{
	if (!sb)
		return;

	free(sb->data);
	memset(sb, 0, sizeof(*sb));
}

/*
 * 追加指定长度的子字符串
 */
/**
 * @brief 向动态字符串缓冲追加指定长度的字节序列。
 * @param sb 调用者拥有的 str_buf；本函数可修改其 data/len/cap，但不接管结构体所有权。
 * @param text 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param len 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
bool sb_append_n(struct str_buf *sb, const char *text, size_t len)
{
	if (!sb || !text)
		return false;

	if (!sb_reserve(sb, len))
		return false;

	memcpy(sb->data + sb->len, text, len);
	sb->len += len;
	sb->data[sb->len] = '\0';
	return true;
}

/*
 * 追加以 NULL 结尾的字符串
 */
/**
 * @brief 向动态字符串缓冲追加 NUL 结尾字符串。
 * @param sb 调用者拥有的 str_buf；本函数可修改其 data/len/cap，但不接管结构体所有权。
 * @param text 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
bool sb_append(struct str_buf *sb, const char *text)
{
	if (!text)
		return false;

	return sb_append_n(sb, text, strlen(text));
}

/*
 * 格式化输出追加到缓冲区
 */
/**
 * @brief 使用 printf 格式规则将格式化内容追加到动态字符串缓冲。
 * @param sb 调用者拥有的 str_buf；本函数可修改其 data/len/cap，但不接管结构体所有权。
 * @param fmt 只读借用指针；本函数不修改或释放所指向的对象。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
bool sb_printf(struct str_buf *sb, const char *fmt, ...)
{
	va_list args;
	va_list copy;
	int needed;

	if (!sb || !fmt)
		return false;

	va_start(args, fmt);
	va_copy(copy, args);
	needed = vsnprintf(NULL, 0, fmt, copy);
	va_end(copy);

	if (needed < 0 || !sb_reserve(sb, (size_t)needed)) {
		va_end(args);
		return false;
	}

	vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, args);
	va_end(args);
	sb->len += (size_t)needed;
	return true;
}

/*
 * 追加单个 JSON 字符（对特殊控制字符进行转义）
 */
/**
 * @brief 按 JSON 字符串规则转义单个输入字节并追加到缓冲区。
 * @param sb 调用者拥有的 str_buf；本函数可修改其 data/len/cap，但不接管结构体所有权。
 * @param c 当前处理的单个无符号字符字节；用于 JSON 转义时按其 ASCII/UTF-8 字节值判断。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
static bool sb_append_escaped_char(struct str_buf *sb, unsigned char c)
{
	switch (c) {
	case '\"':
		return sb_append(sb, "\\\"");
	case '\\':
		return sb_append(sb, "\\\\");
	case '\b':
		return sb_append(sb, "\\b");
	case '\f':
		return sb_append(sb, "\\f");
	case '\n':
		return sb_append(sb, "\\n");
	case '\r':
		return sb_append(sb, "\\r");
	case '\t':
		return sb_append(sb, "\\t");
	default:
		if (c < 0x20)
			return sb_printf(sb, "\\u%04x", c);
		return sb_append_n(sb, (const char *)&c, 1);
	}
}

/*
 * 对普通字符串进行 JSON 转义并追加
 */
/**
 * @brief 将普通字符串编码为带双引号和必要转义的 JSON 字符串。
 * @param sb 调用者拥有的 str_buf；本函数可修改其 data/len/cap，但不接管结构体所有权。
 * @param text 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
bool sb_json_string(struct str_buf *sb, const char *text)
{
	const unsigned char *p;

	if (!sb || !text)
		return false;

	if (!sb_append_n(sb, "\"", 1))
		return false;

	for (p = (const unsigned char *)text; *p; p++) {
		if (!sb_append_escaped_char(sb, *p))
			return false;
	}

	return sb_append_n(sb, "\"", 1);
}

/*
 * 将 Unicode 码点编码为 UTF-8 字节序列
 */
/**
 * @brief 将 Unicode 码点编码为 UTF-8 字节并追加到字符串缓冲。
 * @param sb 调用者拥有的 str_buf；本函数可修改其 data/len/cap，但不接管结构体所有权。
 * @param cp 待编码的 Unicode 码点（Unicode scalar value），由本函数转换为 1–4 个 UTF-8 字节。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
static void utf8_codepoint(struct str_buf *sb, uint32_t cp)
{
	char bytes[4];
	size_t n = 0;

	if (cp <= 0x7F) {
		bytes[n++] = (char)cp;
	} else if (cp <= 0x7FF) {
		bytes[n++] = (char)(0xC0 | (cp >> 6));
		bytes[n++] = (char)(0x80 | (cp & 0x3F));
	} else if (cp <= 0xFFFF) {
		bytes[n++] = (char)(0xE0 | (cp >> 12));
		bytes[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		bytes[n++] = (char)(0x80 | (cp & 0x3F));
	} else {
		bytes[n++] = (char)(0xF0 | (cp >> 18));
		bytes[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
		bytes[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		bytes[n++] = (char)(0x80 | (cp & 0x3F));
	}
	sb_append_n(sb, bytes, n);
}

/*
 * 解析 4 位十六进制数
 */
/**
 * @brief 把四位十六进制字符解析成整数，非法字符返回错误值。
 * @param p 只读借用指针；本函数不修改或释放所指向的对象。
 * @return 返回条目数量、解析结果或状态码，具体语义见函数说明。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
static int hex4(const char *p)
{
	int value = 0;
	int i;

	for (i = 0; i < 4; i++) {
		unsigned char c = (unsigned char)p[i];

		if (c >= '0' && c <= '9')
			value = value * 16 + (c - '0');
		else if (c >= 'a' && c <= 'f')
			value = value * 16 + (c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			value = value * 16 + (c - 'A' + 10);
		else
			return -1;
	}
	return value;
}

/*
 * 解析 \uXXXX 格式的 Unicode 转义序列
 */
/**
 * @brief 解析 JSON 的 \uXXXX 转义，并处理代理项组合。
 * @param token 只读借用指针；本函数不修改或释放所指向的对象。
 * @param i 当前扫描的数组/缓冲区元素索引，从 0 开始。
 * @param end 扫描边界位置的只读指针；指向原始 JSON/文本缓冲区内部，不拥有该内存。
 * @param cp_out 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @param consumed_out 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @return 返回条目数量、解析结果或状态码，具体语义见函数说明。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
static int parse_unicode_escape(const char *token, size_t i, size_t end,
				uint32_t *cp_out, size_t *consumed_out)
{
	int first;
	uint32_t cp;

	if (i + 4 > end)
		return -1;

	first = hex4(token + i);
	if (first < 0)
		return -1;

	i += 4;
	cp = (uint32_t)first;

	/* 处理 UTF-16 代理对 */
	if (first >= 0xD800 && first <= 0xDBFF && i + 6 <= end &&
	    token[i] == '\\' && token[i + 1] == 'u') {
		int second = hex4(token + i + 2);

		if (second >= 0xDC00 && second <= 0xDFFF) {
			uint32_t high = ((uint32_t)first - 0xD800u) << 10;
			uint32_t low = (uint32_t)second - 0xDC00u;

			cp = 0x10000u + high + low;
			i += 6;
		}
	}

	*cp_out = cp;
	*consumed_out = i;
	return 0;
}

/*
 * 追加普通 JSON 反转义字符到缓冲区
 */
/**
 * @brief 将解码后的单个字节追加到字符串缓冲并保持 UTF-8 解码状态。
 * @param sb 调用者拥有的 str_buf；本函数可修改其 data/len/cap，但不接管结构体所有权。
 * @param c 当前处理的单个无符号字符字节；用于 JSON 转义时按其 ASCII/UTF-8 字节值判断。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
static bool sb_append_unescaped_char(struct str_buf *sb, unsigned char c)
{
	switch (c) {
	case '\"':
	case '\\':
	case '/':
		return sb_append_n(sb, (const char *)&c, 1);
	case 'b':
		return sb_append_n(sb, "\b", 1);
	case 'f':
		return sb_append_n(sb, "\f", 1);
	case 'n':
		return sb_append_n(sb, "\n", 1);
	case 'r':
		return sb_append_n(sb, "\r", 1);
	case 't':
		return sb_append_n(sb, "\t", 1);
	default:
		return sb_append_n(sb, (const char *)&c, 1);
	}
}

/*
 * 解析 JSON 字符串 Token 并进行反转义
 */
/**
 * @brief 解码指定 JSON 字符串 token 到调用者提供的输出缓冲区。
 * @param token 只读借用指针；本函数不修改或释放所指向的对象。
 * @param length 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @param out 调用者拥有的可写输出缓冲区；本函数只写入有界结果，不释放该缓冲区。
 * @param out_size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
bool json_token_string(const char *token, size_t length, char *out,
		       size_t out_size)
{
	struct str_buf sb;
	size_t i = 1;
	size_t end;
	size_t copy;
	uint32_t cp;
	size_t next_i;
	bool ret = false;

	if (!token || length < 2 || token[0] != '\"' || !out || out_size == 0)
		return false;

	sb_init(&sb, length + 1);
	if (!sb.data)
		return false;

	end = length;
	if (token[length - 1] == '\"')
		end = length - 1;

	while (i < end) {
		unsigned char c = (unsigned char)token[i++];

		if (c != '\\') {
			/* 非转义普通字符直接追加，失败跳转清理 */
			if (!sb_append_n(&sb, (const char *)&c, 1))
				goto out_free_buf;
			continue;
		}

		if (i >= end)
			break;

		c = (unsigned char)token[i++];
		if (c == 'u') {
			if (parse_unicode_escape(token, i, end, &cp,
						 &next_i) == 0) {
				i = next_i;
				utf8_codepoint(&sb, cp);
			}
			continue;
		}

		/* 处理普通转义字符追加，失败跳转清理 */
		if (!sb_append_unescaped_char(&sb, c))
			goto out_free_buf;
	}

	copy = sb.len < out_size - 1 ? sb.len : out_size - 1;
	memcpy(out, sb.data, copy);
	out[copy] = '\0';
	ret = true;

/* 统一清理出口：释放动态字符串缓冲区 */
out_free_buf:
	sb_free(&sb);
	return ret;
}

/*
 * 在 JSON 文本中查找指定字段的值位置
 */
/**
 * @brief 在 JSON 文本中寻找指定键并返回其值 token 的借用指针。
 * @param json NUL 结尾的只读 JSON 文本；本函数不拥有其内存，解析 token 指针不得逃逸其生命周期。
 * @param key 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return 返回输入缓冲区内的借用 token 指针；不得单独 free，且不能超过原 JSON 生命周期。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
const char *json_find_value(const char *json, const char *key)
{
	char pattern[128];
	const char *p;
	int ret;

	if (!json || !key)
		return NULL;

	ret = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	if (ret < 0 || (size_t)ret >= sizeof(pattern))
		return NULL;

	p = json;
	while ((p = strstr(p, pattern)) != NULL) {
		p += strlen(pattern);
		while (isspace((unsigned char)*p))
			p++;

		if (*p++ != ':')
			continue;

		while (isspace((unsigned char)*p))
			p++;

		return p;
	}

	return NULL;
}

/*
 * 跳过 JSON 字符串并返回闭合引号之后的位置，格式错误返回 NULL
 */
/**
 * @brief 跳过一个完整 JSON 字符串 token，返回其后的字符位置。
 * @param p 只读借用指针；本函数不修改或释放所指向的对象。
 * @return 返回输入缓冲区内的借用 token 指针；不得单独 free，且不能超过原 JSON 生命周期。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
static const char *skip_json_string(const char *p)
{
	const char *q;
	bool escape = false;

	if (!p || *p != '\"')
		return NULL;

	for (q = p + 1; *q; q++) {
		if (escape) {
			escape = false;
			continue;
		}

		if (*q == '\\') {
			escape = true;
			continue;
		}

		if (*q == '\"')
			return q + 1;
	}

	return NULL;
}

/*
 * 跳过复合 JSON 结构（对象 {} 或数组 []），返回闭合字符下一位置
 */
/**
 * @brief 跳过嵌套 JSON 对象或数组，返回闭合括号之后的位置。
 * @param p 只读借用指针；本函数不修改或释放所指向的对象。
 * @param open 要匹配的复合结构起始字符，通常为左花括号 '{' 或左方括号 '['。
 * @param close 与 open 配对的结束字符，通常为右花括号 '}' 或右方括号 ']'。
 * @return 返回输入缓冲区内的借用 token 指针；不得单独 free，且不能超过原 JSON 生命周期。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
static const char *skip_json_composite(const char *p, char open, char close)
{
	const char *q;
	int depth = 0;

	if (!p || *p != open)
		return NULL;

	q = p;
	while (*q) {
		if (*q == '\"') {
			q = skip_json_string(q);
			if (!q)
				return NULL;
			continue;
		}

		if (*q == open)
			depth++;
		else if (*q == close && --depth == 0)
			return q + 1;

		q++;
	}

	return NULL;
}

/*
 * 探测单个 JSON 值的字节长度
 */
/**
 * @brief 计算 JSON 值 token 的字节长度，不复制或拥有输入内存。
 * @param p 只读借用指针；本函数不修改或释放所指向的对象。
 * @return 返回处理或消耗的字节数。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
static size_t json_value_length(const char *p)
{
	const char *end;
	const char *q;

	if (!p)
		return 0;

	if (*p == '\"') {
		end = skip_json_string(p);
		return end ? (size_t)(end - p) : 0;
	}

	if (*p == '[' || *p == '{') {
		char close = (*p == '[') ? ']' : '}';

		end = skip_json_composite(p, *p, close);
		return end ? (size_t)(end - p) : 0;
	}

	for (q = p; *q && *q != ',' && *q != '}' && *q != ']' &&
	     !isspace((unsigned char)*q); q++)
		;

	return (size_t)(q - p);
}

/*
 * 只在当前 JSON 对象的直接成员中查找字段。
 * 与 json_find_value() 的全树搜索不同，此函数不会误取子对象字段。
 */
/**
 * @brief 只在指定 JSON 对象的直接层级查找成员，返回输入文本中的借用指针。
 * @param json NUL 结尾的只读 JSON 文本；本函数不拥有其内存，解析 token 指针不得逃逸其生命周期。
 * @param key 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @return 返回输入缓冲区内的借用 token 指针；不得单独 free，且不能超过原 JSON 生命周期。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
const char *json_object_find_value(const char *json, const char *key)
{
	const char *p;
	char key_buf[128];

	if (!json || !key || *json != '{')
		return NULL;

	p = json + 1;
	while (*p) {
		size_t key_len;
		size_t value_len;
		const char *value;

		while (isspace((unsigned char)*p) || *p == ',')
			p++;

		if (*p == '}' || *p == '\0')
			return NULL;

		key_len = json_value_length(p);
		if (!key_len || *p != '\"' ||
		    !json_token_string(p, key_len, key_buf,
				       sizeof(key_buf)))
			return NULL;

		p += key_len;
		while (isspace((unsigned char)*p))
			p++;
		if (*p != ':')
			return NULL;
		p++;
		while (isspace((unsigned char)*p))
			p++;

		value = p;
		value_len = json_value_length(value);
		if (!value_len)
			return NULL;

		if (strcmp(key_buf, key) == 0)
			return value;

		p = value + value_len;
		while (isspace((unsigned char)*p))
			p++;
		if (*p == '}')
			return NULL;
		if (*p != ',')
			return NULL;
		p++;
	}

	return NULL;
}

/*
 * 从 JSON 对象的直接成员中提取字符串字段。
 */
/**
 * @brief 读取对象直接成员字符串并写入调用者的 NUL 结尾缓冲区。
 * @param json NUL 结尾的只读 JSON 文本；本函数不拥有其内存，解析 token 指针不得逃逸其生命周期。
 * @param key 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param out 调用者拥有的可写输出缓冲区；本函数只写入有界结果，不释放该缓冲区。
 * @param out_size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
bool json_object_get_string(const char *json, const char *key, char *out,
			    size_t out_size)
{
	const char *value = json_object_find_value(json, key);
	size_t len = json_value_length(value);

	return json_token_string(value, len, out, out_size);
}

/*
 * 从 JSON 对象的直接成员中提取整数。
 */
/**
 * @brief 读取对象直接成员整数；字段缺失或无法解析时返回 fallback。
 * @param json NUL 结尾的只读 JSON 文本；本函数不拥有其内存，解析 token 指针不得逃逸其生命周期。
 * @param key 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param fallback 字段不存在、类型不符或解析失败时返回的调用方指定默认值。
 * @return 返回解析出的整数，缺失/解析失败时使用 fallback。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
long long json_object_get_integer(const char *json, const char *key,
				  long long fallback)
{
	const char *value = json_object_find_value(json, key);
	char *end = NULL;
	long long result;

	char number[64];

	if (!value)
		return fallback;

	if (*value == '\"') {
		size_t len = json_value_length(value);

		if (!json_token_string(value, len, number, sizeof(number)))
			return fallback;
		value = number;
	}

	result = strtoll(value, &end, 10);
	return end == value ? fallback : result;
}

/*
 * 从 JSON 对象中提取字符串字段
 */
/**
 * @brief 从 JSON 文本读取字符串字段并写入调用者输出缓冲区。
 * @param json NUL 结尾的只读 JSON 文本；本函数不拥有其内存，解析 token 指针不得逃逸其生命周期。
 * @param key 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param out 调用者拥有的可写输出缓冲区；本函数只写入有界结果，不释放该缓冲区。
 * @param out_size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
bool json_get_string(const char *json, const char *key, char *out,
		     size_t out_size)
{
	const char *value = json_find_value(json, key);
	size_t len = json_value_length(value);

	return json_token_string(value, len, out, out_size);
}

/*
 * 从 JSON 对象中提取整数字段
 */
/**
 * @brief 从 JSON 文本读取整数值，无法解析时返回调用者提供的默认值。
 * @param json NUL 结尾的只读 JSON 文本；本函数不拥有其内存，解析 token 指针不得逃逸其生命周期。
 * @param key 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param fallback 字段不存在、类型不符或解析失败时返回的调用方指定默认值。
 * @return 返回解析出的整数，缺失/解析失败时使用 fallback。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
long long json_get_integer(const char *json, const char *key,
			   long long fallback)
{
	const char *value = json_find_value(json, key);
	char *end = NULL;
	long long result;

	if (!value)
		return fallback;

	result = strtoll(value, &end, 10);
	return (end == value) ? fallback : result;
}

/*
 * 从 JSON 对象中提取布尔字段
 */
/**
 * @brief 从 JSON 文本读取布尔字段，缺失时返回默认值。
 * @param json NUL 结尾的只读 JSON 文本；本函数不拥有其内存，解析 token 指针不得逃逸其生命周期。
 * @param key 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param fallback 字段不存在、类型不符或解析失败时返回的调用方指定默认值。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
bool json_get_boolean(const char *json, const char *key, bool fallback)
{
	const char *value = json_find_value(json, key);

	if (!value)
		return fallback;

	if (strncmp(value, "true", 4) == 0)
		return true;
	if (strncmp(value, "false", 5) == 0)
		return false;

	return fallback;
}

/*
 * 提取 JSON 数组中指定索引的元素切片
 */
/**
 * @brief 定位数组中指定索引元素，输出指向输入 JSON 的 token 指针和长度。
 * @param array 只读借用指针；本函数不修改或释放所指向的对象。
 * @param index 目标数组元素的从 0 开始索引；负数或越界索引会被视为读取失败。
 * @param start 输出参数：接收指向 array 输入文本内部 token 起点的借用指针，不拥有该内存。
 * @param length 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
bool json_array_element(const char *array, int index, const char **start,
			size_t *length)
{
	const char *p;
	int current = 0;

	if (!array || *array != '[' || index < 0)
		return false;

	p = array + 1;
	while (*p) {
		size_t len;

		while (isspace((unsigned char)*p) || *p == ',')
			p++;

		if (*p == ']')
			return false;

		len = json_value_length(p);
		if (!len)
			return false;

		if (current == index) {
			*start = p;
			*length = len;
			return true;
		}

		p += len;
		current++;
	}

	return false;
}

/*
 * 读取完整文件到内存
 */
/**
 * @brief 读取完整文件到新分配的 NUL 结尾堆缓冲区，调用者负责 free。
 * @param path 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param size_out 调用者提供的可写指针；所指存储由调用者管理，本函数按函数契约读取或写入。
 * @return 成功时返回新分配的字符串，所有权交给调用者并由调用者 free；失败返回 NULL。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
char *read_entire_file(const char *path, size_t *size_out)
{
	FILE *file;
	long size;
	char *data;
	size_t read_bytes;

	if (!path)
		return NULL;

	file = fopen(path, "rb");
	if (!file)
		return NULL;

	/* 探测文件大小失败，关闭文件退出 */
	if (fseek(file, 0, SEEK_END) != 0)
		goto err_close_file;

	size = ftell(file);
	/* 获取文件偏移量失败，关闭文件退出 */
	if (size < 0)
		goto err_close_file;

	/* 重置文件指针到开头失败，关闭文件退出 */
	if (fseek(file, 0, SEEK_SET) != 0)
		goto err_close_file;

	data = malloc((size_t)size + 1);
	/* 内存分配失败，关闭文件退出 */
	if (!data)
		goto err_close_file;

	read_bytes = fread(data, 1, (size_t)size, file);
	data[read_bytes] = '\0';

	if (size_out)
		*size_out = read_bytes;

	fclose(file);
	return data;

/* 错误分层清理：关闭已打开的文件句柄 */
err_close_file:
	fclose(file);
	return NULL;
}

/*
 * 将内存数据写入文件
 */
/**
 * @brief 将指定字节长度完整写入文件并检查写入/关闭结果。
 * @param path 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param data 指向输入 JSON 对象或文本的只读借用指针，不得释放或保存到调用之后。
 * @param length 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
bool write_entire_file(const char *path, const char *data, size_t length)
{
	FILE *file;
	size_t written;

	if (!path || !data)
		return false;

	file = fopen(path, "wb");
	if (!file)
		return false;

	written = fwrite(data, 1, length, file);
	fclose(file);

	return written == length;
}

/* =========================================================================
 * RFC 1321 MD5 哈希算法实现
 * ========================================================================= */

struct md5_context {
	uint32_t state[4];  /* A, B, C, D 链接变量 */
	uint32_t count[2];  /* 消息比特计数器 (低32位, 高32位) */
	uint8_t buffer[64]; /* 512 位输入缓冲区 */
};

#define MD5_F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | (~z)))

#define MD5_ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define MD5_FF(a, b, c, d, x, s, ac) do { \
	(a) += MD5_F((b), (c), (d)) + (x) + (uint32_t)(ac); \
	(a) = MD5_ROTATE_LEFT((a), (s)); \
	(a) += (b); \
} while (0)

#define MD5_GG(a, b, c, d, x, s, ac) do { \
	(a) += MD5_G((b), (c), (d)) + (x) + (uint32_t)(ac); \
	(a) = MD5_ROTATE_LEFT((a), (s)); \
	(a) += (b); \
} while (0)

#define MD5_HH(a, b, c, d, x, s, ac) do { \
	(a) += MD5_H((b), (c), (d)) + (x) + (uint32_t)(ac); \
	(a) = MD5_ROTATE_LEFT((a), (s)); \
	(a) += (b); \
} while (0)

#define MD5_II(a, b, c, d, x, s, ac) do { \
	(a) += MD5_I((b), (c), (d)) + (x) + (uint32_t)(ac); \
	(a) = MD5_ROTATE_LEFT((a), (s)); \
	(a) += (b); \
} while (0)

/*
 * 单个 64 字节块的核心 64 轮压缩变换
 */
/**
 * @brief 对 MD5 状态执行一个 512 位消息块的压缩变换。
 * @param state MD5 四个 32 位工作寄存器组成的状态数组；本函数会原位更新这四个寄存器。
 * @param block MD5 压缩函数输入的单个 64 字节消息块；本函数只读该缓冲区。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
static void md5_transform_block(uint32_t state[4], const uint8_t block[64])
{
	uint32_t a = state[0];
	uint32_t b = state[1];
	uint32_t c = state[2];
	uint32_t d = state[3];
	uint32_t x[16];
	int i;
	int j;

	for (i = 0, j = 0; i < 16; i++, j += 4) {
		x[i] = ((uint32_t)block[j]) |
		       (((uint32_t)block[j + 1]) << 8) |
		       (((uint32_t)block[j + 2]) << 16) |
		       (((uint32_t)block[j + 3]) << 24);
	}

	/* 第一轮 */
	MD5_FF(a, b, c, d, x[0], 7, 0xd76aa478);
	MD5_FF(d, a, b, c, x[1], 12, 0xe8c7b756);
	MD5_FF(c, d, a, b, x[2], 17, 0x242070db);
	MD5_FF(b, c, d, a, x[3], 22, 0xc1bdceee);
	MD5_FF(a, b, c, d, x[4], 7, 0xf57c0faf);
	MD5_FF(d, a, b, c, x[5], 12, 0x4787c62a);
	MD5_FF(c, d, a, b, x[6], 17, 0xa8304613);
	MD5_FF(b, c, d, a, x[7], 22, 0xfd469501);
	MD5_FF(a, b, c, d, x[8], 7, 0x698098d8);
	MD5_FF(d, a, b, c, x[9], 12, 0x8b44f7af);
	MD5_FF(c, d, a, b, x[10], 17, 0xffff5bb1);
	MD5_FF(b, c, d, a, x[11], 22, 0x895cd7be);
	MD5_FF(a, b, c, d, x[12], 7, 0x6b901122);
	MD5_FF(d, a, b, c, x[13], 12, 0xfd987193);
	MD5_FF(c, d, a, b, x[14], 17, 0xa679438e);
	MD5_FF(b, c, d, a, x[15], 22, 0x49b40821);

	/* 第二轮 */
	MD5_GG(a, b, c, d, x[1], 5, 0xf61e2562);
	MD5_GG(d, a, b, c, x[6], 9, 0xc040b340);
	MD5_GG(c, d, a, b, x[11], 14, 0x265e5a51);
	MD5_GG(b, c, d, a, x[0], 20, 0xe9b6c7aa);
	MD5_GG(a, b, c, d, x[5], 5, 0xd62f105d);
	MD5_GG(d, a, b, c, x[10], 9, 0x2441453);
	MD5_GG(c, d, a, b, x[15], 14, 0xd8a1e681);
	MD5_GG(b, c, d, a, x[4], 20, 0xe7d3fbc8);
	MD5_GG(a, b, c, d, x[9], 5, 0x21e1cde6);
	MD5_GG(d, a, b, c, x[14], 9, 0xc33707d6);
	MD5_GG(c, d, a, b, x[3], 14, 0xf4d50d87);
	MD5_GG(b, c, d, a, x[8], 20, 0x455a14ed);
	MD5_GG(a, b, c, d, x[13], 5, 0xa9e3e905);
	MD5_GG(d, a, b, c, x[2], 9, 0xfcefa3f8);
	MD5_GG(c, d, a, b, x[7], 14, 0x676f02d9);
	MD5_GG(b, c, d, a, x[12], 20, 0x8d2a4c8a);

	/* 第三轮 */
	MD5_HH(a, b, c, d, x[5], 4, 0xfffa3942);
	MD5_HH(d, a, b, c, x[8], 11, 0x8771f681);
	MD5_HH(c, d, a, b, x[11], 16, 0x6d9d6122);
	MD5_HH(b, c, d, a, x[14], 23, 0xfde5380c);
	MD5_HH(a, b, c, d, x[1], 4, 0xa4beea44);
	MD5_HH(d, a, b, c, x[4], 11, 0x4bdecfa9);
	MD5_HH(c, d, a, b, x[7], 16, 0xf6bb4b60);
	MD5_HH(b, c, d, a, x[10], 23, 0xbebfbc70);
	MD5_HH(a, b, c, d, x[13], 4, 0x289b7ec6);
	MD5_HH(d, a, b, c, x[0], 11, 0xeaa127fa);
	MD5_HH(c, d, a, b, x[3], 16, 0xd4ef3085);
	MD5_HH(b, c, d, a, x[6], 23, 0x4881d05);
	MD5_HH(a, b, c, d, x[9], 4, 0xd9d4d039);
	MD5_HH(d, a, b, c, x[12], 11, 0xe6db99e5);
	MD5_HH(c, d, a, b, x[15], 16, 0x1fa27cf8);
	MD5_HH(b, c, d, a, x[2], 23, 0xc4ac5665);

	/* 第四轮 */
	MD5_II(a, b, c, d, x[0], 6, 0xf4292244);
	MD5_II(d, a, b, c, x[7], 10, 0x432aff97);
	MD5_II(c, d, a, b, x[14], 15, 0xab9423a7);
	MD5_II(b, c, d, a, x[5], 21, 0xfc93a039);
	MD5_II(a, b, c, d, x[12], 6, 0x655b59c3);
	MD5_II(d, a, b, c, x[3], 10, 0x8f0ccc92);
	MD5_II(c, d, a, b, x[10], 15, 0xffeff47d);
	MD5_II(b, c, d, a, x[1], 21, 0x85845dd1);
	MD5_II(a, b, c, d, x[8], 6, 0x6fa87e4f);
	MD5_II(d, a, b, c, x[15], 10, 0xfe2ce6e0);
	MD5_II(c, d, a, b, x[6], 15, 0xa3014314);
	MD5_II(b, c, d, a, x[13], 21, 0x4e0811a1);
	MD5_II(a, b, c, d, x[4], 6, 0xf7537e82);
	MD5_II(d, a, b, c, x[11], 10, 0xbd3af235);
	MD5_II(c, d, a, b, x[2], 15, 0x2ad7d2bb);
	MD5_II(b, c, d, a, x[9], 21, 0xeb86d391);

	/* 累加状态变量 */
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
}

/*
 * 初始化 MD5 上下文
 */
/**
 * @brief 初始化 MD5 上下文中的初始状态、计数器和缓冲数据。
 * @param ctx 调用者维护的算法上下文；本函数直接更新该上下文的内部状态。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
static void md5_init(struct md5_context *ctx)
{
	ctx->count[0] = 0;
	ctx->count[1] = 0;
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xefcdab89;
	ctx->state[2] = 0x98badcfe;
	ctx->state[3] = 0x10325476;
}

/*
 * 更新 MD5 消息摘要输入
 */
/**
 * @brief 把任意长度输入数据累计进 MD5 上下文并处理完整消息块。
 * @param ctx 调用者维护的算法上下文；本函数直接更新该上下文的内部状态。
 * @param input 调用者拥有的只读输入字节/文本缓冲区；仅在本次调用期间读取。
 * @param input_len input 指向的有效输入数据长度，单位为字节。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
static void md5_update(struct md5_context *ctx, const uint8_t *input,
		       size_t input_len)
{
	size_t i = 0;
	size_t index = (ctx->count[0] >> 3) & 0x3F;
	size_t part_len;

	if ((ctx->count[0] += (uint32_t)(input_len << 3)) <
	    (uint32_t)(input_len << 3))
		ctx->count[1]++;

	ctx->count[1] += (uint32_t)(input_len >> 29);

	part_len = 64 - index;
	if (input_len >= part_len) {
		memcpy(&ctx->buffer[index], input, part_len);
		md5_transform_block(ctx->state, ctx->buffer);

		for (i = part_len; i + 63 < input_len; i += 64)
			md5_transform_block(ctx->state, &input[i]);

		index = 0;
	}
	memcpy(&ctx->buffer[index], &input[i], input_len - i);
}

/*
 * 完成 MD5 计算并输出 16 字节原始摘要
 */
/**
 * @brief 完成 MD5 填充和最终变换，输出 16 字节摘要并清理上下文。
 * @param digest 调用者提供的 16 字节输出数组，用于接收 MD5 原始摘要。
 * @param ctx 调用者维护的算法上下文；本函数直接更新该上下文的内部状态。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
static void md5_final(uint8_t digest[16], struct md5_context *ctx)
{
	static const uint8_t padding[64] = { 0x80 };
	uint8_t bits[8];
	size_t index;
	size_t pad_len;
	int i;

	for (i = 0; i < 4; i++) {
		bits[i] = (uint8_t)((ctx->count[0] >> (i * 8)) & 0xFF);
		bits[i + 4] = (uint8_t)((ctx->count[1] >> (i * 8)) & 0xFF);
	}

	index = (ctx->count[0] >> 3) & 0x3F;
	pad_len = (index < 56) ? (56 - index) : (120 - index);
	md5_update(ctx, padding, pad_len);
	md5_update(ctx, bits, 8);

	for (i = 0; i < 4; i++) {
		digest[i * 4]     = (uint8_t)((ctx->state[i]) & 0xFF);
		digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 8) & 0xFF);
		digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 16) & 0xFF);
		digest[i * 4 + 3] = (uint8_t)((ctx->state[i] >> 24) & 0xFF);
	}
}

/*
 * 计算字符串的 MD5 哈希值，输出 32 位小写十六进制字符串
 */
/**
 * @brief 计算文本的 MD5 摘要并输出 32 位小写十六进制字符串。
 * @param text 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param out 调用者提供的至少 33 字节输出数组；写入 32 位小写十六进制 MD5 字符串及结尾 NUL。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
void md5_hex(const char *text, char out[33])
{
	struct md5_context ctx;
	uint8_t digest[16];
	int i;

	if (!text || !out)
		return;

	md5_init(&ctx);
	md5_update(&ctx, (const uint8_t *)text, strlen(text));
	md5_final(digest, &ctx);

	for (i = 0; i < 16; i++)
		snprintf(out + i * 2, 3, "%02x", digest[i]);

	out[32] = '\0';
}

/*
 * 去除富文本中包含的 HTML 标签并输出纯文本
 */
/**
 * @brief 移除输入文本中的尖括号标签并写入有界输出缓冲区。
 * @param in 只读借用指针；本函数不修改或释放所指向的对象。
 * @param out 调用者拥有的可写输出缓冲区；本函数只写入有界结果，不释放该缓冲区。
 * @param out_size 与对应输入/输出缓冲区关联的长度或容量，单位为字节。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
void util_strip_tags(const char *in, char *out, size_t out_size)
{
	size_t o = 0;
	bool in_tag = false;
	const char *p;

	if (!out || out_size == 0)
		return;

	out[0] = '\0';
	if (!in)
		return;

	for (p = in; *p != '\0'; p++) {
		if (*p == '<') {
			in_tag = true;
			continue;
		}
		if (*p == '>') {
			in_tag = false;
			continue;
		}
		if (in_tag)
			continue;

		if (o + 1 < out_size)
			out[o++] = *p;
	}

	out[o] = '\0';
}

/*
 * 记录客户端新会话启动标记到持久化日志文件
 */
/**
 * @brief 为当前房间建立或追加一次日志会话分隔记录。
 * @param room_id B 站直播间房间号；使用真实房间号而非短链或用户 UID。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
void util_log_session_start(long long room_id)
{
	FILE *fp;
	time_t now;
	struct tm tm_buf;
	char time_str[32];

	fp = fopen(DANMU_LOG_FILE, "a");
	if (!fp)
		return;

	now = time(NULL);
	if (localtime_r(&now, &tm_buf))
		strftime(time_str, sizeof(time_str),
			 "%Y-%m-%d %H:%M:%S", &tm_buf);
	else
		snprintf(time_str, sizeof(time_str), "未知时间");

	fprintf(fp, "======================================================\n");
	fprintf(fp, "[%s] === 会话启动 (直播间房号: %lld) ===\n",
		time_str, room_id);
	fprintf(fp, "======================================================\n");
	fflush(fp);
	fclose(fp);
}

/*
 * 持久化记录单条弹幕信息至本地日志文件
 */
/**
 * @brief 将弹幕时间、昵称、UID 和文本追加到本地历史日志。
 * @param room_id B 站直播间房间号；使用真实房间号而非短链或用户 UID。
 * @param uname 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param uid 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param text 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param timestamp 弹幕发生时间的 Unix 时间戳，单位为秒。
 * @return 无返回值；结果通过参数、共享状态或回调完成。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
void util_log_danmu(long long room_id, const char *uname, const char *uid,
		    const char *text, int64_t timestamp)
{
	FILE *fp;
	time_t t;
	struct tm tm_buf;
	char time_str[32];
	char clean_text[1024];

	fp = fopen(DANMU_LOG_FILE, "a");
	if (!fp)
		return;

	t = (timestamp > 0) ? (time_t)timestamp : time(NULL);
	if (localtime_r(&t, &tm_buf))
		strftime(time_str, sizeof(time_str),
			 "%Y-%m-%d %H:%M:%S", &tm_buf);
	else
		snprintf(time_str, sizeof(time_str), "未知时间");

	util_strip_tags(text ? text : "", clean_text, sizeof(clean_text));

	fprintf(fp, "[%s] [房号: %lld] [用户: %s (UID: %s)] %s\n",
		time_str, room_id,
		uname ? uname : "未知用户",
		uid ? uid : "0",
		clean_text);
	fflush(fp);
	fclose(fp);
}

/*
 * 解析单行日志中的弹幕字段（时间戳、用户昵称、UID与正文）
 */
/**
 * @brief 解析一行历史日志并填充调用者提供的 danmu_item 结构。
 * @param line 只读借用指针；本函数不修改或释放所指向的对象。
 * @param item 弹幕结构体；只读输入时不修改，异步派发前按值复制。
 * @return true 表示操作成功/条件成立；false 表示失败或不满足条件。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
static bool parse_danmu_line(const char *line, struct danmu_item *item)
{
	const char *u_tag = "[用户: ";
	const char *uid_tag = " (UID: ";
	const char *p1;
	const char *u_start;
	const char *uid_start;
	const char *uid_end;
	const char *text_start;
	char time_buf[64];
	size_t t_len;
	size_t u_len;
	size_t uid_len;
	size_t txt_len;
	int year;
	int mon;
	int day;
	int hour;
	int min;
	int sec;
	struct tm tm_buf;

	if (!line || !item || line[0] != '[')
		return false;

	p1 = strchr(line + 1, ']');
	if (!p1)
		return false;

	t_len = (size_t)(p1 - (line + 1));
	if (t_len >= sizeof(time_buf))
		t_len = sizeof(time_buf) - 1;

	memcpy(time_buf, line + 1, t_len);
	time_buf[t_len] = '\0';

	memset(&tm_buf, 0, sizeof(tm_buf));
	if (sscanf(time_buf, "%d-%d-%d %d:%d:%d",
		   &year, &mon, &day, &hour, &min, &sec) == 6) {
		tm_buf.tm_year = year - 1900;
		tm_buf.tm_mon = mon - 1;
		tm_buf.tm_mday = day;
		tm_buf.tm_hour = hour;
		tm_buf.tm_min = min;
		tm_buf.tm_sec = sec;
		tm_buf.tm_isdst = -1;
		item->timestamp = (int64_t)mktime(&tm_buf);
	} else {
		item->timestamp = (int64_t)time(NULL);
	}

	u_start = strstr(p1, u_tag);
	if (!u_start)
		return false;
	u_start += strlen(u_tag);

	uid_start = strstr(u_start, uid_tag);
	if (!uid_start)
		return false;

	u_len = (size_t)(uid_start - u_start);
	if (u_len >= sizeof(item->uname))
		u_len = sizeof(item->uname) - 1;
	memcpy(item->uname, u_start, u_len);
	item->uname[u_len] = '\0';

	uid_start += strlen(uid_tag);
	uid_end = strstr(uid_start, ")]");
	if (!uid_end)
		return false;

	uid_len = (size_t)(uid_end - uid_start);
	if (uid_len >= sizeof(item->uid))
		uid_len = sizeof(item->uid) - 1;
	memcpy(item->uid, uid_start, uid_len);
	item->uid[uid_len] = '\0';

	text_start = uid_end + 2;
	if (*text_start == ' ')
		text_start++;

	txt_len = strlen(text_start);
	if (txt_len >= sizeof(item->text))
		txt_len = sizeof(item->text) - 1;
	memcpy(item->text, text_start, txt_len);
	item->text[txt_len] = '\0';

	item->avatar_path[0] = '\0';
	return true;
}

/*
 * 从本地日志文件加载并还原近期历史弹幕（限制最多 max_items 条）
 */
/**
 * @brief 从日志加载最近 max_items 条有效弹幕至调用者数组。
 * @param path 调用者提供的只读字符串；函数不会修改或释放该字符串。
 * @param items 调用者分配的 danmu_item 数组；本函数在 max_items 范围内填充元素。
 * @param max_items items 输出数组最多可写入的元素数量，也是本次历史恢复的上限。
 * @return 返回条目数量、解析结果或状态码，具体语义见函数说明。
 * @thread 无 GTK 线程亲和要求；调用者需保证传入缓冲区在调用期间有效。
 */
int util_load_history_danmu(const char *path, struct danmu_item *items,
			    int max_items)
{
	FILE *fp;
	char line[2048];
	struct danmu_item *ring;
	int total = 0;
	int count = 0;
	int start = 0;
	int i;

	if (!items || max_items <= 0)
		return 0;

	if (!path)
		path = DANMU_LOG_FILE;

	fp = fopen(path, "r");
	if (!fp)
		return 0;

	ring = calloc((size_t)max_items, sizeof(*ring));
	if (!ring) {
		fclose(fp);
		return 0;
	}

	while (fgets(line, sizeof(line), fp)) {
		size_t len = strlen(line);
		struct danmu_item parsed;

		while (len > 0 && (line[len - 1] == '\n' ||
				   line[len - 1] == '\r'))
			line[--len] = '\0';

		if (len == 0)
			continue;

		memset(&parsed, 0, sizeof(parsed));
		if (parse_danmu_line(line, &parsed)) {
			ring[total % max_items] = parsed;
			total++;
		}
	}

	fclose(fp);

	if (total == 0) {
		free(ring);
		return 0;
	}

	if (total > max_items) {
		count = max_items;
		start = total % max_items;
	} else {
		count = total;
		start = 0;
	}

	for (i = 0; i < count; i++) {
		int idx = (start + i) % max_items;

		items[i] = ring[idx];
	}

	free(ring);
	return count;
}
