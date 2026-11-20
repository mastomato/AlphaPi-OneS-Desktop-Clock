# -*- coding: utf-8 -*-
"""
AlphaPi One S / ota_1 收音机 —— M2「淡橙焦点」版（零重编译）

相对 v15 radio.bin 做两件事：

1) 改 6 个颜色常量（各 4 字节，IROM 常量池）。
   kAccent 取小智首页的 kWarm 0xFFD27A（温度 21°C 的暖沙黄），
   于是 LIVE / 电台图标 / NET RADIO / 列表计数 / 状态字 / 静音键
   全部变成与首页一致的淡暖色，青色不再由 kAccent 提供。

2) 把 2 条 l32r 指令从 kAccent 字面量槽重定向到 kMuted 字面量槽，
   让「频谱高条」与「列表选中行」保持青色：
     - 0x132D9D  RefreshMeter(): blti a8,8 之后的高条分支
     - 0x1341B8  RefreshList() : selected 分支的整条选中行
   kMuted 槽因此设为 kAccent 的青色 0x8FE3F0；它原本还被
   g_wifi(断网) / g_live(STOP) 引用，这两处顺势变成冷青，
   与「橙＝播放中」形成状态区分。

Xtensa L32R 为 3 字节小端指令：
   byte0 = (reg << 4) | 0x1, imm16 = byte1 | byte2 << 8（有符号）
   target = ((PC + 3) & ~3) + (imm16 << 2)
   PC 的 VMA = 0x40080020 + (file_off - 0x120020)
新旧槽相差 0xCC 字节，故 imm16 只需 +51（0xCC / 4）。

改完照例重算 1 字节 XOR 校验与尾部 SHA256。

用法：
  python patch_radio_m2.py <原 radio.bin> <输出目录>
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

# M2 淡橙版：焦点色直接照搬小智首页 kWarm
COLORS = {
    "kPaper":  0x0E1420,  # 深蓝黑底
    "kInk":    0x93AAC6,  # 柔键主文字
    "kAccent": 0xFFD27A,  # 焦点淡暖黄 = 首页温度色
    "kMuted":  0x8FE3F0,  # 青（频谱高条 / 选中行 / STOP / 断网）
    "kBarDim": 0x8A6440,  # 暗暖褐，频谱低条 / 音量暗格
    "kHair":   0x5C4834,  # 暗暖棕，5 条 1px 分隔线
}

# 需要保持青色的两条 l32r（文件偏移）
REDIR = {
    0x132D9D: "频谱高条 RefreshMeter()",
    0x1341B8: "列表选中行 RefreshList()",
}
FROM_VMA = SEG_VMA + (LITERALS["kAccent"] - SEG_FILE)   # 0x40081100
TO_VMA = SEG_VMA + (LITERALS["kMuted"] - SEG_FILE)      # 0x400811CC
DELTA_WORDS = (TO_VMA - FROM_VMA) // 4                   # 51

OUT_NAME = "radio-配色统一-M2淡橙版.bin"


def vma(file_off):
    return SEG_VMA + (file_off - SEG_FILE)


def l32r_target(data, p):
    """解析 p 处的 l32r，返回 (reg, imm16_signed, target_vma)。"""
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


def verify(data):
    ranges, _end = segment_payload_ranges(data)
    checksum = 0xEF
    for start, stop in ranges:
        for b in data[start:stop]:
            checksum ^= b
    return (checksum == data[-33],
            hashlib.sha256(data[:-32]).digest() == data[-32:],
            checksum)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    src, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    original = open(src, "rb").read()
    print("源文件: %s (%d B, md5 %s)" % (
        src, len(original), hashlib.md5(original).hexdigest()))

    # 前置断言：源镜像合法、6 个字面量位置/值/唯一性正确
    assert all(verify(original)[:2]), "源镜像校验和不合法"
    for name, off in LITERALS.items():
        found = struct.unpack_from("<I", original, off)[0]
        assert found == ORIGINAL[name], (
            "%s @ 0x%X 期望 %06X 实际 %06X" % (name, off, ORIGINAL[name], found))
        assert original.count(struct.pack("<I", found)) == 1, (
            "%s 字面量不唯一，拒绝打补丁" % name)
    for p, desc in REDIR.items():
        reg, imm, tgt = l32r_target(original, p)
        assert tgt == FROM_VMA, (
            "0x%X (%s) 目标 0x%08X，不是 kAccent 0x%08X" % (p, desc, tgt, FROM_VMA))
        print("  待重定向 0x%X  a%d imm=%+d -> 0x%08X  [%s]" % (
            p, reg, imm, tgt, desc))
    assert (TO_VMA - FROM_VMA) % 4 == 0
    print("字面量定位、源校验、l32r 目标断言：通过；imm 增量 = +%d" % DELTA_WORDS)

    buf = bytearray(original)

    # 1) 6 个颜色常量
    for name, off in LITERALS.items():
        struct.pack_into("<I", buf, off, COLORS[name])

    # 2) 2 条 l32r 的 imm16 加 DELTA_WORDS
    for p in REDIR:
        imm = buf[p + 1] | (buf[p + 2] << 8)
        new_imm = (imm + DELTA_WORDS) & 0xFFFF
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
    for p, desc in REDIR.items():
        reg, imm, tgt = l32r_target(result, p)
        assert tgt == TO_VMA, "0x%X 重定向后目标 0x%08X 不对" % (p, tgt)
        print("  已重定向 0x%X  a%d imm=%+d -> 0x%08X  [%s]" % (
            p, reg, imm, tgt, desc))
        # 该槽现在的值必须是青色
        assert struct.unpack_from("<I", result, LITERALS["kMuted"])[0] == 0x8FE3F0
    ok_cs, ok_sha, cs2 = verify(result)
    assert ok_cs and ok_sha, "输出镜像校验失败"
    diff = [i for i in range(len(original)) if original[i] != result[i]]
    in_literals = [i for i in diff if any(
        o <= i < o + 4 for o in LITERALS.values())]
    in_l32r = [i for i in diff if any(
        p + 1 <= i <= p + 2 for p in REDIR)]
    print("\n输出: %s" % out)
    print("  md5 %s" % hashlib.md5(result).hexdigest())
    print("  XOR 0x%02X  SHA256 %s" % (cs, sha[:16]))
    print("  自检 checksum=%s sha256=%s" % (ok_cs, ok_sha))
    print("  改动字节 %d = 常量池 %d + l32r %d + XOR 1 + SHA256 32" % (
        len(diff), len(in_literals), len(in_l32r)))
    assert len(diff) <= 24 + 4 + 1 + 32
    print("\n  色值映射:")
    for name in LITERALS:
        print("    %-8s %06X -> %06X" % (name, ORIGINAL[name], COLORS[name]))


if __name__ == "__main__":
    main()
