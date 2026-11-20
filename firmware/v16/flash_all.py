#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""AlphaPi One S 双固件 —— 六段式一键烧录。

用法：
    python flash_all.py                 # 自动找串口，等板子进 ROM 下载模式
    python flash_all.py COM20          # 指定串口
    python flash_all.py --check-only   # 只校验六段文件，不烧录
    python flash_all.py --wait 300     # 最多等 300 秒（默认 600 秒）

特点：
  * 优先使用本机 ESP-IDF 自带的 esptool 4.8.1；没有则退回系统 python 的 esptool
    （需 pip install esptool pyserial）。
  * 不写死 COM 口：按 USB VID 2F4E / 303A 自动发现，或识别新出现的串口；
    自动排除主板 COM1 和 Spreadtrum 手机调试口（VID 1782）。
  * 芯片已在 ROM 下载器时可直接写，不必拨开关。
  * 一次写六段；若中途失败，自动回退成逐段重试。
  * --after no_reset：烧完不自动复位（这块板 RTS 接不到 EN），请手动拨 OFF→ON。
"""
import argparse
import hashlib
import os
import subprocess
import sys
import time

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))

# 地址、文件名、说明 —— 顺序与 README / md5sums.txt 完全一致
PARTS = [
    (0x001000, "bootloader.bin",         "bootloader"),
    (0x008000, "partition-table.bin",    "分区表（双 OTA 槽）"),
    (0x00D000, "ota_data_initial.bin",   "otadata → 启动槽重置为 ota_0"),
    (0x020000, "xiaozhi.bin",            "小智 + 自定义深色时钟页/天气（ota_0）"),
    (0x310000, "radio.bin",              "网络收音机（ota_1）"),
    (0x600000, "generated_assets.bin",   "字体/表情资源"),
]

IDF_PY = r"C:\esp\.espressif\python_env\idf6.0_py3.11_env\Scripts\python.exe"
ESPTOOL_DIR = r"C:\esp\esptool4"


def resolve_runtime():
    """返回 (python_exe, env, 描述)。找不到 esptool 时 python_exe 为 None。"""
    env = os.environ.copy()
    if os.path.isfile(IDF_PY) and os.path.isfile(
            os.path.join(ESPTOOL_DIR, "esptool", "__init__.py")):
        env["PYTHONPATH"] = ESPTOOL_DIR + os.pathsep + env.get("PYTHONPATH", "")
        return IDF_PY, env, "esptool 4.8.1（本机 ESP-IDF 环境）"
    try:
        r = subprocess.run(
            [sys.executable, "-c", "import esptool; print(esptool.__version__)"],
            capture_output=True, text=True, timeout=20)
        if r.returncode == 0:
            return sys.executable, env, "esptool " + r.stdout.strip() + "（系统 Python）"
    except Exception:
        pass
    return None, env, None


def file_md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_files():
    sums = {}
    p = os.path.join(HERE, "md5sums.txt")
    if os.path.isfile(p):
        with open(p, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                bits = line.split()
                if len(bits) >= 2:
                    sums[bits[-1].lstrip("*")] = bits[0].lower()
    print("=== 校验六段文件 ===")
    ok = True
    for off, name, label in PARTS:
        path = os.path.join(HERE, name)
        if not os.path.isfile(path):
            print("  [缺失] 0x%06X  %s" % (off, name))
            ok = False
            continue
        digest = file_md5(path)
        size = os.path.getsize(path)
        expected = sums.get(name)
        if expected and digest != expected:
            state = "MD5 不符！"
            ok = False
        else:
            state = "OK"
        print("  0x%06X  %-24s %9d B  %s  %s" %
              (off, name, size, digest[:12], state))
    return ok


def discover_ports():
    """返回 [(port, 是否本板)]；导入失败返回 None。"""
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    out = []
    for p in list_ports.comports():
        hwid = (p.hwid or "").upper()
        if p.device == "COM1" or "VID_1782" in hwid or "VID:1782" in hwid:
            continue
        is_board = any(k in hwid for k in ("VID_2F4E", "VID:2F4E",
                                           "VID_303A", "VID:303A"))
        out.append((p.device, is_board))
    # 本板优先
    return sorted(out, key=lambda x: not x[1])


def cmd_base(py, port, after="no_reset", before="no_reset"):
    return [py, "-m", "esptool", "--chip", "esp32s2", "-p", port,
            "-b", "115200", "--before", before, "--after", after,
            "--connect-attempts", "1"]


def run(cmd, env, timeout):
    try:
        r = subprocess.run(cmd, env=env, capture_output=True, text=True,
                           errors="replace", timeout=timeout, cwd=HERE)
        return r.returncode, (r.stdout or "") + (r.stderr or "")
    except subprocess.TimeoutExpired:
        return -1, "timeout"
    except Exception as e:
        return -1, str(e)


def probe(py, env, port):
    rc, out = run(cmd_base(py, port) + ["chip_id"], env, 25)
    return rc == 0, out


def flash_once(py, env, port, parts, subcmd="write_flash", before="no_reset"):
    cmd = cmd_base(py, port, before=before) + [
        subcmd, "-fm", "dio", "-fs", "8MB", "-ff", "80m"]
    for off, name, _ in parts:
        cmd += [hex(off), os.path.join(HERE, name)]
    print("  执行：一次写入 %d 段 ..." % len(parts), flush=True)
    return run(cmd, env, 540)


def flash_per_part(py, env, port, subcmd):
    """连接不稳定时逐段写，每段最多重试 3 次。"""
    print("  回退到逐段写入 ...", flush=True)
    done = []
    for off, name, label in PARTS:
        for attempt in range(1, 4):
            cmd = cmd_base(py, port) + [
                subcmd, "-fm", "dio", "-fs", "8MB", "-ff", "80m",
                hex(off), os.path.join(HERE, name)]
            rc, out = run(cmd, env, 300)
            if "Hash of data verified" in out:
                print("  [OK] 0x%06X  %s" % (off, name), flush=True)
                done.append(name)
                break
            tail = [l for l in out.strip().splitlines() if l.strip()][-1:] or [""]
            print("  [重试 %d] 0x%06X %s: %s" %
                  (attempt, off, name, tail[0][:100]), flush=True)
            time.sleep(1)
        else:
            print("  [失败] 0x%06X %s，中止。" % (off, name), flush=True)
            return done
    return done


def main():
    ap = argparse.ArgumentParser(description="AlphaPi One S 六段式烧录")
    ap.add_argument("port", nargs="?", help="串口，如 COM20；省略则自动发现")
    ap.add_argument("--check-only", action="store_true", help="只校验文件")
    ap.add_argument("--wait", type=int, default=600, help="等待下载器秒数")
    ap.add_argument("--try-reset", action="store_true",
                    help="先尝试 DTR/RTS 复位进下载模式（本板通常无效）")
    args = ap.parse_args()

    if not verify_files():
        print("\n!! 文件校验未通过，请重新解压刷机包。")
        return 1
    if args.check_only:
        print("\n校验完成（--check-only，未烧录）。")
        return 0

    py, env, desc = resolve_runtime()
    if py is None:
        print("\n!! 找不到 esptool。请先执行：pip install esptool pyserial")
        return 1
    print("\n烧录工具：%s" % desc)

    fixed_port = bool(args.port)
    port = args.port
    if fixed_port:
        print("指定端口：%s" % port)
    else:
        print("正在查找板子 ...（未上电或没进下载模式时会一直等）")

    deadline = time.time() + args.wait
    hinted = False
    while True:
        if not fixed_port:
            # 每轮重新枚举：断电重上电后 COM 号可能变化，不能死守第一次找到的口
            found = discover_ports()
            if found is None:
                print("!! 缺少 pyserial，请用 `python flash_all.py COMxx` 指定端口。")
                return 1
            new_port = found[0][0] if found else None
            if new_port != port:
                port = new_port
                hinted = False
                print("端口：%s" % (port or "（暂未发现，等待上电）"), flush=True)
        if port:
            ok, out = probe(py, env, port)
            if ok:
                print("已连接 ROM 下载器：%s" % port, flush=True)
                break
            if args.try_reset:
                ok2, _ = probe_before(py, env, port, "default_reset")
                if ok2:
                    print("DTR/RTS 复位成功：%s" % port, flush=True)
                    break
            if not hinted:
                tail = [l for l in out.strip().splitlines() if l.strip()][-1:] or [""]
                print("  端口还不是 ROM 下载器：%s" % tail[0][:100])
                print("  → 请拨电源开关 OFF → 等 2 秒 → ON（保持进下载模式的动作），"
                      "脚本会自动继续。", flush=True)
                hinted = True
        if time.time() > deadline:
            print("\n!! 等待超时，没等到 ROM 下载器。板子要进下载模式才能写 flash。")
            return 2
        time.sleep(2)

    # esptool 5.x 可能只接受连字符写法，先试下划线，失败再换
    subcmd = "write_flash"
    rc, out = flash_once(py, env, port, PARTS, subcmd)
    if ("No such command" in out) or ("no such command" in out.lower()):
        print("  改用 esptool 5.x 命令名 write-flash ...", flush=True)
        subcmd = "write-flash"
        rc, out = flash_once(py, env, port, PARTS, subcmd)

    verified = out.count("Hash of data verified")
    if rc == 0 and verified >= len(PARTS):
        print("\n=== 六段全部写入并校验通过（%d/%d）===" % (verified, len(PARTS)))
    else:
        print("\n一次写入未完整通过（校验 %d/%d），转入逐段重试。" %
              (verified, len(PARTS)))
        tail = [l for l in out.strip().splitlines() if l.strip()][-3:]
        for l in tail:
            print("  " + l[:120])
        done = flash_per_part(py, env, port, subcmd)
        if len(done) != len(PARTS):
            print("\n!! 仍有段写入失败，请确认没有浏览器刷机页/串口监视器占用端口后重跑。")
            return 3
        print("\n=== 逐段写入完成（6/6）===")

    print("  ota_0 = 小智（自定义界面 v3）  ota_1 = 网络收音机")
    print("  otadata 已重置为从 ota_0 启动；NVS 里的 Wi-Fi 配置保留。")
    print("\n烧录时未自动复位：请拨一次 OFF → ON 正常开机（不进下载模式）。")
    print("开机后检查：时间是否为本地时间 / A 长按进配网 / 长按 A 回时钟页 / D 长按切收音机。")
    return 0


def probe_before(py, env, port, before):
    rc, _ = run(cmd_base(py, port, before=before) + ["chip_id"], env, 25)
    return rc == 0, ""


if __name__ == "__main__":
    sys.exit(main())
