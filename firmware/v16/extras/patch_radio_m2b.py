# -*- coding: utf-8 -*-
"""
AlphaPi One S / ota_1 收音机 —— M2b「按键框 + 电台名 = 首页日期色」版（零重编译）

在 M2 淡橙版基础上再改两处，都换成小智首页日期的 kMuted = 0x7186A0：
  1) 四个按键（暂停 / 上台 / 下台 / 静音）的底色块。列表页 4 个提示键走同一个
     MakeKey()，会一起变，风格统一。
  2) 电台名称 g_title（"中国之声" 那行 17px 大字）。
     源码里它只在创建时设一次 kInk，刷新只改 x / 宽 / 对齐 / 文本，不改颜色。

源码落点（官方 2.3.2 ui_home.cc）：
  MakeKey(): Box(screen, x, 103, 35, 18, kInk, 2)   <- 按键块用 kInk
  g_title : Label(screen, &font_radio_17_2, kInk, 47, 31, "中国之声")
  frame   : border_color(kInk)                       <- 整屏 2px 外框，本次不动
  RefreshMeter(): g_bar_height[i] >= 8 ? kAccent : kBarDim
  RefreshUi()   : volume > i*20 ? kInk : kBarDim
kInk 同时是主文字色（19 条引用），直接改会拖暗全部文字，所以走指令级重定向。

实现：把 0x7186A0 写进 kBarDim 字面量槽，共 6 条 l32r 重定向：
  from kAccent -> kMuted   (2 条, M2 原有: 频谱高条 / 列表选中行留青)
  from kInk    -> kBarDim  (1 条, MakeKey 按键底)
  from kInk    -> kBarDim  (1 条, g_title 电台名)
  from kBarDim -> kHair    (2 条, 频谱低条 / 音量暗格，改吃暗暖棕 0x5C4834)
kHair 的 5 条分隔线保持 0x5C4834 不变，其余主文字 / 整屏外框保持 0x93AAC6。

Xtensa L32R 为 3 字节小端指令：
   byte0 = (reg << 4) | 0x1, imm16 = byte1 | byte2 << 8（有符号）
   target = ((PC + 3) & ~3) + (imm16 << 2)
   PC 的 VMA = 0x40080020 + (file_off - 0x120020)
改目标槽只需给 imm16 加上 (new_target - old_target) / 4。

改完照例重算 1 字节 XOR 校验与尾部 SHA256。

用法：
  python patch_radio_m2b.py <v15 原版 radio.bin> <输出目录>
"""
import hashlib
import os
import struct
import sys

SEG_FILE = 0x120020
SEG_VMA = 0x40080020

LITERALS = {
    "kAccent": 0x121100,
    "kBarDim": 0x121104,
    "kInk":    0x121114,
    "kPaper":  0x121118,
    "kMuted":  0x1211CC,
    "kHair":   0x121278,
}
ORIGINAL = {
    "kAccent": 0xBB351D,
    "kBarDim": 0xC4B79F,
    "kInk":    0x211B14,
    "kPaper":  0xFFF8E8,
    "kMuted":  0xA89B84,
    "kHair":   0xBEB09A,
}

# M2b：与 M2 淡橙版唯一差别是 kBarDim 槽改存日期色，供按键块使用
COLORS = {
    "kPaper":  0x0E1420,  # 深蓝黑底 / 按键文字
    "kInk":    0x93AAC6,  # 主文字 + 整屏 2px 外框（保持不变）
    "kAccent": 0xFFD27A,  # 焦点淡暖黄 = 首页温度色
    "kMuted":  0x8FE3F0,  # 青（频谱高条 / 选中行 / STOP / 断网）
    "kBarDim": 0x7186A0,  # 首页日期色 —— 现在是按键块专用槽
    "kHair":   0x5C4834,  # 暗暖棕：5 条分隔线 + 接管频谱低条/音量暗格
}

# 文件偏移 -> (原目标槽, 新目标槽, 说明)
REDIRECTS = {
    0x132D9D: ("kAccent", "kMuted",  "频谱高条 RefreshMeter() 留青"),
    0x1341B8: ("kAccent", "kMuted",  "列表选中行 RefreshList() 留青"),
    0x132E9D: ("kInk",    "kBarDim", "MakeKey() 四个按键底 -> 日期色"),
    0x1347F9: ("kInk",    "kBarDim", "g_title 电台名称 -> 日期色"),
    0x132D8B: ("kBarDim", "kHair",   "频谱低条 -> 暗暖棕"),
    0x1340E7: ("kBarDim", "kHair",   "音量暗格 -> 暗暖棕"),
}

OUT_NAME = "radio-配色统一-M2b日期色版.bin"


def vma(file_off):
    return SEG_VMA + (file_off - SEG_FILE)


def slot_vma(name):
    return vma(LITERALS[name])


def l32r_target(data, p):
    assert (data[p] & 0x0F) == 0x01, "不是 l32r opcode @ 0x%X" % p
    imm = data[p + 1] | (data[p + 2] << 8)
    signed = imm - 0x10000 if imm >= 0x8000 else imm
    return data[p] >> 4, signed, ((vma(p) + 3) & ~3) + (signed << 2)


def segment_payload_ranges(data):
    count = data[1]
    off = 24
    ranges = []
    for _ in range(count):
        _la, length = struct.unpack_from("<II", data, off)
        off += 8
        ranges.append((off, off + length))
        off += length
    return ranges, off


def verify(data):
    ranges, _end = segment_payload_ranges(data)
    checksum = 0xEF
    for start, stop in ranges:
        for b in data[start:stop]:
            checksum ^= b
    return (checksum == data[-33],
            hashlib.sha256(data[:-32]).digest() == data[-32:],
            checksum)


def fix_checksums(buf):
    ranges, _end = segment_payload_ranges(buf)
    checksum = 0xEF
    for start, stop in ranges:
        for b in buf[start:stop]:
            checksum ^= b
    buf[len(buf) - 33] = checksum
    digest = hashlib.sha256(bytes(buf[:-32])).digest()
    buf[-32:] = digest
    return checksum, digest.hex()


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    src, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    original = open(src, "rb").read()
    print("源文件: %s (%d B, md5 %s)" % (
        src, len(original), hashlib.md5(original).hexdigest()))

    # 前置断言：源镜像合法、6 个字面量位置/值/唯一性正确、6 条 l32r 目标正确
    assert all(verify(original)[:2]), "源镜像校验和不合法"
    for name, off in LITERALS.items():
        found = struct.unpack_from("<I", original, off)[0]
        assert found == ORIGINAL[name], (
            "%s @ 0x%X 期望 %06X 实际 %06X" % (name, off, ORIGINAL[name], found))
        assert original.count(struct.pack("<I", found)) == 1, (
            "%s 字面量不唯一，拒绝打补丁" % name)
    for p, (src_slot, dst_slot, desc) in REDIRECTS.items():
        reg, imm, tgt = l32r_target(original, p)
        assert tgt == slot_vma(src_slot), (
            "0x%X (%s) 目标 0x%08X，不是 %s 0x%08X" % (
                p, desc, tgt, src_slot, slot_vma(src_slot)))
        delta = (slot_vma(dst_slot) - slot_vma(src_slot)) // 4
        assert (slot_vma(dst_slot) - slot_vma(src_slot)) % 4 == 0
        assert -32768 <= imm + delta <= 32767, "0x%X imm16 越界" % p
        print("  待重定向 0x%X  a%d imm=%+d (%+d) -> %-8s  [%s]" % (
            p, reg, imm, delta, dst_slot, desc))

    buf = bytearray(original)

    # 1) 6 个颜色常量
    for name, off in LITERALS.items():
        struct.pack_into("<I", buf, off, COLORS[name])

    # 2) 6 条 l32r 的 imm16 加上槽差值（word 数）
    for p, (src_slot, dst_slot, _desc) in REDIRECTS.items():
        delta = (slot_vma(dst_slot) - slot_vma(src_slot)) // 4
        imm = buf[p + 1] | (buf[p + 2] << 8)
        new_imm = (imm + delta) & 0xFFFF
        buf[p + 1] = new_imm & 0xFF
        buf[p + 2] = (new_imm >> 8) & 0xFF

    # 3) 双校验
    cs, sha = fix_checksums(buf)
    out = os.path.join(outdir, OUT_NAME)
    open(out, "wb").write(bytes(buf))

    # ---- 自检 ----
    result = open(out, "rb").read()
    assert len(result) == len(original), "文件长度变化"
    for name, off in LITERALS.items():
        assert struct.unpack_from("<I", result, off)[0] == COLORS[name]
    for p, (src_slot, dst_slot, desc) in REDIRECTS.items():
        reg, imm, tgt = l32r_target(result, p)
        assert tgt == slot_vma(dst_slot), (
            "0x%X 重定向后目标 0x%08X 不对" % (p, tgt))
        loaded = struct.unpack_from("<I", result, LITERALS[dst_slot])[0]
        print("  已重定向 0x%X  a%d -> %-8s = #%06X  [%s]" % (
            p, reg, dst_slot, loaded, desc))
    ok_cs, ok_sha, cs2 = verify(result)
    assert ok_cs and ok_sha, "输出镜像校验失败"
    diff = [i for i in range(len(original)) if original[i] != result[i]]
    print("\n输出: %s" % out)
    print("  md5 %s" % hashlib.md5(result).hexdigest())
    print("  XOR 0x%02X  SHA256 %s" % (cs, sha[:16]))
    print("  自检 checksum=%s sha256=%s" % (ok_cs, ok_sha))
    print("  改动字节 %d（常量池 + l32r imm + XOR 1 + SHA256 32）" % len(diff))
    assert len(diff) <= 24 + 12 + 1 + 32
    print("\n  色值映射:")
    for name in LITERALS:
        print("    %-8s %06X -> %06X" % (name, ORIGINAL[name], COLORS[name]))


if __name__ == "__main__":
    main()
