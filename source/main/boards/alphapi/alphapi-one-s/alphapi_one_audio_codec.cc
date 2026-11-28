#include "alphapi_one_audio_codec.h"

#include "board.h"
#include "config.h"      /* AUDIO_* 宏（采样率/声道/音量范围/分块大小）*/
#include "display.h"
#include "alert_dat.h"

/* ===== 测试开关：把 alert.dat 整文件发给 N32（配合 §14.4 的缓冲区声明试探）=====
 *  §14.4：发声正路 = c.init() + c.InitBackground_buf(40960) + playAsync(文件)
 *  "少 InitBackground_buf 这步不响" ⇒ 先发 40960 的缓冲区声明再发 WAV 文件 */
#define ALPHAPI_ALERT_TEST 0

/* ===== 麦克风读取寄存器开关（2026-10-06）=====
 * 实测：寄存器 0x12（麦克风音量）随说话 5→255 剧变 ⇒ 麦克风确实在拾音，
 *       硬件与录音链路都没问题。但 0x11 读回的 8bit 数据相邻样本几乎不相关
 *       （平均|Δ|≈82/255）⇒ 不是线性 8bit PCM。
 * 依据：手册 §6.1 说 .dat 音效是 ADPCM 编码；出厂 ByteCode 里
 *       playAddr = 22 (0x16) 表示 ADPCM 流，21 (0x15) 是 PCM。
 *       ⇒ 录音流很可能同样在 0x16。
 * 改回 0x11 只需把下面这行改成 0x11。 */
#define ALPHAPI_MIC_SELFTEST 0

/* ===== ALPHAPI_LINEAR_DUMP：连续（非环形）录制 128KB 麦克风原始数据 =====
 * 目的：环形 dump 的 8 块来自不同时刻，拼接后听起来像"变调"，无法判断采样率。
 * 这里改成：开机后第一次读麦克风起，顺序不间断地写满 128KB（24000Hz 下约 5.5 秒）。
 * 位置：ota_1 偏移 0x1F0000 ⇒ flash 0x500000（在 app 镜像之后，安全）。
 * 置 0 关闭。 */
#define ALPHAPI_LINEAR_DUMP 0
#define ALPHAPI_LD_OFFSET   0x1F0000
#define ALPHAPI_LD_BYTES    (128 * 1024)
unsigned g_alphapi_ld_len = 0;   /* 非 volatile：只在 Read() 里访问 */
/* AGC 当前增益，供诊断观察 */
int g_alphapi_agc_gain = 0;
#define ALPHAPI_MIC_READ_ADDR 0x11

/* ALPHAPI_MIC_ALIGN（2026-10-06 关键修复）：
 * 环形 dump 显示 0x16 读回的数据 99.5% 是 0x00、且只有 2 种字节值 ⇒ 是空帧，不是音频。
 * 真相：0x11 才是麦克风寄存器，N32 没数据时回 len=0 的空帧，但帧结构仍是
 *       [0x81][0x11][0x00][checksum] —— 有 1 个校验和字节！
 * 之前代码遇到 len<=0 直接 continue，没读掉那个校验和字节 ⇒ 接收流永久错位 1 字节
 * ⇒ 之后所有数据都是垃圾（表现为"不相关满幅噪声"，bl=10719）。
 * 修复：len==0 时读掉校验和再重试，保持帧对齐。 */
/* ALPHAPI_IMA：按 IMA ADPCM 4bit 解码（每个字节 2 个样本）。
 * 依据：读 0x16 时所有错误计数归零（bl 10719→0）⇒ 0x16 是正确寄存器；
 *       数据绝大多数是 0x00、偶发 0x64 ⇒ 结构化，且 00 在 IMA ADPCM 里
 *       表示"步长不变、预测值不变"= 静音，符合 ADPCM 特征。
 * 置 0 可退回 8bit 线性 PCM 解释。 */
#define ALPHAPI_IMA 0

#include <cstdio>
#include <cstring>
#include <cmath>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <driver/i2s_std.h>

/* ★ 上传链路埋点（定义在 main/audio/audio_service.cc）*/
extern volatile unsigned g_alphapi_dbg_rad;      /* ReadAudioData 调用次数 */
extern volatile unsigned g_alphapi_dbg_evp;      /* EnableVoiceProcessing 调用次数 */
extern volatile unsigned g_alphapi_dbg_itask;    /* AudioInputTask 循环次数 */
extern volatile unsigned g_alphapi_dbg_ecall;    /* EnableInput 调用次数 */
extern volatile unsigned g_alphapi_dbg_engfail;  /* 引擎初始化失败次数 */
extern volatile unsigned g_alphapi_dbg_eq;       /* ★ 推入待发送队列的帧数 */
extern volatile unsigned g_alphapi_dbg_drop;     /* 编码队列溢出丢弃帧数 */
extern volatile unsigned g_alphapi_dbg_enc_ok;   /* 编码成功次数 */
extern volatile unsigned g_alphapi_dbg_enc_err;  /* 编码失败次数 */
extern volatile unsigned g_alphapi_dbg_enc_bad;  /* ★ 帧大小不匹配丢弃次数 */
extern volatile unsigned g_alphapi_dbg_sq;       /* ★ 进待发送包队列的包数 */
extern volatile unsigned g_alphapi_dbg_lastpcm;  /* 实际 PCM 样本数 */
extern volatile unsigned g_alphapi_dbg_framesz;  /* 编码器期望样本数 */
extern volatile unsigned g_alphapi_dbg_sa_in;    /* SendAudio 调用次数 */
extern volatile unsigned g_alphapi_dbg_sa_noudp; /* (1) udp_ 为空 */
extern volatile unsigned g_alphapi_dbg_sa_nonce; /* (2) nonce 长度不对 */
extern volatile unsigned g_alphapi_dbg_sa_crypt; /* (3) 加密失败 */
extern volatile unsigned g_alphapi_dbg_sa_fail;  /* (4) Send 返回 <=0 */
extern volatile unsigned g_alphapi_dbg_sa_sent;  /* OK 真正发出 */
extern volatile unsigned g_alphapi_dbg_udpok;    /* UDP 通道建立成功次数 */
/* ★ 会话快照：每次开始聆听前，把上一轮的统计存下来再清零，
 * 这样诊断里同时能看到"上一轮"和"本轮"，用于判断
 * "第一句能懂、后面不行" 是不是因为录音没有真正重启。 */
volatile unsigned g_alphapi_sess_prev_req = 0, g_alphapi_sess_prev_rx = 0;
volatile unsigned g_alphapi_sess_prev_bl  = 0, g_alphapi_sess_prev_cor_n = 0;
volatile unsigned g_alphapi_sess_prev_cor_s = 0, g_alphapi_sess_cur_cor_s = 0;
volatile unsigned g_alphapi_sess_n = 0;

/* 【已废弃，勿用】候选"输入增益"寄存器扫描表。
 * 2026-11-09 结论：**N32 没有可配置的输入增益寄存器**。
 * 证据：用出厂 MicroPython 固件把 0x00–0x20 全表读回，0x02–0x0F 全为 0；
 *       往这 12 个候选地址写 100 实测全部无效（COR 恒≈3）。
 * 麦克风本身完全正常（静音 std≈1.0，说话 std 5–19，min 34 / max 229），
 * 之前的"电平极低"是发错命令 0x10=2 拿到垃圾流造成的误判。 */
static const uint8_t g_gain_cand[] = { 0x01, 0x02, 0x12, 0x13, 0x16, 0x17,
                                       0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D };
#define GAIN_CAND_N ((int)(sizeof(g_gain_cand) / sizeof(g_gain_cand[0])))
/* 2026-10-09 正式版：增益/会话 flash 日志（GainLogAppend/SessLogAppend）已删除。 */

/*
 * ============================ AlphaPi One S 音频链路 ============================
 * ESP32-S2 --(UART1 460800 8N1, TX=GPIO3 / RX=GPIO0)--> N32L403 协处理器 --> 板载功放/喇叭
 *
 * 协议（依据：厂商《AlphaPi_One_S_开发者手册_完整版.md》§3 + §8.1 + §8.2，标注"已逆向验证"）：
 *   写请求(主机->N32): 0x90 | addr(寄存器地址) | len | data... | checksum(全部字节相加取低8位)
 *   写响应(N32->主机): 0x91 | addr | status(0=成功)
 *   读请求(主机->N32): 0x80 | addr | len
 *   读响应(N32->主机): 0x81 | addr | len | data... | checksum
 *
 *   寄存器地址：
 *     0x10 AUDIO_CONTROL  音频控制：**1 = 开始录音，0 = 停止/播放**
 *                         （2026-11-09 逐值扫描 0..7 确认；发 2 会得到垃圾流）
 *     0x11 AUDIO_READ     音频读取（每次 ≤200 字节）
 *     0x14 AUDIO_VOLUME   音量 0-100（手册 §8.2；出厂内部钳到 0-6）
 *     0x15 AUDIO_STREAM   音频流写入（每次 ≤200 字节）    ← 播放 PCM 用这个
 *
 * PCM 格式：8bit 无符号 / 单声道
 *   **录音 22050 Hz，播放 24000 Hz**（两者不同，见 config.h 里的实测依据）
 *   s16 -> u8: (sample + 32768) >> 8        u8 -> s16: (u8 - 128) * 256
 *
 * 已解决（别再当成未解问题去改）：
 *   · 播放：按 accepted 字节数重发余量即可，**不要发数据长度 0xFFFFFFFF 的 WAV 头**（N32 会静音）
 *   · 录音：`len == 0` 的空帧也要读掉 1 字节校验和，否则流永久错位
 *   · 连续对话：播放结束必须 `AudioCodec::EnableInput(false)`，否则第二句没反应
 *   · 「说一半就停」：输出采样率必须是 24000（按 22050 喂料会让 N32 缓冲句中见底）
 * ==============================================================================
 */

/* ---- 屏幕诊断计数器（启用音频后串口桥被 N32 占用，只能看屏幕）---- */
volatile uint32_t g_alphapi_audio_tx_bytes = 0;   /* 成功写出的 PCM 字节 */
volatile uint32_t g_alphapi_audio_retry     = 0;  /* accepted==0 的重试次数 */
volatile uint32_t g_alphapi_audio_fail      = 0;  /* 写超时/协议错误次数 */
volatile uint32_t g_alphapi_audio_rx_bytes  = 0;  /* 从麦克风读回的字节 */
volatile uint32_t g_alphapi_audio_acks      = 0;  /* 收到的应答数 */
volatile int      g_alphapi_audio_last_acc  = -1; /* 最近一次 accepted */
volatile int      g_alphapi_n32_wake        = -1; /* 0x0F 唤醒命令的应答结果（>=0 成功） */
volatile int      g_alphapi_started         = 0;  /* Start() 是否执行过 */
volatile unsigned g_alphapi_pcm_calls       = 0;
volatile int64_t g_alphapi_last_write_us   = 0;  /* 最近一次 Write() 时刻（麦克风切换保护用） */
static constexpr int kMicSwitchGuardMs     = 600; /* 0x10 切换前距最后一次播放的最小间隔 */
/* 麦克风嗅探：N32 是否在主动推数据（每轮记录前 32 字节） */
volatile int      g_alphapi_mic_sniff_len   = 0;
volatile unsigned g_alphapi_mic_sniff_count = 0;
volatile unsigned g_alphapi_mic_req_count   = 0;
uint8_t           g_alphapi_mic_sniff[32]   = {0};
/* mic probe sweep results */
volatile unsigned g_alphapi_probe_hit[8] = {0};
volatile unsigned g_alphapi_probe_n     = 0;
/* 输入/输出使能调用计数（诊断用） */
volatile unsigned g_alphapi_ena_in      = 0;   /* EnableInput 调用次数 */
volatile unsigned g_alphapi_ena_in_on   = 0;   /* EnableInput(true) */
volatile unsigned g_alphapi_ena_out_on  = 0;   /* EnableOutput(true) */
/* ALPHAPI_MIC_V3：麦克风读诊断计数 */
volatile unsigned g_alphapi_mic_timeout  = 0;  /* 3字节头/数据 读超时 */
volatile unsigned g_alphapi_mic_badhdr   = 0;  /* 头不匹配 */
volatile unsigned g_alphapi_mic_badlen   = 0;  /* len 非法 */
volatile unsigned g_alphapi_mic_badck    = 0;  /* 校验错 */
volatile unsigned g_alphapi_mic_empty    = 0;  /* N32 明确没数据 */
volatile unsigned g_alphapi_mic_probe[16] = {0}; /* 录音寄存器试探结果 */
volatile unsigned g_alphapi_mic_dump_n   = 0;  /* dump 写盘次数 */
/* ALPHAPI_MICVOL：读寄存器 0x12（麦克风音量）做拾音诊断
 * 依据出厂 controlBoardOneS.read_mic_volume(): v = uart_read(18,1)[3]，v>=40 表示有声 */
volatile unsigned g_alphapi_micvol_n     = 0;
volatile unsigned g_alphapi_micvol_last  = 0;
volatile unsigned g_alphapi_micvol_min   = 255;
volatile unsigned g_alphapi_micvol_max   = 0;
/* ALPHAPI_ADPCM：相邻字节平均绝对差（越小越像真音频；随机数据≈85/255）
 * 以及"非零字节比例"、"字节种类数"——都是判据 */
volatile unsigned g_alphapi_mic_cor_sum  = 0;
volatile unsigned g_alphapi_mic_cor_n    = 0;
volatile unsigned g_alphapi_mic_prev     = 0xFFFFFFFF;
volatile unsigned g_alphapi_mic_kind[256] = {0};
volatile unsigned g_alphapi_mic_kind_n   = 0;
/* ALPHAPI_IMA：IMA ADPCM 解码状态与统计 */
volatile int g_alphapi_ima_val = 0;   /* 预测值 */
volatile int g_alphapi_ima_idx = 0;   /* 步长索引 */
volatile unsigned g_alphapi_ima_nz = 0;
/* ===== ALPHAPI_MIC_SELFTEST：麦克风启动场景自测 =====
 * 每个场景记录：corr_sum/corr_n（相邻字节绝对差，归一化后 <0.2 视为真音频）、
 * 读到的字节数、非零字节数。结果写到 flash 供 esptool 读回。 */
#define MICST_SCEN 10
volatile unsigned g_micst_corr_n[MICST_SCEN]  = {0};
volatile unsigned g_micst_corr_sum[MICST_SCEN]= {0};
volatile unsigned g_micst_bytes[MICST_SCEN]   = {0};
volatile unsigned g_micst_nonzero[MICST_SCEN] = {0};
volatile unsigned g_micst_maxb[MICST_SCEN]    = {0};
/* 每个场景读到的前 8 个原始字节：用来辨认"常量值"到底是什么 */
volatile unsigned char g_micst_first[MICST_SCEN][8] = {{0}};
/* 增益扫描: 8 个寄存器 × 8 个值，记录读回 PCM 的幅度(std) */
#define GAIN_NR 8
#define GAIN_NV 8
volatile unsigned g_gain_std[GAIN_NR][GAIN_NV] = {{0}};
volatile unsigned g_gain_raw[GAIN_NR][GAIN_NV] = {{0}};
/* ★ 自测期间置 1：让后台诊断任务停止一切 UART 操作，避免两个任务抢 UART1 导致崩溃 */
volatile int g_mictest_active = 0;
volatile int      g_micst_done = 0;   /* 非零字节数 */
volatile unsigned g_alphapi_ima_maxb = 0; /* 最大字节值 */

/* IMA ADPCM (DVI4) 标准表 */
static const int kImaIndex[16] = {
    8, 6, 4, 2, -1, -1, -1, -1, 7, 5, 3, 1, -1, -1, -1, -1
};
static const int kImaStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};
/* nibble: 4bit 有符号差分；返回解码后的 16bit 样本
 * 2026-10-09：ALPHAPI_IMA=0 时不再被调用，加属性防 -Wunused-function（-Werror） */
__attribute__((unused)) static int ImaDecodeNibble(int nibble)
{
    int step = kImaStep[g_alphapi_ima_idx];
    int diff = step >> 3;
    if (nibble & 1) { diff += step >> 2; }
    if (nibble & 2) { diff += step >> 1; }
    if (nibble & 4) { diff += step; }
    int v = g_alphapi_ima_val;
    if (nibble & 8) { v -= diff; } else { v += diff; }
    if (v > 32767) { v = 32767; }
    if (v < -32768) { v = -32768; }
    g_alphapi_ima_val = v;
    g_alphapi_ima_idx += kImaIndex[nibble & 0x0F];
    if (g_alphapi_ima_idx < 0) { g_alphapi_ima_idx = 0; }
    if (g_alphapi_ima_idx > 88) { g_alphapi_ima_idx = 88; }
    return v;
}
volatile unsigned g_alphapi_alert_sent  = 0;  /* Write() 被调用次数 */

/* TLS 诊断全局：在本文件定义（上游组件的 esp_ssl.cc 不再提供它们） */
volatile int g_alphapi_tls_err   = 0;
volatile int g_alphapi_tls_flags = 0;
volatile int g_alphapi_tls_esp   = 0;
volatile int g_alphapi_tls_num   = 0;

static const char* TAG = "AlphaPiAudio";

/* ---- 协议常量（手册 §3.1 / §8.1 / §8.2）---- */
static constexpr uint8_t kFrameHostHdr  = 0x90;   /* 主机 → N32 写请求 */
static constexpr uint8_t kFrameDevHdr   = 0x91;   /* N32 → 主机 写响应 */
static constexpr uint8_t kMicStreamHdr  = 0x80;   /* 主机 → N32 读请求 */
static constexpr uint8_t kMicStreamResp = 0x81;   /* N32 → 主机 读响应 */

static constexpr uint8_t kCmdMicSwitch  = 0x10;   /* AUDIO_CONTROL */
static constexpr uint8_t kCmdMicRead    = 0x11;   /* AUDIO_READ */
static constexpr uint8_t kCmdHwVolume   = 0x14;   /* AUDIO_VOLUME 0-100 */
static constexpr uint8_t kCmdPcmWrite   = 0x15;   /* AUDIO_STREAM <=200 字节/帧 */
static constexpr uint8_t kCmdLinkControl = 0x0C;  /* 官方 EnableInput 独有：录音启动后写 2 */

static constexpr int kAckTimeoutMs = 300;
/* 诊断记录位置：ota_1 内偏移 0x2E0000 ⇒ flash 0x5F0000
 * （app 镜像 2.15MB 结束于 0x52D000，其后是附加 SHA256；0x5F0000 安全）*/
static constexpr size_t kDiagOff = 0x2E0000;

/* 诊断记录放在 ota_1 分区的最后 4KB（开头被 OTA 机制占用会被覆盖）。
 * 读回：esptool read_flash <ota_1起始 + size - 4096> 0x1000 tls.bin */
static constexpr uint32_t kDiagRecordOffsetFromEnd = 4096;

/* 屏幕诊断开关：0 = 屏幕上不显示任何调试信息（默认）。
 * 注意：诊断【任务本身】总是运行——它负责把 TLS 错误码写进 ota_1 分区供 PC 读回；
 *       只有"往屏幕上画"那部分受这个宏控制。 */
/* ★ 诊断记录写入开关：设为 0 是关键！
 * 之前每秒擦写 ota_1 前 4KB —— 若 app 正运行在 ota_1 上，会破坏自身代码 ⇒ 黑屏/崩溃。
 * 现在完全关闭，改用屏幕或串口调试。 */
#define ALPHAPI_DIAG_WRITE 0  /* 诊断已收官：写 flash 有擦写损耗且拖慢音频，正式版关闭 */
#define ALPHAPI_SCREEN_DIAG 0

/* 诊断任务：每秒把计数显示到屏幕（通知区显示 TLS 码，对话区显示音频计数） */
static void MaybeDumpPixelSnap(void);   /* 像素取证搬运（定义在 AudioDiagTask 之后） */
static void AudioDiagTask(void* arg)
{
    /* 自测运行期间完全不碰 UART，避免与自测抢 UART1 */
    while (g_mictest_active) { vTaskDelay(pdMS_TO_TICKS(50)); }
    char buf[96];
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        MaybeDumpPixelSnap();
        char tlsbuf[48];
        snprintf(tlsbuf, sizeof(tlsbuf), "M%d H%d P%d W%u",
                 g_alphapi_tls_err,
                 (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 g_alphapi_pcm_calls);
        snprintf(buf, sizeof(buf), "AUD w%d tx%u rty%u f%u rx%u ack%u l%d",
                 g_alphapi_n32_wake,
                 (unsigned)g_alphapi_audio_tx_bytes, (unsigned)g_alphapi_audio_retry,
                 (unsigned)g_alphapi_audio_fail, (unsigned)g_alphapi_audio_rx_bytes,
                 (unsigned)g_alphapi_audio_acks, g_alphapi_audio_last_acc);
        (void)buf;
        (void)tlsbuf;
#if ALPHAPI_SCREEN_DIAG
        auto d = Board::GetInstance().GetDisplay();
        if (d != nullptr) {
            d->ShowNotification(tlsbuf);
            d->SetChatMessage("dbg", buf);
        }
#else
        /* 屏幕保持干净。2026-10-09 正式版：flash 诊断写入（ALPHAPI_DIAG_WRITE=0）
         * 已全部关闭，这里不再擦写 ota_1。诊断计数器仍在内存中累积。 */
        {
        }
#endif
    }
}

/* ★ 颜色取证（第 5 轮）：RawPush 首个像素块的 u16 快照。
 * 写到 flash 0x5B0000（ota_1 内 0x2A0000，一次写 512B），PC 用 esptool 读回。 */
#include <esp_partition.h>
static uint16_t s_pixel_snap[64];
static volatile bool s_pixel_snap_ready = false;

extern "C" void AlphapiSavePixelSnapImpl(const uint16_t* px, size_t n)
{
    if (s_pixel_snap_ready || n > 64) return;
    for (size_t k = 0; k < n; k++) s_pixel_snap[k] = px[k];
    s_pixel_snap_ready = true;
}

extern "C" void AlphapiSavePixelSnap(const uint16_t* px, size_t n)
{
    AlphapiSavePixelSnapImpl(px, n);
}

/* 由 AudioDiagTask 首轮搬运（避开 RawPush 的 LVGL 任务上下文直接写 flash） */
static void MaybeDumpPixelSnap(void)
{
    static bool done = false;
    if (done || !s_pixel_snap_ready) return;
    const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                        ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    if (!p) return;
    static uint8_t rec[512];
    memcpy(rec, "PSNAP", 5);
    uint32_t seq = 1;
    memcpy(rec + 5, &seq, 4);
    memcpy(rec + 16, s_pixel_snap, sizeof(s_pixel_snap));
    esp_partition_erase_range(p, 0x2A0000, 4096);
    esp_partition_write(p, 0x2A0000, rec, sizeof(rec));
    done = true;
}



/* ================= ALPHAPI_MIC_SELFTEST =================
 * 目的：查出「怎样才能让 N32 真正开始采样麦克风」。
 * 思路：把各种"启动录音"的时序组合跑一遍，每个组合读若干次 0x11，
 *       统计相邻字节绝对差（真音频会明显偏小，随机数据≈0.4~0.5）。
 * 判据：归一化相邻差 < 0.2 ⇒ 找到真音频。
 */
extern volatile unsigned g_micst_corr_n[MICST_SCEN];
extern volatile unsigned g_micst_corr_sum[MICST_SCEN];
extern volatile unsigned g_micst_bytes[MICST_SCEN];
extern volatile unsigned g_micst_nonzero[MICST_SCEN];
extern volatile unsigned g_micst_maxb[MICST_SCEN];
extern volatile int      g_micst_done;

static void MicTestRawWrite(uint8_t addr, const uint8_t* d, int n)
{
    /* 直接发 0x90 帧，不等待应答（自测用，避免被应答语义干扰） */
    uint8_t buf[8];
    buf[0] = 0x90;
    buf[1] = addr;
    buf[2] = (uint8_t)n;
    for (int i = 0; i < n && i < 4; i++) { buf[3 + i] = d[i]; }
    uint8_t ck = 0;
    for (int i = 0; i < 3 + n; i++) { ck = (uint8_t)(ck + buf[i]); }
    buf[3 + n] = ck;
    uart_write_bytes(UART_NUM_1, reinterpret_cast<const char*>(buf), 4 + n);
    vTaskDelay(pdMS_TO_TICKS(2));   /* 不用 uart_wait_tx_done，避免在未就绪/忙时崩溃 */
}

static void __attribute__((unused)) MicTestProbe(int si, int reads)
{
    /* 读 reads 次 0x11（每次 200 字节），统计相邻字节绝对差 */
    uint32_t cs = 0, cn = 0, by = 0, nz = 0, mx = 0;
    int prev = -1;
    for (int r = 0; r < reads; r++) {
        uint8_t req[3] = { 0x80, 0x11, 200 };
        uart_write_bytes(UART_NUM_1, reinterpret_cast<const char*>(req), 3);
        uint8_t hdr[3];
        if (uart_read_bytes(UART_NUM_1, hdr, 3, pdMS_TO_TICKS(100)) != 3) { continue; }
        if (hdr[0] != 0x81 || hdr[1] != 0x11) { uart_flush_input(UART_NUM_1); continue; }
        int len = hdr[2];
        if (len == 0) { uint8_t ck; uart_read_bytes(UART_NUM_1, &ck, 1, pdMS_TO_TICKS(50)); continue; }
        if (len > 200) { uart_flush_input(UART_NUM_1); continue; }
        uint8_t tail[201];
        if (uart_read_bytes(UART_NUM_1, tail, len + 1, pdMS_TO_TICKS(100)) != len + 1) {
            uart_flush_input(UART_NUM_1); continue;
        }
        uint8_t s0 = 0x81, s1 = 0x11, s2 = (uint8_t)len;
        uint8_t cks = (uint8_t)(s0 + s1 + s2);
        for (int i = 0; i < len; i++) { cks = (uint8_t)(cks + tail[i]); }
        if (cks != tail[len]) { uart_flush_input(UART_NUM_1); continue; }
        for (int i = 0; i < len; i++) {
            uint8_t b = tail[i];
            if (by < 8) { g_micst_first[si][by] = b; }
            by++;
            if (b) { nz++; }
            if (b > mx) { mx = b; }
            if (prev >= 0) {
                int d = (int)b - prev;
                if (d < 0) { d = -d; }
                cs += (uint32_t)d;
                cn++;
            }
            prev = b;
        }
        taskYIELD();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    g_micst_corr_sum[si] = cs;
    g_micst_corr_n[si]   = cn;
    g_micst_bytes[si]    = by;
    g_micst_nonzero[si]  = nz;
    g_micst_maxb[si]     = mx;
}


struct MicTestScen { uint8_t cmd; uint16_t delay; uint8_t claim; };

static void __attribute__((unused)) MicTestDump(const struct MicTestScen* sc, int upto)
{
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)0x11, "ota_1");
    if (p == nullptr) { return; }
    static char out[768];
    int n = 0;
    n += snprintf(out + n, sizeof(out) - n, "MICST v7 u=%d", upto);
    for (int i = 0; i <= upto && i < MICST_SCEN; i++) {
        unsigned cn = g_micst_corr_n[i];
        unsigned av = (cn > 0) ? (g_micst_corr_sum[i] / cn) : 0;
        n += snprintf(out + n, sizeof(out) - n,
                      "|S%d c=%u av=%u nz=%u f=%02X%02X%02X%02X",
                      i, sc[i].cmd, av, g_micst_nonzero[i],
                      g_micst_first[i][0], g_micst_first[i][1],
                      g_micst_first[i][2], g_micst_first[i][3]);
    }
    /* ★ 必须用 4KB 对齐的偏移，否则 esp_partition_erase_range 直接报 INVALID_ARG，
     * 后续写入全失败（这就是之前只有 upto=0 的原因）。
     * 标记区用 0x2A0000 这个扇区，结果区单独用 0x2B0000 扇区，互不干扰。 */
    esp_partition_erase_range(p, 0x2B0000, 4096);
    esp_partition_write(p, 0x2B0000, out, 512);
}

/* 只写一行状态：每个 slot 写在独立偏移，避免 NOR flash 覆盖写产生乱码 */
static void MicTestMark(int slot, const char* s, int a, int b)
{
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)0x11, "ota_1");
    if (p == nullptr || slot < 0 || slot > 31) { return; }
    static char m[48];
    int n = snprintf(m, sizeof(m), "MK%02d %s %d %d", slot, s, a, b);
    for (int i = n; i < 48; i++) { m[i] = 0; }
    esp_partition_write(p, 0x2A0000 + (size_t)slot * 0x40, m, sizeof(m));
}

/* 开头擦一次整个 4KB，之后各 slot 都在干净的 0xFF 上写 */
static void MicTestErase(void)
{
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)0x11, "ota_1");
    if (p != nullptr) { esp_partition_erase_range(p, 0x2A0000, 4096); }
}


/* ===== 增益扫描：找出能放大麦克风 PCM 幅度的寄存器 =====
 * 背景：0x10=1 已能录到真实音频，但幅度只有 ±4（std=1.36），
 *       而 N32 的音量寄存器显示 max=255（它检测到了声音）⇒ 输出增益太低。
 * 做法：启动录音后，逐个写候选寄存器/值，读 8 块 0x11，算 PCM 的 std。 */
static void MicGainSweep(void)
{
    static const uint8_t regs[GAIN_NR] = { 0x13, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E };
    static const uint8_t vals[GAIN_NV] = { 0, 3, 10, 30, 60, 100, 160, 255 };

    for (int r = 0; r < GAIN_NR; r++) {
        for (int v = 0; v < GAIN_NV; v++) {
            uint8_t z = 0;
            MicTestRawWrite(0x10, &z, 1);                 /* 停止 */
            vTaskDelay(pdMS_TO_TICKS(30));
            uint8_t one = 1;
            MicTestRawWrite(0x10, &one, 1);               /* 0x10=1 开始录音 */
            vTaskDelay(pdMS_TO_TICKS(60));

            uint8_t gv = vals[v];
            MicTestRawWrite(regs[r], &gv, 1);             /* 写候选增益 */
            vTaskDelay(pdMS_TO_TICKS(60));

            /* 读 8 块 0x11，累计平方和算 std */
            unsigned long sum = 0, sum2 = 0, cnt = 0;
            for (int k = 0; k < 8; k++) {
                uint8_t req[3] = { 0x80, 0x11, 200 };
                uart_write_bytes(UART_NUM_1, reinterpret_cast<const char*>(req), 3);
                uint8_t hdr[3];
                if (uart_read_bytes(UART_NUM_1, hdr, 3, pdMS_TO_TICKS(80)) != 3) { continue; }
                if (hdr[0] != 0x81 || hdr[1] != 0x11) { uart_flush_input(UART_NUM_1); continue; }
                int len = hdr[2];
                if (len == 0) { uint8_t ck; uart_read_bytes(UART_NUM_1, &ck, 1, pdMS_TO_TICKS(50)); continue; }
                if (len > 200) { uart_flush_input(UART_NUM_1); continue; }
                uint8_t tail[201];
                if (uart_read_bytes(UART_NUM_1, tail, len + 1, pdMS_TO_TICKS(80)) != len + 1) {
                    uart_flush_input(UART_NUM_1); continue;
                }
                for (int i = 0; i < len; i++) {
                    unsigned long b = tail[i];
                    sum += b; sum2 += b * b; cnt++;
                }
                vTaskDelay(pdMS_TO_TICKS(3));
            }
            if (cnt > 0) {
                /* std = sqrt(E[x^2] - E[x]^2)，用整数近似 */
                unsigned long mean = sum / cnt;
                unsigned long var  = (sum2 / cnt) > (mean * mean) ? (sum2 / cnt - mean * mean) : 0;
                unsigned stdv = 0;
                while (stdv * stdv < var && stdv < 200) { stdv++; }
                g_gain_std[r][v] = stdv;
                g_gain_raw[r][v] = cnt;
            }
        }
    }
    /* 收尾：停录音 */
    uint8_t z = 0;
    MicTestRawWrite(0x10, &z, 1);
}

static void __attribute__((unused)) MicSelfTestInline()
{
    MicTestErase();
    MicTestMark(0, "ENTER", 0, 0);
    vTaskDelay(pdMS_TO_TICKS(2000));   /* 等系统稳定 */

    /* 场景表：cmd=写 0x10 的值；delay=写之前先停播放并等待的毫秒数；
     * claim=1 表示先发一个 40960 长度的缓冲声明再录音 */
    struct MicTestScen sc[MICST_SCEN] __attribute__((unused)) = {
        { 0, 300, 0 },   /* S0 cmd=0 对照 */
        { 1, 300, 0 },   /* S1 cmd=1 */
        { 2, 300, 0 },   /* S2 cmd=2（已知噪声）*/
        { 3, 300, 0 },   /* S3 cmd=3 */
        { 4, 300, 0 },   /* S4 cmd=4 */
        { 5, 300, 0 },   /* S5 cmd=5 */
        { 6, 300, 0 },   /* S6 cmd=6 */
        { 7, 300, 0 },   /* S7 cmd=7 */
        { 2, 300, 0 },   /* S8 复测 cmd=2 */
        { 2, 300, 0 },   /* S9 复测 cmd=2 */
    };

    MicTestMark(1, "BEGIN", 0, 0);
    g_mictest_active = 1;
    MicTestMark(2, "GAIN", 0, 0);
    MicGainSweep();
    g_mictest_active = 0;
    MicTestMark(3, "GAINDONE", 0, 0);

    /* 输出增益扫描结果 */
    {
        const esp_partition_t* p = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)0x11, "ota_1");
        if (p != nullptr) {
            static const uint8_t regs[GAIN_NR] = { 0x12, 0x13, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D };
            static const uint8_t vals[GAIN_NV] __attribute__((unused)) = { 1, 3, 5, 10, 30, 100, 200, 255 };
            static char out[1024];
            int n = snprintf(out, sizeof(out), "GAIN v8 baseline(std)");
            for (int r = 0; r < GAIN_NR; r++) {
                n += snprintf(out + n, sizeof(out) - n, "|R%02X", regs[r]);
                for (int v = 0; v < GAIN_NV; v++) {
                    n += snprintf(out + n, sizeof(out) - n, ",%u", g_gain_std[r][v]);
                }
            }
            esp_partition_erase_range(p, 0x2B0000, 4096);
            esp_partition_write(p, 0x2B0000, out, 768);
        }
    }
    g_micst_done = 1;
}

/* 独立任务：把 alert.dat 整文件流式发给 N32（不阻塞启动）
 * 要点：UART 0x15 分块发送，200 字节/帧；不等应答，只做小幅节流，
 *       避免像之前那样在 Start() 里阻塞十几秒导致黑屏。*/
static void __attribute__((unused)) AlertPlaybackTask(void* arg)
{
    auto* codec = static_cast<AlphaPiOneAudioCodec*>(arg);
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* ★ 按出厂 controlBoardOneS 的真实序列（2026-10-06 从 .mpy 字节码解出）：
     *   0x0F 写 1  初始化（失败重试）
     *   0x14 写 v  音量（手册 0-100，出厂内部钳到 0-6）
     *   0x10 写 0  开始播放            ← 之前漏了这步
     *   0x15 块    200 字节/块，带流控重发 ← 之前没做流控（应答第3字节=接受字节数）
     * alert.dat 自带 46 字节 WAV 头（fmt=18），整文件原样发送。 */
    uint8_t one = 1, vol = 5, zero = 0;
    unsigned wake_ok = 0;
    for (int i = 0; i < 5; i++) {
        if (codec->DispatchFrame(0x0F, &one, 1) > 0) { wake_ok = 1; break; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    codec->DispatchFrame(0x14, &vol, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    int start_ok = codec->DispatchFrame(0x10, &zero, 1);
    vTaskDelay(pdMS_TO_TICKS(30));

    unsigned sent = 0, frames = 0;
    for (unsigned off = 0; off < kAlertDatLen; off += 200) {
        unsigned n = kAlertDatLen - off;
        if (n > 200) n = 200;
        if (!codec->WriteAudioFlow(kCmdPcmWrite, &kAlertDat[off], n)) break;
        sent += n; frames++;
    }
    g_alphapi_alert_sent = (sent & 0xFFFFFFu) | (wake_ok << 24);
    ESP_LOGW(TAG, "alert.dat: wake=%u start=%d sent=%u frames=%u", wake_ok, start_ok, sent, frames);
    vTaskDelete(nullptr);
}

/* 供板级构造函数调用：启动诊断任务（总是创建） */
extern "C" void AlphapiStartAudioDiag(void)
{
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    xTaskCreate(AudioDiagTask, "audiodiag", 4096, nullptr, 1, nullptr);
}

/* ------------------------------ 基础协议 ------------------------------ */

uint8_t AlphaPiOneAudioCodec::Checksum(const uint8_t* data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = static_cast<uint8_t>(sum + data[i]);
    }
    return sum;
}

void AlphaPiOneAudioCodec::SendControlFrame(uint8_t cmd, const uint8_t* payload, size_t len)
{
    if (!uart_ready_) {
        return;
    }
    if (len > 200) {
        ESP_LOGE(TAG, "payload too long: %u", static_cast<unsigned>(len));
        return;
    }
    uint8_t frame[8 + 200];
    frame[0] = kFrameHostHdr;
    frame[1] = cmd;
    frame[2] = static_cast<uint8_t>(len);
    if (len > 0 && payload != nullptr) {
        memcpy(&frame[3], payload, len);
    }
    frame[3 + len] = Checksum(frame, 3 + len);
    uart_write_bytes(uart_num_, reinterpret_cast<const char*>(frame), 4 + len);
}

/* 帧格式试探（排障用：已确认第 0 种——0x90/addr/len/data/cksum——是对的） */
void AlphaPiOneAudioCodec::SendFrameVariant(int f, uint8_t cmd, const uint8_t* data, size_t len)
{
    uint8_t buf[8 + 200];
    size_t n = 0;
    switch (f) {
        case 0: buf[n++] = 0x90; buf[n++] = cmd; buf[n++] = (uint8_t)len;
                memcpy(buf + n, data, len); n += len; buf[n] = Checksum(buf, n); n++; break;
        case 1: buf[n++] = cmd; buf[n++] = (uint8_t)len;
                memcpy(buf + n, data, len); n += len; buf[n] = Checksum(buf, n); n++; break;
        case 2: buf[n++] = cmd; memcpy(buf + n, data, len); n += len; break;
        case 3: buf[n++] = 0x90; buf[n++] = cmd; buf[n++] = (uint8_t)len;
                memcpy(buf + n, data, len); n += len; break;
        case 4: buf[n++] = cmd; buf[n++] = (uint8_t)len;
                memcpy(buf + n, data, len); n += len; break;
        default: buf[n++] = 0x90; buf[n++] = cmd;
                memcpy(buf + n, data, len); n += len; buf[n] = Checksum(buf, n); n++; break;
    }
    uart_write_bytes(uart_num_, reinterpret_cast<const char*>(buf), n);
}

void AlphaPiOneAudioCodec::SendTestFrame(uint8_t cmd, const uint8_t* payload, size_t len)
{
    SendControlFrame(cmd, payload, len);
}

int AlphaPiOneAudioCodec::ReadTestAck(uint8_t cmd, TickType_t timeout_ticks)
{
    return ReadAck(cmd, timeout_ticks);
}

int AlphaPiOneAudioCodec::ReadAck(uint8_t expected_cmd, TickType_t timeout_ticks)
{
    if (!uart_ready_) {
        return -1;
    }
    uint8_t buf[3] = {0};
    int read = uart_read_bytes(uart_num_, buf, sizeof(buf), timeout_ticks);
    if (read != sizeof(buf)) {
        return -1;
    }
    if (buf[0] != kFrameDevHdr || buf[1] != expected_cmd) {
        ESP_LOGD(TAG, "unexpected ack: %02X %02X %02X", buf[0], buf[1], buf[2]);
        return -1;
    }
    g_alphapi_audio_acks = g_alphapi_audio_acks + 1;
    return buf[2];
}

/* ================= 出厂协议正确实现（2026-10-06 从 controlBoardOneS.mpy 解出）=================
 * machine_helper_one_s.uart_write(data):
 *     uart.write(data); r = uart.read(3); return r[2]      ← 返回【被接受的字节数】！
 * machine_helper_one_s.uart_write_buf(addr, data):
 *     n = uart_write(addr, data)
 *     while n < len(data): time.sleep_ms(10); n += uart_write(addr, data[n:])
 * 播放序列（PlayRecordMission.__tickPlay）：
 *     uart_write(16, bytearray([0]))     ← 寄存器 0x10 写 0 = 开始播放
 *     uart_write(21, chunk)              ← 寄存器 0x15 = PCM 数据（每块 200 字节）
 * 音量（set_speaker_volume）：
 *     uart_write(20, bytearray([ClampD(v, 0, 6)]))  ← 出厂内部钳制到 0-6（但手册 0x14 量程是 0-100）
 * =======================================================================================*/
int AlphaPiOneAudioCodec::DispatchFrame(uint8_t cmd, const uint8_t* payload, size_t len)
{
    if (!uart_ready_ || len > 200) {
        return 0;
    }
    /* 清掉可能残留的应答，避免与本次应答串位 */
    uint8_t junk[3];
    int drained = 0;
    while (uart_read_bytes(uart_num_, junk, sizeof(junk), 0) == sizeof(junk) && drained < 16) {
        drained++;
    }
    SendControlFrame(cmd, payload, len);
    uint8_t buf[3] = {0};
    int got = uart_read_bytes(uart_num_, buf, sizeof(buf), pdMS_TO_TICKS(kAckTimeoutMs));
    if (got != (int)sizeof(buf) || buf[0] != kFrameDevHdr || buf[1] != cmd) {
        g_alphapi_audio_fail = g_alphapi_audio_fail + 1;
        return 0;
    }
    g_alphapi_audio_acks = g_alphapi_audio_acks + 1;
    return buf[2];
}

bool AlphaPiOneAudioCodec::WriteAudioFlow(uint8_t addr, const uint8_t* data, size_t len)
{
    if (len == 0) {
        return false;
    }
    size_t sent = 0;
    int stalls = 0;
    while (sent < len) {
        size_t n = len - sent;
        if (n > 200) {
            n = 200;
        }
        int accepted = DispatchFrame(addr, data + sent, n);
        if (accepted > 0) {
            sent += static_cast<size_t>(accepted);
            stalls = 0;
            /* 7ms 固定节流试验失败（实测死机/更差），已撤。
             * DispatchFrame 的 ack 往返本身提供节流。 */
        } else {
            if (++stalls > 100) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return true;
}

/* 出厂 __tickRecord：byte_to_write = b'\x80\x11' + [n]（n = min(200, 剩余)），
 * 写完读 3 字节头 [0x81,0x11,len]，再读 len+1 字节（数据+校验）。 */
int AlphaPiOneAudioCodec::RequestMicBlock()
{
    /* ===== 已恢复到"能正常对话"的版本（2026-10-07）=====
     * 历史教训（不要再重蹈）：
     *   尝试 A：把超长帧"整帧读完后继续"（加 100ms 阻塞等待）
     *           ⇒ 长度与实际字节数不符时卡死读取循环 ⇒ 完全没反应 ✗
     *   尝试 B：改成滑动窗口非阻塞解析器
     *           ⇒ 响应还没到达就读，produced 恒为 0 ⇒ 完全没反应 ✗
     * 结论：本板 N32 的响应时序需要"先发请求、再短等一会儿读"，
     *       当前这个"请求-应答 + 失败即 flush"的实现在实机上【确实能跑通】
     *       （实测服务器回过 400KB 语音、用户听到过回答）。
     *       它的缺点是会因超长帧偶发失步（表现为"偶尔能听到"），
     *       但比重写成完全没反应要好得多。要动这里必须非常小心。 */
    if (!uart_ready_) {
        return 0;
    }
    for (int attempt = 0; attempt < 6; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        uint8_t req[3];
        req[0] = kMicStreamHdr;
        req[1] = ALPHAPI_MIC_READ_ADDR;
        req[2] = 200;
        uart_write_bytes(uart_num_, reinterpret_cast<const char*>(req), sizeof(req));

        uint8_t hdr[3];
        if (uart_read_bytes(uart_num_, hdr, 3, pdMS_TO_TICKS(100)) != 3) {
            g_alphapi_mic_timeout = g_alphapi_mic_timeout + 1;
            uart_flush_input(uart_num_);
            continue;
        }
        if (hdr[0] != kMicStreamResp || hdr[1] != ALPHAPI_MIC_READ_ADDR) {
            g_alphapi_mic_badhdr = g_alphapi_mic_badhdr + 1;
            uart_flush_input(uart_num_);
            continue;
        }

        int len = hdr[2];
        if (len == 0) {
            /* 空帧 [0x81][addr][0x00][cksum] ⇒ 必须读掉那 1 字节校验和，不要 flush */
            uint8_t ck = 0;
            uart_read_bytes(uart_num_, &ck, 1, pdMS_TO_TICKS(50));
            g_alphapi_mic_badlen = g_alphapi_mic_badlen + 1;
            continue;
        }
        if (len > 200) {
            g_alphapi_mic_badlen = g_alphapi_mic_badlen + 1;
            uart_flush_input(uart_num_);
            continue;
        }

        uint8_t tail[201];
        if (uart_read_bytes(uart_num_, tail, len + 1, pdMS_TO_TICKS(100)) != len + 1) {
            g_alphapi_mic_timeout = g_alphapi_mic_timeout + 1;
            uart_flush_input(uart_num_);
            continue;
        }

        uint8_t chk_src[203];
        chk_src[0] = hdr[0];
        chk_src[1] = hdr[1];
        chk_src[2] = hdr[2];
        memcpy(&chk_src[3], tail, (size_t)len);
        if (Checksum(chk_src, 3 + len) != tail[len]) {
            g_alphapi_mic_badck = g_alphapi_mic_badck + 1;
            uart_flush_input(uart_num_);
            continue;
        }

        memcpy(mic_pcm_, tail, (size_t)len);

        /* 统计：相邻字节绝对差 + 字节种类数 + 非零数，用于判定数据是不是真音频 */
        for (int i = 0; i < len; i++) {
            unsigned b = tail[i];
            if (g_alphapi_mic_kind[b] == 0) {
                g_alphapi_mic_kind[b] = 1;
                g_alphapi_mic_kind_n = g_alphapi_mic_kind_n + 1;
            }
            if (b != 0) { g_alphapi_ima_nz = g_alphapi_ima_nz + 1; }
            if (b > g_alphapi_ima_maxb) { g_alphapi_ima_maxb = b; }
            if (g_alphapi_mic_prev != 0xFFFFFFFF) {
                int d = (int)b - (int)g_alphapi_mic_prev;
                if (d < 0) { d = -d; }
                g_alphapi_mic_cor_sum = g_alphapi_mic_cor_sum + (unsigned)d;
                g_alphapi_mic_cor_n = g_alphapi_mic_cor_n + 1;
            }
            g_alphapi_mic_prev = b;
        }
        for (int i = 0; i < len && i < 8; i++) {
            g_alphapi_mic_sniff[i] = tail[i];
        }
        g_alphapi_mic_sniff_count = g_alphapi_mic_sniff_count + 1;
        g_alphapi_mic_sniff_len = len;
        g_alphapi_mic_req_count = g_alphapi_mic_req_count + 1;
        return len;
    }
    return 0;
}

void AlphaPiOneAudioCodec::ResetRxBuffer()
{
    if (uart_ready_) {
        uart_flush_input(uart_num_);
    }
}

/* ------------------------------ 生命周期 ------------------------------ */

AlphaPiOneAudioCodec::AlphaPiOneAudioCodec(uart_port_t uart_num, gpio_num_t tx_pin,
                                           gpio_num_t rx_pin, int input_sample_rate,
                                           int output_sample_rate, int baud_rate)
    : uart_num_(uart_num), tx_pin_(tx_pin), rx_pin_(rx_pin), baud_rate_(baud_rate)
{
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_channels_ = AUDIO_INPUT_CHANNELS;
    output_channels_ = AUDIO_OUTPUT_CHANNELS;
    duplex_ = AUDIO_DUPLEX;
    input_reference_ = AUDIO_INPUT_REFERENCE;
    output_volume_ = 70;

    // 播放会话空闲检测（决定何时补发 WAV 头）
    esp_timer_create_args_t timer_args = {
        .callback = &AlphaPiOneAudioCodec::PlaybackIdleCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alphapi_pb_idle",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &playback_idle_timer_));
}

AlphaPiOneAudioCodec::~AlphaPiOneAudioCodec()
{
    if (playback_idle_timer_ != nullptr) {
        esp_timer_stop(playback_idle_timer_);
        esp_timer_delete(playback_idle_timer_);
        playback_idle_timer_ = nullptr;
    }
    if (i2s_tx_ != nullptr) { i2s_channel_disable(i2s_tx_); i2s_del_channel(i2s_tx_); i2s_tx_ = nullptr; }
    if (i2s_rx_ != nullptr) { i2s_channel_disable(i2s_rx_); i2s_del_channel(i2s_rx_); i2s_rx_ = nullptr; }
    i2s_ready_ = false;
    if (uart_ready_) {
        uart_driver_delete(uart_num_);
        uart_ready_ = false;
    }
}

void AlphaPiOneAudioCodec::Start()
{
    AudioCodec::Start();

    if (!uart_ready_) {
        uart_config_t uart_config = {};
        uart_config.baud_rate = baud_rate_;
        uart_config.data_bits = UART_DATA_8_BITS;
        uart_config.parity = UART_PARITY_DISABLE;
        uart_config.stop_bits = UART_STOP_BITS_1;
        uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        uart_config.source_clk = UART_SCLK_DEFAULT;

        /* 缓冲大小照厂商实现（官方固件字符串里是 4096/4096） */
        ESP_ERROR_CHECK(uart_driver_install(uart_num_, 4096, 4096, 0, nullptr, 0));
        ESP_ERROR_CHECK(uart_param_config(uart_num_, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(uart_num_, tx_pin_, rx_pin_,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        uart_ready_ = true;
    }
    /* ALPHAPI: 16KB 麦克风 dump 缓冲放 PSRAM，别占内部堆 */
    if (mic_dump_ == nullptr) {
        mic_dump_ = static_cast<uint8_t*>(
            heap_caps_malloc(kMicDumpBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (mic_dump_ == nullptr) {
            mic_dump_ = static_cast<uint8_t*>(heap_caps_malloc(kMicDumpBytes, MALLOC_CAP_8BIT));
        }
    }
    g_alphapi_started = 1;
#if ALPHAPI_MIC_SELFTEST
    /* ===== 自测模式：同步执行，不建任务（避免调度/单核问题）=====
     * 先立刻写一个"已进入自测"标记，证明 Start() 走到了这里。 */
    {
        const esp_partition_t* mp = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)0x11, "ota_1");
        if (mp != nullptr) {
            static char mk[64];
            snprintf(mk, sizeof(mk), "MICST v3 ENTERED Start()");
            esp_partition_erase_range(mp, 0x2A0000, 4096);
            esp_partition_write(mp, 0x2A0000, mk, sizeof(mk));
        } else {
            /* 分区都找不到：写到别处做标记也没用，直接返回 */
        }
    }
    MicSelfTestInline();
    return;   /* 自测模式下不再跑后面的流程 */
#endif
    /* 正常模式：继续走后面的唤醒/探测流程 */

    /* 唤醒 N32：厂商 init_ones.py 用 uart_write(0x0f, bytearray([1]))，失败隔 20ms 重试。
     * 实测量产板子上电后可能要数秒才应答，故长时间重试（最多 ~10 秒）。 */
    {
        const uint8_t wake_payload = 0x01;
        int ok_format = -1;
        for (int round = 0; round < 50 && ok_format < 0; round++) {
            for (int f = 0; f < 6 && ok_format < 0; f++) {
                SendFrameVariant(f, 0x0F, &wake_payload, 1);
                int ack = ReadAck(0x0F, pdMS_TO_TICKS(100));
                if (ack >= 0) {
                    ok_format = f;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (ok_format < 0) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
        g_alphapi_n32_wake = ok_format;
        ESP_LOGW(TAG, "N32 wake result = %d", ok_format);
    }

    /* 2026-10-18 正式版：删除 ALPHAPI_MIC_PROBE 开机寄存器扫描。
     * 它每次开机都向 0x12/0x13/0x16/0x0F/0x17/0x18 写 1/0/50/100——
     * 已实测写 0x12 会把 N32 自动增益压坏（MV max 255→98），
     * 正式版绝不能再在启动时碰这些寄存器。 */
#if 0
    /* mic register probe sweep: start recording then try candidate regs */
    {
        const uint8_t cand_regs[] = {0x12, 0x13, 0x16, 0x0F, 0x17, 0x18};
        const uint8_t cand_vals[] = {1, 0, 50, 100};
        const int n_reg = (int)(sizeof(cand_regs)/sizeof(cand_regs[0]));
        const int n_val = (int)(sizeof(cand_vals)/sizeof(cand_vals[0]));
        const uint8_t rec_on = 1;
        SendControlFrame(kCmdMicSwitch, &rec_on, 1);
        ReadAck(kCmdMicSwitch, pdMS_TO_TICKS(kAckTimeoutMs));
        mic_streaming_ = true;
        vTaskDelay(pdMS_TO_TICKS(50));
        unsigned seq = 0;
        for (int r = 0; r < n_reg; r++) {
            for (int v = 0; v < n_val; v++) {
                const uint8_t payload = cand_vals[v];
                SendControlFrame(cand_regs[r], &payload, 1);
                ReadAck(cand_regs[r], pdMS_TO_TICKS(100));
                vTaskDelay(pdMS_TO_TICKS(15));
                uint8_t sniff[32];
                int n = uart_read_bytes(uart_num_, sniff, sizeof(sniff), pdMS_TO_TICKS(30));
                if (n > 0 && g_alphapi_probe_n < 8) {
                    g_alphapi_probe_hit[g_alphapi_probe_n] = seq;
                    g_alphapi_probe_n = g_alphapi_probe_n + 1;
                    g_alphapi_mic_sniff_count = g_alphapi_mic_sniff_count + 1;
                    g_alphapi_mic_sniff_len = n;
                    for (int i = 0; i < n && i < 32; i++) g_alphapi_mic_sniff[i] = sniff[i];
                }
                seq++;
            }
        }
    }
#endif
    /* ============ I2S 音频数据通道（手册 §11：音频数据走 I2S，UART 只走控制）============
     * 依据：手册 §11「控制接口=UART，数据接口=I2S」、§4.1 信号 BCLK/WS/DOUT/DIN、
     *       手册 L2494-2497 官方 config：BCLK=5 WS=4 OUT=7 IN=6（P1 口 4/5/6/7）
     * 位宽用 16bit（I2S 不支持 8bit；xiaozhi 内部也是 s16 单声道）*/
#define ALPHAPI_I2S_ENABLE 0
#if ALPHAPI_I2S_ENABLE
    if (!i2s_ready_) {
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        chan_cfg.auto_clear = true;
        esp_err_t e1 = i2s_new_channel(&chan_cfg, &i2s_tx_, &i2s_rx_);
        if (e1 != ESP_OK) {
            ESP_LOGW(TAG, "i2s_new_channel failed: %d", (int)e1);
            i2s_tx_ = nullptr; i2s_rx_ = nullptr;
        } else {
            i2s_std_config_t std_cfg = {};
            std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)output_sample_rate_);
            std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                                   I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
            std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
            std_cfg.gpio_cfg.bclk = (gpio_num_t)AUDIO_I2S_GPIO_BCLK;
            std_cfg.gpio_cfg.ws   = (gpio_num_t)AUDIO_I2S_GPIO_WS;
            std_cfg.gpio_cfg.dout = (gpio_num_t)AUDIO_I2S_GPIO_DATA_OUT;
            std_cfg.gpio_cfg.din  = (gpio_num_t)AUDIO_I2S_GPIO_DATA_IN;
            esp_err_t e2 = ESP_OK;
            if (i2s_tx_) e2 |= i2s_channel_init_std_mode(i2s_tx_, &std_cfg);
            if (i2s_rx_) e2 |= i2s_channel_init_std_mode(i2s_rx_, &std_cfg);
            if (i2s_tx_) e2 |= i2s_channel_enable(i2s_tx_);
            if (i2s_rx_) e2 |= i2s_channel_enable(i2s_rx_);
            i2s_ready_ = (e2 == ESP_OK);
            ESP_LOGW(TAG, "I2S init: new=%d cfg=%d ready=%d (BCLK=%d WS=%d OUT=%d IN=%d, %dHz)",
                     (int)e1, (int)e2, (int)i2s_ready_,
                     (int)AUDIO_I2S_GPIO_BCLK, (int)AUDIO_I2S_GPIO_WS,
                     (int)AUDIO_I2S_GPIO_DATA_OUT, (int)AUDIO_I2S_GPIO_DATA_IN,
                     output_sample_rate_);
        }
    }
#endif

#if ALPHAPI_ALERT_TEST
    /* 异步播放 alert.dat，绝不阻塞启动 */
    xTaskCreate(AlertPlaybackTask, "alertsnd", 4096, this, 3, nullptr);
#endif
    /* ===== ★★★ 2026-10-08 开机强制复位 N32 到播放模式 ★★★ =====
     * N32 有独立供电（板载电池 + 电源开关），ESP32 重启或刷机【不会】复位它。
     * 本板音频是半双工：N32 停在录音模式时，写入 0x15 的 PCM 会被忽略
     * ⇒ 表现为"设备完全没有声音"，但 app 侧的 calls/tx 计数照常增加
     *   （因为 Write() 只保证"写进 UART"，不代表 N32 在播）。
     * 这里在启动时显式发一次 0x10=0，把 N32 拉回播放模式。 */
    {
        uint8_t v_reset = 0;
        int acc = DispatchFrame(kCmdMicSwitch, &v_reset, 1);
        ESP_LOGI(TAG, "N32 boot reset: 0x10=0 accepted=%d", acc);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    SetHardwareVolume(output_volume_);
    ESP_LOGI(TAG, "AlphaPi One S audio ready (UART%d @%d, %d Hz mono 8bit)",
             static_cast<int>(uart_num_), baud_rate_, output_sample_rate_);
}

void AlphaPiOneAudioCodec::MarkPlaybackActivity()
{
    if (playback_idle_timer_ != nullptr) {
        esp_timer_stop(playback_idle_timer_);
        esp_timer_start_once(playback_idle_timer_, kPlaybackIdleMs * 1000);
    }
}
void AlphaPiOneAudioCodec::PlaybackIdleCallback(void* arg)
{
    auto* self = static_cast<AlphaPiOneAudioCodec*>(arg);
    self->playback_session_ = false;   /* 本次播放结束：下次 Write 会重发 WAV 头 */
}

/* ------------------------------ 输出通路 ------------------------------ */

void AlphaPiOneAudioCodec::SetHardwareVolume(int hw_volume)
{
    /* 出厂 set_speaker_volume：ClampD(v, 0, 6) → uart_write(20=0x14, bytearray([v])) */
    int v = (hw_volume * 6 + 50) / 100;
    if (v < 0) { v = 0; }
    if (v > 6) { v = 6; }
    uint8_t b = static_cast<uint8_t>(v);
    DispatchFrame(kCmdHwVolume, &b, 1);
}

void AlphaPiOneAudioCodec::SetOutputVolume(int volume)
{
    AudioCodec::SetOutputVolume(volume);   // 记录到 NVS
    SetHardwareVolume(volume);             // 0..100 直接对应手册的 0-100
}

void AlphaPiOneAudioCodec::SetMicStream(bool enable)
{
    /* ★★★ 出厂 __tickRecord 权威序列：
     *     开始录音 = uart_write(16 = 0x10, bytearray([2]))   ← 是 2，不是 1！
     *     停止录音 = uart_write(16 = 0x10, bytearray([0]))
     * 之前一直写 1/0，N32 永远停在播放态 ⇒ 0x11 读请求全部空手而归
     * （实测 req=6470 次、rx=0 字节）⇒ 唤醒词当然检测不到。 */
    if (!uart_ready_) {
        return;
    }
    /* ===== ALPHAPI_HALFDUPLEX (2026-10-06) =====
     * 本机音频是半双工：N32 内部只有一路 I2S 接 ADC/DAC，播放与录音互斥。
     * 出厂 controlBoardOneS 的 rec() 第一件事就是 p.play('record_start.dat')，
     * 即"确认处于播放已结束的静止状态"后才进入录音；它从不边播边录。
     * 而 application.cc 已被改成"不等播放排空就启动聆听"（那是为了让输入链路能起来），
     * 于是 EnableInput(true) 会在扬声器还在播提示音/问候语时发出录音命令
     * ⇒ N32 仍在 play 模式 ⇒ 0x11 吐回的是统计随机数据（实测平均|Δ|=82.2/255）。
     * 修复：先写 0x10=0 明确停止播放，留 200ms 让 N32 切换，再写 0x10=1 开始录音。
     *       （2026-11-09 更正：这里原来写的是"再写 0x10=2"，2 是错的，正确的是 1） */
    /* ===== ALPHAPI_MIC_OK (2026-10-07) —— 麦克风问题的真正答案 =====
     * 自测固件扫了 0x10 = 0..7 八个值，每个读 8 次 0x11 并统计相邻字节绝对差：
     *   cmd=0  av=0   全零        （停止/播放，对照）
     *   cmd=1  av=2   非零1200/1200  前4字节 7B 80 81 7E  ★★★ 真实音频！
     *   cmd=2  av=83  噪声        ← 我们之前一直发这个，所以永远是噪声
     *   cmd=3  av=0   常量 82 82 7F 7F
     *   cmd=4  av=0   常量 80 80 80 7F
     *   cmd=5  av=0   常量 80 80 81 80
     *   cmd=6  av=1   非零1200/1200  ★ 也是音频
     *   cmd=7  av=1   非零1200/1200  ★ 也是音频
     * 手册 §8.2 早就写明"0x10 AUDIO_CONTROL（1=录音，0=播放/停止）"，
     * 是我从出厂字节码里误读成 2 ⇒ 一直用错命令。
     * 结论：启动录音 = 写 1；停止 = 写 0。 */
    uint8_t v = 0;
    if (enable) {
        /* ★ 2026-10-18 官方逆向（EnableInput=0x401292D8）：官方没有
         *   "0x10=0 → 延时 → 0x10=1" 序列，也没有任何 vTaskDelay；
         *   它的顺序是：写 0x10=enable → 写 0x0C=2 → 检查 UART FIFO。
         *   0x0C 我们从未写过——疑似半双工/数据链控制，抄之。
         *   原来的 600ms 写保护延时 + 0x10=0 预写全部撤掉（实测无效且拖慢切换）。 */
        v = 1;                                  /* 1 = 录音 */
    }
    DispatchFrame(kCmdMicSwitch, &v, 1);
    if (enable) {
        /* 官方 EnableInput 独有命令：写 0x0C=2 */
        uint8_t link = 2;
        DispatchFrame(kCmdLinkControl, &link, 1);
    }

    /* 候选"输入增益"寄存器扫描已关闭（2026-11-09 定论：**N32 没有输入增益寄存器**）：
     * 实测 0x01/0x02/0x12/0x13/0x16~0x1D 共 12 个地址写 100 都不改变原始 PCM 电平；
     * 出厂固件把 0x00–0x20 全表读回，0x02–0x0F 全为 0。
     * 麦克风本身正常（静音 std≈1.0，说话 std 5–19），不需要放大。 */
    (void)GAIN_CAND_N;
    (void)g_gain_cand;

    /* ★★★ 2026-10-08 关键发现 ★★★
     * 手册 §8.2 写明 0x14 AUDIO_VOLUME 的音量量程是【0-100】，
     * 而本移植的 config.h 一直写成 0-6（照抄了别的板子的 AC101 量程），
     * 于是我们实际只把音量设成 0~6/100 ⇒ 几乎静音。
     * （注：2026-11-09 复核，`SetHardwareVolume` 仍保留 (v*6+50)/100 的压缩写法，
     *   因为出厂字节码 ClampD(v,0,6) 也是这个量级；这里直接写 100 是为了
     *   把 N32 侧音量开到最大，两者不冲突。） */
    if (enable) {
        uint8_t vol100 = 100;
        DispatchFrame(kCmdHwVolume, &vol100, 1);
    }
    mic_streaming_ = enable;
    if (enable) {
        /* 官方零延时；这里只留 20ms 让 0x10/0x0C 命令在 N32 侧生效后
         * 再开始 0x11 轮询（官方靠 FIFO 状态判断，我们保守一点）。 */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void AlphaPiOneAudioCodec::EnableInput(bool enable)
{
    g_alphapi_ena_in = g_alphapi_ena_in + 1;
    if (enable) { g_alphapi_ena_in_on = g_alphapi_ena_in_on + 1; }
    AudioCodec::EnableInput(enable);
    SetMicStream(enable);
}

void AlphaPiOneAudioCodec::EnableOutput(bool enable)
{
    if (enable) { g_alphapi_ena_out_on = g_alphapi_ena_out_on + 1; }
    AudioCodec::EnableOutput(enable);
    // 输出不需要单独的使能命令：0x15 写入即播放；播放结束由空闲计时器收敛。
    if (!enable) {
        playback_session_ = false;
        if (playback_idle_timer_ != nullptr) {
            esp_timer_stop(playback_idle_timer_);
        }
    }
}

/* 【已删除】SendWavHeader()。
 * 原实现会先发一个 46 字节 WAV 头（data 长度写 0xFFFFFFFF 表示"未知长度流式"）。
 * 2026-10 实测结论：**N32 收到这种头之后就不再出声**（它等不到声明的字节数）。
 * 正确做法是直接写 0x15 发裸 PCM，不发任何头。见 Write()。 */

int AlphaPiOneAudioCodec::Write(const int16_t* data, int samples)
{
    /* 音频经 UART 送 N32（寄存器 0x15），带流控。
     * 16bit 有符号 → 8bit 无符号；**不发 WAV 头**（发了会静音）。
     * 采样率由 config.h 的 AUDIO_OUTPUT_SAMPLE_RATE = 24000 决定。 */
    if (samples <= 0 || !uart_ready_) {
        return 0;
    }
    g_alphapi_pcm_calls = g_alphapi_pcm_calls + 1;
    g_alphapi_last_write_us = esp_timer_get_time();  /* 记录最近播放时刻（麦克风切换保护用） */
    MarkPlaybackActivity();   /* ★ 2026-10-18：重激活播放空闲计时器（此前从未被调用） */
    /* ★ 2026-10-16 修"出字幕没声音"（第二版）：
     * 原方案判 mic_streaming_，但下面 SetMicStream(false) 会先把它清掉，
     * 判断处永远读到 false ⇒ 录音→播放切换时仍不写 0x10=0 ⇒ N32 停在录音态静音。
     * 正确做法：进函数先记住"刚在录音"，再关录音流。 */
    bool was_recording = mic_streaming_;
    if (was_recording) {
        SetMicStream(false);          /* 内部写 0x10=0（enable=false 分支） */
        /* ★ 2026-10-16 修"出字幕没声音"（第三版）：
         * 录音刚停时 UART RX 里还残留 N32 的 0x81 麦克风帧，
         * 0x15 的 ack 读取会把它们误当响应 ⇒ WriteAudioFlow 判失败
         * ⇒ 重发循环永不清零 ⇒ 静音。切换必须先清 RX 缓冲。 */
        uart_flush_input(uart_num_);
        vTaskDelay(pdMS_TO_TICKS(200));   /* 出厂半双工切换等待 200ms */
        /* ★ 关键：同步清框架层的 input_enabled_。
         * audio_service 的 ReadAudioData() 只在 !codec_->input_enabled() 时才调
         * EnableInput(true)；我们这里已经把 N32 切到播放态（0x10=0），若不同步清标志，
         * TTS 播完回到聆听时框架认为麦克风还开着 ⇒ 永不重发 0x10=1 ⇒ 第二句没反应。 */
        AudioCodec::EnableInput(false);
    }
    /* 播放前确保处于播放态。playback_session_ 在空闲计时器到期后置 false，
     * 每次新播放会话都会重写 0x10=0；刚录完音则无条件重写。 */
    if (!playback_session_ || was_recording) {
        uint8_t zero = 0;
        DispatchFrame(kCmdMicSwitch, &zero, 1);
        vTaskDelay(pdMS_TO_TICKS(20));
        /* 录音期间 SetMicStream 写过 0x14=100，可能改变 N32 播放状态；
         * 播放会话开始时把硬件音量按当前音量重写一遍。 */
        SetHardwareVolume(output_volume_);
        playback_session_ = true;
    }
    static uint8_t pcm8[200];
    int done = 0;
    while (done < samples) {
        int n = samples - done;
        if (n > 200) { n = 200; }
        for (int i = 0; i < n; i++) {
            int v = data[done + i] >> 8;
            if (v < -128) { v = -128; }
            if (v > 127) { v = 127; }
            pcm8[i] = static_cast<uint8_t>(v + 128);
        }
        if (!WriteAudioFlow(kCmdPcmWrite, pcm8, static_cast<size_t>(n))) {
            break;
        }
        g_alphapi_audio_tx_bytes = g_alphapi_audio_tx_bytes + static_cast<unsigned>(n);
        done += n;
    }
    return done;
}

/* ------------------------------ 输入通路 ------------------------------ */

/* 麦克风：发 0x80 0x11 <size>，读回 0x81 0x11 <actual> <payload...> <checksum>
 * 8bit PCM -> s16: sample = (u8 - 128) * 256 */
int AlphaPiOneAudioCodec::Read(int16_t* dest, int samples)
{
    /* 麦克风：内部缓冲 + 按需向 N32 取 200 字节块（与出厂一致），
     * 这样每次 Read 都能拿到连续的 8bit 样本，转 16bit 交给上层。 */
    g_alphapi_mic_req_count = g_alphapi_mic_req_count + 1;
    if (samples <= 0 || !uart_ready_) {
        return 0;
    }
    /* ALPHAPI_MICVOL：每 20 次采样一次寄存器 0x12（麦克风音量），
     * 出厂 read_mic_volume() 就是 uart_read(18,1)[3]，>=40 表示检测到声音。 */
    /* ALPHAPI_MICVOL 诊断读已删除（2026-10-18 正式版）：
     * 每 20 次 Read 插一次 0x12 寄存器读会阻塞最长 100ms，
     * 干扰 0x11 连续取流节奏；诊断期已收官。 */
    if (false && (g_alphapi_mic_req_count % 20) == 1) {
        uint8_t mreq[3];
        mreq[0] = kMicStreamHdr;
        mreq[1] = 0x12;
        mreq[2] = 1;
        uart_write_bytes(uart_num_, reinterpret_cast<const char*>(mreq), sizeof(mreq));
        uint8_t mfram[8];
        memset(mfram, 0, sizeof(mfram));
        if (uart_read_bytes(uart_num_, mfram, 5, pdMS_TO_TICKS(100)) == 5 &&
            mfram[0] == kMicStreamResp && mfram[1] == 0x12) {
            unsigned v = mfram[3];
            g_alphapi_micvol_n = g_alphapi_micvol_n + 1;
            g_alphapi_micvol_last = v;
            if (v < g_alphapi_micvol_min) { g_alphapi_micvol_min = v; }
            if (v > g_alphapi_micvol_max) { g_alphapi_micvol_max = v; }
        }
    }
    int produced = 0;
    int guard = 0;
    while (produced < samples && guard++ < 8) {
        if (mic_take_ >= mic_fill_) {
            int got = RequestMicBlock();
            if (got <= 0) {
                /* ★ 2026-10-16 修"第二句没反应"：TTS 播放与聆听切换时，
                 * Write() 里 SetMicStream(false) 关掉了 N32 录音，但框架层
                 * input_enabled_ 仍是 true，之后没人再调 EnableInput(true)
                 * ⇒ Read() 每次都拿不到数据（框架认为麦克风开着）。
                 * 对策：连续多次取不到数据且本 codec 认为应在录音态时，
                 * 重新下发录音启动序列（0x10=0 -> 0x10=1），清残留后恢复。 */
                mic_stall_count_++;
                if (mic_stall_count_ >= 3 && mic_streaming_) {
                    mic_stall_count_ = 0;
                    uart_flush_input(uart_num_);
                    uint8_t zero = 0;
                    DispatchFrame(kCmdMicSwitch, &zero, 1);
                    vTaskDelay(pdMS_TO_TICKS(50));
                    uint8_t one = 1;
                    DispatchFrame(kCmdMicSwitch, &one, 1);
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                break;
            }
            mic_stall_count_ = 0;
            mic_fill_ = got;
            mic_take_ = 0;
            /* 2026-10-09 正式版：删除全部麦克风 dump/嗅探 flash 写入。
             * 诊断期已收官；这些写入每 4KB 擦一次 ota_1 尾部（0x2E0000/0x280000），
             * 有擦写损耗、拖慢 Read 循环（曾观察到 session 内 bl 激增与它相关）。
             * mic_dump_ 缓冲保留（供 ASAN/断言类检查），不再写 flash。 */
            (void)got;
        }
        /* ALPHAPI_ADPCM：改动点总开关
 * 2026-10-06 实测：寄存器 0x12（麦克风音量）随说话 5→255 剧变 ⇒ 麦克风确实在拾音。
 * 但 0x11 读回的 8bit 数据相邻样本几乎不相关（差=20894/65536）⇒ 不是线性 PCM。
 * 手册 §6.1：.dat 音效是 ADPCM 编码；出厂 playAddr=22(0x16)=ADPCM。
 * 故改读 0x16。 */
/* ALPHAPI_MIC_16BIT = 0 ⇒ 走 8bit 分支（正确）。
 * 2026-11-09 定论：N32 的 0x11 在 0x10=1 模式下返回**8bit 无符号 PCM**，
 * 用 (u8-128)<<8 扩展即可，不需要 <<20（那是把噪声削成方波的历史错误），
 * 也不需要按 16bit 解析（16BIT=1 期间用户始终"没反应"）。 */
#define ALPHAPI_MIC_16BIT 0
#if ALPHAPI_MIC_16BIT
        /* ALPHAPI_MIC_16BIT：按 16bit 小端【有符号】解析。
         * 依据：8bit 解析出来的波形是 std=70、过零率≈0.5、满量程 ±128，
         * 这正是"把 16bit 数据当 8bit 读"的典型特征（相当于对真实样本的
         * 高/低字节交替取值）。改用 int16 后每个样本占 2 字节。 */
#if ALPHAPI_IMA
        /* IMA ADPCM：每字节 2 个 4bit 样本（高 nibble 在前） */
        while (mic_take_ < mic_fill_ && produced + 1 < samples) {
            uint8_t b = mic_pcm_[mic_take_++];
            dest[produced++] = static_cast<int16_t>(ImaDecodeNibble((b >> 4) & 0x0F));
            if (produced < samples) {
                dest[produced++] = static_cast<int16_t>(ImaDecodeNibble(b & 0x0F));
            }
        }
#else
        while (mic_take_ + 1 < mic_fill_ && produced < samples) {
            uint16_t lo = mic_pcm_[mic_take_];
            uint16_t hi = mic_pcm_[mic_take_ + 1];
            dest[produced++] = static_cast<int16_t>(lo | (hi << 8));
            mic_take_ += 2;
        }
        /* 落单的半个样本丢掉，保持 2 字节对齐 */
        if (mic_take_ + 1 >= mic_fill_) {
            mic_take_ = mic_fill_;
        }
#endif
#else
        /* ===== 8bit 无符号 PCM -> int16：标准 8→16 位扩展 =====
         *
         * 【2026-11-04 实测依据】用出厂固件 REPL 直接抓 N32 0x11 原始流：
         *   静音 2 秒：std=1.00  min=124 max=132
         *   说话 3 秒：静音段 std≈0.9 → 说话段 std 5.3/18.9/11.1/12.9/7.7/5.2
         *              min=34 max=229，相邻样本平均差 1.44，
         *              20ms 包络峰值 45.3、4.2 Hz 音节调制（语音特征）
         * ⇒ 麦克风硬件完全正常，8bit 样本就是标准 8bit PCM。
         *   (u8-128)<<8 是教科书式 8→16 位扩展：满量程 ±127 → ±32512，
         *   永不削波；静音底噪 std≈0.9*256≈230 (-43dBFS)。
         *
         * 历史教训：早期一直发 0x10=2（N32 返回垃圾流，std≈70 均匀随机），
         *   所以以为"电平只有 std 1.4"，把数字增益一路加到 <<20 硬削成方波。
         *   改用 0x10=1（正确录音命令）之后从未重新验证过标准扩展。
         */
        while (mic_take_ < mic_fill_ && produced < samples) {
            dest[produced++] = static_cast<int16_t>(
                static_cast<int16_t>(static_cast<int16_t>(mic_pcm_[mic_take_]) - 128) << 8);
            mic_take_++;
        }
#endif
    }
    if (produced > 0) {
        g_alphapi_audio_rx_bytes = g_alphapi_audio_rx_bytes + static_cast<unsigned>(produced);
    }
    return produced;
}
/* 2026-10-09 正式版备注：Read() 的 flash 诊断写入已全部移除；
 * ALPHAPI_MIC_SELFTEST / MicSelfTestInline / MicTest* 自测代码保留在
 * 文件前部，宏关闭时不参与运行（ALPHAPI_MIC_SELFTEST=0）。*/
