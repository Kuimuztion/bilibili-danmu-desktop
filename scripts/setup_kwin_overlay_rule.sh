#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 kuimuztion
#
# 配置 KWin 窗口规则，使 Bilibili 桌面弹幕窗口跨越全屏层始终置顶 (OSD 层)
# 适配 KDE Plasma 6 与 Plasma 5 环境，通用适配任意用户的桌面

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KWINRULES="${HOME}/.config/kwinrulesrc"
RULE_GROUP="Bilibili Danmu Overlay"

echo "=== 配置 Bilibili 桌面弹幕全屏防遮挡规则 (KWin OSD Layer) ==="

# 自动探测 KDE 配置工具 (优先 Plasma 6，回退 Plasma 5)
if command -v kwriteconfig6 >/dev/null 2>&1; then
        KWRITE="kwriteconfig6"
        KREAD="kreadconfig6"
elif command -v kwriteconfig5 >/dev/null 2>&1; then
        KWRITE="kwriteconfig5"
        KREAD="kreadconfig5"
else
        echo "错误: 未找到 kwriteconfig6 或 kwriteconfig5 工具。" >&2
        echo "请确认当前处于 KDE Plasma 桌面环境。" >&2
        exit 1
fi

# 若配置文件存在，创建带时间戳的备份
mkdir -p "${HOME}/.config"
if [ -f "${KWINRULES}" ]; then
        BACKUP="${KWINRULES}.bak.$(date +%s)"
        cp "${KWINRULES}" "${BACKUP}"
        echo "已备份原配置至: ${BACKUP}"
fi

# 写入针对本弹幕程序的专有规则 (绑定至最高优先级 OSD 浮动层)
"${KWRITE}" --file kwinrulesrc --group "${RULE_GROUP}" \
        --key Description \
        "Bilibili Danmu Desktop Overlay Above Fullscreen"
"${KWRITE}" --file kwinrulesrc --group "${RULE_GROUP}" \
        --key wmclass "com.kuimuztion.bilibili_danmu_desktop"
"${KWRITE}" --file kwinrulesrc --group "${RULE_GROUP}" \
        --key wmclassmatch 1
"${KWRITE}" --file kwinrulesrc --group "${RULE_GROUP}" \
        --key type 16
"${KWRITE}" --file kwinrulesrc --group "${RULE_GROUP}" \
        --key typerule 2
"${KWRITE}" --file kwinrulesrc --group "${RULE_GROUP}" \
        --key above true
"${KWRITE}" --file kwinrulesrc --group "${RULE_GROUP}" \
        --key aboverule 2

# 维护 [General] 索引表
CURRENT_RULES=$("${KREAD}" --file kwinrulesrc \
        --group General --key rules || true)
if [ -z "${CURRENT_RULES}" ]; then
        "${KWRITE}" --file kwinrulesrc --group General \
                --key rules "${RULE_GROUP}"
        "${KWRITE}" --file kwinrulesrc --group General \
                --key count 1
else
        if ! echo "${CURRENT_RULES}" | grep -q "${RULE_GROUP}"; then
                NEW_RULES="${CURRENT_RULES},${RULE_GROUP}"
                COUNT=$(echo "${NEW_RULES}" | tr ',' '\n' | \
                        grep -v '^$' | wc -l)
                "${KWRITE}" --file kwinrulesrc --group General \
                        --key rules "${NEW_RULES}"
                "${KWRITE}" --file kwinrulesrc --group General \
                        --key count "${COUNT}"
        fi
fi

# 通知 KWin 立即重载配置并生效 (兼容 busctl 与 qdbus)
echo "正在通知 KWin 重新加载窗口规则..."
if command -v busctl >/dev/null 2>&1; then
        busctl --user call org.kde.KWin /KWin \
                org.kde.KWin reconfigure >/dev/null 2>&1 || true
elif command -v qdbus >/dev/null 2>&1; then
        qdbus org.kde.KWin /KWin reconfigure >/dev/null 2>&1 || true
fi

echo "配置成功！弹幕浮窗已绑定至 KDE OnScreenDisplay 最高层级。"
echo "无论游戏全屏、浏览器 F11 还是视频播放器全屏，弹幕浮窗均保持在最前层。"
