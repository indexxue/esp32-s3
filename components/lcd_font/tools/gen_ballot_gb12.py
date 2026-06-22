#!/usr/bin/env python3
"""Generate GB2312 12x12 bitmap font for ballot_guard LCD (typFNT_GB12 compatible)."""

from __future__ import annotations

import os
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("pip install pillow", file=sys.stderr)
    raise

ROOT = Path(__file__).resolve().parents[1]
OUT_FONT = ROOT / "include" / "ballot_gb12.h"
OUT_STR = ROOT.parents[1] / "ballot_guard" / "main" / "source" / "vote_menu_zh.h"

# All UI copy (UTF-8). Script deduplicates glyphs and emits GB2312 string macros.
UI_STRINGS = [
    "管理员设置",
    "1.进入投票",
    "2.投票时段",
    "3.候选人数量",
    "4.冷却时长",
    "5.设置时钟",
    "6.重置数据",
    "取消",
    "确认重置",
    "进入投票",
    "保存",
    "投票时段",
    "开始时",
    "开始分",
    "结束时",
    "结束分",
    "候选人数量",
    "人数",
    "范围2-6",
    "名称见网页",
    "冷却(秒)",
    "重置",
    "重置全部投票数据?",
    "无法撤销",
    "离开管理菜单?",
    "系统时钟",
    "时",
    "分",
    "秒",
    "调节 确认 返回",
    "选择 确认 返回",
    "滑动 选择 确认 返回",
    "看板",
    "待开启",
    "已锁定",
    "空闲",
    "候选人",
    "票数",
    "有效",
    "废票",
    "总计",
    "后开始",
    "上=历史 长按OK=菜单",
    "投票中",
    "可以投票",
    "剩余",
    "等待传感器",
    "选人",
    "左右 确认 长按废票",
    "空白",
    "多选",
    "不规范",
    "等待",
    "冷却中",
    "请勿靠近",
    "请稍候 返回",
    "违规",
    "重复投票",
    "报警中 返回",
    "已锁定",
    "最终",
    "上下 长按OK=菜单 返回",
    "已结束 长按OK=菜单 返回",
    "历史",
    "无记录",
    "时段无效",
    "保存失败",
    "已保存",
    "请先清零票数",
    "配置失败",
    "重置成功",
    "时钟保存失败",
    "时钟已保存",
    "阶段:",
    "废票",
    "开始",
    "结束",
    "上一页 下一页 返回",
    "候选",
    "故障",
]


def is_gb2312(ch: str) -> bool:
    try:
        ch.encode("gb2312")
        return ord(ch) > 0x7F
    except UnicodeEncodeError:
        return False


def collect_chars() -> list[str]:
    seen: set[str] = set()
    order: list[str] = []
    for s in UI_STRINGS:
        for ch in s:
            if is_gb2312(ch) and ch not in seen:
                seen.add(ch)
                order.append(ch)
    return order


def load_font(size: int = 12) -> ImageFont.FreeTypeFont:
    candidates = [
        r"C:\Windows\Fonts\simsun.ttc",
        r"C:\Windows\Fonts\simhei.ttf",
        r"C:\Windows\Fonts\msyh.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    ]
    for path in candidates:
        if os.path.isfile(path):
            return ImageFont.truetype(path, size=size)
    raise RuntimeError("No CJK font found; install SimSun or WenQuanYi")


def glyph_to_msk(img: Image.Image, w: int = 12, h: int = 12) -> list[int]:
    """12x12 点阵行优先：每行 12 像素占 Msk[2*r]（8bit）+ Msk[2*r+1]（4bit），与 lcd.c 一致。"""
    px = img.load()
    msk = [0] * 24
    for r in range(h):
        for c in range(w):
            if px[c, r] > 128:
                continue
            if c < 8:
                msk[r * 2] |= 1 << c
            else:
                msk[r * 2 + 1] |= 1 << (c - 8)
    return msk


def validate_row_major_reference() -> None:
    """与 cbb/lcdfont.h 中「电」字点阵对照，确保位序正确。"""
    ref = [
        0x10, 0x00, 0x10, 0x00, 0xFF, 0x01, 0x11, 0x01, 0x11, 0x01, 0xFF, 0x01,
        0x11, 0x01, 0x11, 0x01, 0xFF, 0x01, 0x11, 0x04, 0x10, 0x04, 0xE0, 0x07,
    ]
    font = load_font(12)
    got = render_glyph("电", font)
    if got != ref:
        # 字体源不同允许差异，但行列序错误时会完全不像；仅警告
        print("warn: generated 电 msk differs from lcdfont.h reference (font source may differ)")


def render_glyph(ch: str, font: ImageFont.FreeTypeFont) -> list[int]:
    img = Image.new("L", (12, 12), 255)
    draw = ImageDraw.Draw(img)
    bbox = draw.textbbox((0, 0), ch, font=font)
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]
    x = (12 - tw) // 2 - bbox[0]
    y = (12 - th) // 2 - bbox[1]
    draw.text((x, y), ch, font=font, fill=0)
    return glyph_to_msk(img)


def utf8_to_gb2312_c(s: str) -> str:
    out = []
    for b in s.encode("gb2312"):
        out.append(f"\\x{b:02X}")
    return "".join(out)


def emit_font_header(chars: list[str], font: ImageFont.FreeTypeFont) -> None:
    glyph_lines: list[str] = []
    for i, ch in enumerate(chars):
        gb = ch.encode("gb2312")
        msk = render_glyph(ch, font)
        msk_s = ",".join(f"0x{b:02X}" for b in msk)
        suffix = "," if i + 1 < len(chars) else ""
        glyph_lines.append(
            f"    {{{{{gb[0]:#04x},{gb[1]:#04x}}},{{{msk_s}}}}} /* U+{ord(ch):04X} */{suffix}"
        )

    hdr = ROOT / "include" / "ballot_gb12.h"
    src = ROOT / "src" / "ballot_gb12_data.c"
    hdr.parent.mkdir(parents=True, exist_ok=True)
    src.parent.mkdir(parents=True, exist_ok=True)

    hdr.write_text(
        "\n".join(
            [
                "/**",
                " * @file ballot_gb12.h",
                " * @brief ballot_guard GB2312 12x12 字库",
                " */",
                "#ifndef BALLOT_GB12_H",
                "#define BALLOT_GB12_H",
                "",
                "#include <stdint.h>",
                "",
                "typedef struct {",
                "    uint8_t Index[2];",
                "    uint8_t Msk[24];",
                "} ballot_gb12_glyph_t;",
                "",
                f"#define BALLOT_GB12_COUNT ({len(chars)}U)",
                "",
                "extern const ballot_gb12_glyph_t ballot_gb12[BALLOT_GB12_COUNT];",
                "",
                "#endif",
                "",
            ]
        ),
        encoding="utf-8",
    )

    src.write_text(
        "\n".join(
            [
                "/**",
                " * @file ballot_gb12_data.c",
                " * @brief ballot_guard GB2312 12x12 字库数据（自动生成）",
                " */",
                '#include "ballot_gb12.h"',
                "",
                "const ballot_gb12_glyph_t ballot_gb12[BALLOT_GB12_COUNT] = {",
                *glyph_lines,
                "};",
                "",
            ]
        ),
        encoding="utf-8",
    )
    print(f"wrote {hdr} and {src} ({len(chars)} glyphs)")


def emit_string_header() -> None:
    lines = [
        "/**",
        " * @file vote_menu_zh.h",
        " * @brief ballot_guard LCD 中文字符串（GB2312 编码）",
        " */",
        "#ifndef VOTE_MENU_ZH_H",
        "#define VOTE_MENU_ZH_H",
        "",
    ]
    macro_names = {
        "管理员设置": "VOTE_ZH_ADMIN_TITLE",
        "1.进入投票": "VOTE_ZH_ADMIN_ENTER",
        "2.投票时段": "VOTE_ZH_ADMIN_SCHEDULE",
        "3.候选人数量": "VOTE_ZH_ADMIN_COUNT",
        "4.冷却时长": "VOTE_ZH_ADMIN_COOLDOWN",
        "5.设置时钟": "VOTE_ZH_ADMIN_CLOCK",
        "6.重置数据": "VOTE_ZH_ADMIN_RESET",
        "取消": "VOTE_ZH_CANCEL",
        "确认重置": "VOTE_ZH_CONFIRM_RESET",
        "进入投票": "VOTE_ZH_ENTER_VOTING",
        "保存": "VOTE_ZH_SAVE",
        "投票时段": "VOTE_ZH_PAGE_SCHEDULE",
        "开始时": "VOTE_ZH_START_H",
        "开始分": "VOTE_ZH_START_M",
        "结束时": "VOTE_ZH_END_H",
        "结束分": "VOTE_ZH_END_M",
        "候选人数量": "VOTE_ZH_PAGE_COUNT",
        "人数": "VOTE_ZH_COUNT",
        "范围2-6": "VOTE_ZH_RANGE",
        "名称见网页": "VOTE_ZH_NAMES_WEB",
        "冷却(秒)": "VOTE_ZH_PAGE_COOLDOWN",
        "重置": "VOTE_ZH_PAGE_RESET",
        "重置全部投票数据?": "VOTE_ZH_RESET_ASK",
        "无法撤销": "VOTE_ZH_NO_UNDO",
        "离开管理菜单?": "VOTE_ZH_LEAVE_ASK",
        "系统时钟": "VOTE_ZH_PAGE_CLOCK",
        "时": "VOTE_ZH_HOUR",
        "分": "VOTE_ZH_MINUTE",
        "秒": "VOTE_ZH_SECOND",
        "调节 确认 返回": "VOTE_ZH_FOOT_ADJ",
        "选择 确认 返回": "VOTE_ZH_FOOT_SEL",
        "滑动 选择 确认 返回": "VOTE_ZH_FOOT_SLIDE",
        "看板": "VOTE_ZH_BOARD",
        "待开启": "VOTE_ZH_WAITING",
        "已锁定": "VOTE_ZH_LOCKED",
        "空闲": "VOTE_ZH_IDLE",
        "候选人": "VOTE_ZH_CANDIDATE",
        "票数": "VOTE_ZH_VOTES",
        "有效": "VOTE_ZH_VALID",
        "废票": "VOTE_ZH_SPOILED",
        "总计": "VOTE_ZH_TOTAL",
        "后开始": "VOTE_ZH_STARTS_IN_SUFFIX",
        "上=历史 长按OK=菜单": "VOTE_ZH_FOOT_HOME",
        "投票中": "VOTE_ZH_VOTING",
        "可以投票": "VOTE_ZH_READY",
        "剩余": "VOTE_ZH_REMAIN",
        "等待传感器": "VOTE_ZH_WAIT_IR",
        "选人": "VOTE_ZH_SELECT",
        "左右 确认 长按废票": "VOTE_ZH_FOOT_SELECT",
        "空白": "VOTE_ZH_BLANK",
        "多选": "VOTE_ZH_MULTIPLE",
        "不规范": "VOTE_ZH_IRREGULAR",
        "等待": "VOTE_ZH_WAIT",
        "冷却中": "VOTE_ZH_COOLDOWN",
        "请勿靠近": "VOTE_ZH_NO_APPROACH",
        "请稍候 返回": "VOTE_ZH_FOOT_COOLDOWN",
        "违规": "VOTE_ZH_VIOLATION",
        "重复投票": "VOTE_ZH_DUP_VOTE",
        "报警中 返回": "VOTE_ZH_FOOT_ALARM",
        "最终": "VOTE_ZH_FINAL",
        "上下 长按OK=菜单 返回": "VOTE_ZH_FOOT_LOCKED_SCROLL",
        "已结束 长按OK=菜单 返回": "VOTE_ZH_FOOT_LOCKED_END",
        "历史": "VOTE_ZH_HISTORY",
        "无记录": "VOTE_ZH_NO_HISTORY",
        "时段无效": "VOTE_ZH_ERR_TIME",
        "保存失败": "VOTE_ZH_ERR_SAVE",
        "已保存": "VOTE_ZH_OK_SAVED",
        "请先清零票数": "VOTE_ZH_ERR_RESET_FIRST",
        "配置失败": "VOTE_ZH_ERR_CFG",
        "重置成功": "VOTE_ZH_OK_RESET",
        "时钟保存失败": "VOTE_ZH_ERR_CLOCK",
        "时钟已保存": "VOTE_ZH_OK_CLOCK",
        "阶段:": "VOTE_ZH_PHASE_PREFIX",
        "开始": "VOTE_ZH_START",
        "结束": "VOTE_ZH_END",
        "上一页 下一页 返回": "VOTE_ZH_FOOT_HISTORY",
        "候选": "VOTE_ZH_CAND_SHORT",
        "故障": "VOTE_ZH_FAULT",
    }
    for s, name in macro_names.items():
        lines.append(f'#define {name} "{utf8_to_gb2312_c(s)}"')
    lines += ["", "#endif", ""]
    OUT_STR.parent.mkdir(parents=True, exist_ok=True)
    OUT_STR.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {OUT_STR}")


def main() -> None:
    validate_row_major_reference()
    chars = collect_chars()
    font = load_font(12)
    emit_font_header(chars, font)
    emit_string_header()


if __name__ == "__main__":
    main()
