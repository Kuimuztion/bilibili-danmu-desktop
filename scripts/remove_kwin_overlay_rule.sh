#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 kuimuztion
#
# 移除 KWin 窗口规则，还原 Bilibili 桌面弹幕窗口为标准窗口模式
# 适配 KDE Plasma 6 与 Plasma 5 环境，通用适配任意用户的桌面

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KWINRULES="${HOME}/.config/kwinrulesrc"
RULE_GROUP="Bilibili Danmu Overlay"

echo "=== 正在移除 Bilibili 桌面弹幕全屏防遮挡规则 ==="

if [ ! -f "${KWINRULES}" ]; then
        echo "未检测到 kwinrulesrc，无需清理。"
        exit 0
fi

# 自动探测 KDE 配置工具 (优先 Plasma 6，回退 Plasma 5)
if command -v kwriteconfig6 >/dev/null 2>&1; then
        KWRITE="kwriteconfig6"
        KREAD="kreadconfig6"
elif command -v kwriteconfig5 >/dev/null 2>&1; then
        KWRITE="kwriteconfig5"
        KREAD="kreadconfig5"
else
        echo "错误: 未找到 kwriteconfig6 或 kwriteconfig5 工具。" >&2
        exit 1
fi

# 备份原配置
BACKUP="${KWINRULES}.bak.$(date +%s)"
cp "${KWINRULES}" "${BACKUP}"
echo "已备份原配置至: ${BACKUP}"

# 删除规则组中的设置项并还原 [General] 列表
for key in Description wmclass wmclassmatch type typerule above aboverule; do
        "${KWRITE}" --file kwinrulesrc --group "${RULE_GROUP}" \
                --key "${key}" --delete >/dev/null 2>&1 || true
done

CURRENT_RULES=$("${KREAD}" --file kwinrulesrc \
        --group General --key rules || true)
if echo "${CURRENT_RULES}" | grep -q "${RULE_GROUP}"; then
        NEW_RULES=$(echo "${CURRENT_RULES}" | tr ',' '\n' | \
                grep -v "^${RULE_GROUP}$" | paste -sd, -)
        COUNT=$(echo "${NEW_RULES}" | tr ',' '\n' | \
                grep -v '^$' | wc -l)
        "${KWRITE}" --file kwinrulesrc --group General \
                --key rules "${NEW_RULES}"
        "${KWRITE}" --file kwinrulesrc --group General \
                --key count "${COUNT}"
fi

# 通知 KWin 重载配置 (兼容 busctl 与 qdbus)
echo "正在通知 KWin 重新加载窗口规则..."
if command -v busctl >/dev/null 2>&1; then
        busctl --user call org.kde.KWin /KWin \
                org.kde.KWin reconfigure >/dev/null 2>&1 || true
elif command -v qdbus >/dev/null 2>&1; then
        qdbus org.kde.KWin /KWin reconfigure >/dev/null 2>&1 || true
fi

echo "已成功移除全屏 OSD 规则，恢复默认窗口行为。"
