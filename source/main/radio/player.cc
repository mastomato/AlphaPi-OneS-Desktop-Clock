#include "player.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <vector>

#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "pcm_sink.h"
#include "audio_resampler.h"
#include <memory>
#include "board_config.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "stations.h"
#include "station_store.h"
#include "board.h"


namespace radio {
namespace {

constexpr char kTag[] = "Player";

/*
 * 2.4.2 版本这里查的是收音机自带的 Wifi 单例；2.5.0 的联网由 WifiBoard 管，
 * 没有对外暴露「当前是否已连上」的公共接口。收音机的拉流本来就带失败重连
 * （PumpStream 出错后按退避重试），所以这里不再预判链路，直接让 HTTP 去尝试：
 * 网络没起来时表现为连接失败并重试，行为与原来等价，只是少一次预检。
 */
bool NetworkUp() { return true; }

constexpr size_t kNetworkRead = 2048;
// 64 KB -> 32 KB：本板内部堆只有 8~15 KB，缓冲必须收紧
constexpr size_t kCompressedCapacity = 16 * 1024;
constexpr size_t kDecodeInput = 8192;
constexpr size_t kDecodeOutput = 12288;
// Eight seconds of PCM absorb source stalls; retain fast 1.5-second startup
// and accumulate the extra reserve whenever the source sends a burst.
// and 480 samples (20 ms) per blocking UART write. ACKs pace the UART; there is
// no additional sleep after a successful write in the working driver.
// 8 秒 -> 2 秒：8 秒需要 384 KB，进收音机时会把堆吃干，随后 TLS 重试就崩
constexpr size_t kPcmCapacity = kOutputSampleRate;
constexpr size_t kPrimeSamples = kOutputSampleRate * 3 / 2;
constexpr size_t kPrimeFallbackSamples = kOutputSampleRate / 2;
constexpr int64_t kPrimeTimeoutUs = 5 * 1000 * 1000;
constexpr int64_t kFeedWaitUs = 150 * 1000;
constexpr size_t kChunkSamples = 480;

struct StreamHeaders {
    int metaint = 0;
    bool wrong_type = false;
};
esp_err_t OnStreamHeader(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    auto* headers = static_cast<StreamHeaders*>(event->user_data);
    if (strcasecmp(event->header_key, "icy-metaint") == 0) {
        headers->metaint = std::max(0, std::atoi(event->header_value));
    } else if (strcasecmp(event->header_key, "Content-Type") == 0) {
        headers->wrong_type = strncasecmp(event->header_value, "text/", 5) == 0 ||
                             std::strstr(event->header_value, "mpegurl") != nullptr;
    }
    return ESP_OK;
}

}  // namespace

Player& Player::Instance() {
    static Player instance;
    return instance;
}

bool Player::Init() {
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    compressed_queue_.Init(kCompressedCapacity);
    pcm_queue_.Init(kPcmCapacity);
    if (xTaskCreate(TaskEntry, "radio_net", 6144, this, 5, nullptr) != pdPASS ||
        xTaskCreate(DecodeEntry, "radio_decode", 8192, this, 6, nullptr) != pdPASS ||
        xTaskCreate(FeedEntry, "radio_feed", 4096, this, 7, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "task create failed");
        return false;
    }
    return true;
}

PlayerStatus Player::StatusSnapshot() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return {state_.load(), detail_, state_revision_.load()};
}

PlayerDiagnostics Player::Diagnostics() {
    PlayerDiagnostics result{};
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        result.pcm_ms = pcm_queue_.size() * 1000 / kOutputSampleRate;
        result.compressed_bytes = compressed_queue_.size();
    }
    result.received_bytes = received_bytes_.load();
    result.decoded_samples = decoded_samples_.load();
    result.decode_work_us = decode_work_us_.load();
    result.decode_max_us = decode_max_us_.load();
    result.underruns = underruns_.load();
    result.uart_failures = uart_failures_.load();
    result.connections = connections_.load();
    result.sample_rate = sample_rate_.load();
    const uint32_t last_rx = last_rx_ms_.load();
    result.last_rx_age_ms = last_rx ? uint32_t(esp_timer_get_time() / 1000) - last_rx : 0;
    return result;
}

void Player::SetState(PlayerState state, const char* detail, uint32_t generation) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (generation_.load() != generation) {
        return;
    }
    const std::string text = detail == nullptr ? std::string() : std::string(detail);
    if (state_.load() == state && detail_ == text) {
        return;
    }
    state_ = state;
    detail_ = text;
    // Audio/network tasks must never wait for LVGL or issue UI callbacks.
    // The control task consumes the latest snapshot at its own pace.
    state_revision_.fetch_add(1);
}

void Player::Play(size_t station_index) {
    if (StationCount() == 0) {
        return;
    }
    uint32_t generation;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        station_ = station_index % StationCount();
        playing_ = true;
        generation = generation_.fetch_add(1) + 1;
        compressed_queue_.Clear();
        pcm_queue_.Clear();
    }
    SetState(PlayerState::kBuffering, "连接中", generation);
}

void Player::Stop() {
    uint32_t generation;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        playing_ = false;
        generation = generation_.fetch_add(1) + 1;
        compressed_queue_.Clear();
        pcm_queue_.Clear();
    }
    // Feed observes the generation and stops its UART session independently.
    // Do not block the button/control task while waiting for an ACK.
    SetState(PlayerState::kPaused, nullptr, generation);
}

void Player::Toggle() {
    if (playing_) {
        Stop();
    } else {
        Play(station_.load());
    }
}

bool Player::Active(uint32_t generation) const {
    return playing_.load() && generation_.load() == generation;
}

bool Player::PushCompressed(const uint8_t* data, size_t count, uint32_t generation) {
    size_t sent = 0;
    while (sent < count) {
        size_t n;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!Active(generation)) {
                return false;
            }
            n = compressed_queue_.Push(data + sent, count - sent);
        }
        sent += n;
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    return true;
}

bool Player::PushSamples(const int16_t* data, size_t count, uint32_t generation) {
    size_t sent = 0;
    while (sent < count) {
        size_t n;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!Active(generation)) {
                return false;
            }
            n = pcm_queue_.Push(data + sent, count - sent);
        }
        sent += n;
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    return true;
}

void Player::TaskEntry(void* arg) { static_cast<Player*>(arg)->Run(); }
void Player::DecodeEntry(void* arg) { static_cast<Player*>(arg)->Decode(); }
void Player::FeedEntry(void* arg) { static_cast<Player*>(arg)->Feed(); }

void Player::Run() {
    for (;;) {
        if (!playing_) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        const uint32_t generation = generation_.load();
        if (!NetworkUp()) {
            SetState(PlayerState::kBuffering, "等待网络", generation);
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        PumpStream(generation, station_.load());
        if (Active(generation)) {
            // Drain existing audio while reconnecting. Decoder and UART keep
            // their session until the PCM queue actually runs out of data.
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void Player::PumpStream(uint32_t generation, size_t index) {
    const Station station = StationAt(index);
    ESP_LOGI(kTag, "tuning %s -> %s", station.name.c_str(), station.url.c_str());
    if (state_.load() != PlayerState::kPlaying) {
        SetState(PlayerState::kBuffering, "连接中", generation);
    }
    network_finished_ = 0;
    connections_.fetch_add(1);

    esp_http_client_config_t cfg = {};
    StreamHeaders headers;
    cfg.url = station.url.c_str();
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = OnStreamHeader;
    cfg.user_data = &headers;
    cfg.timeout_ms = 7000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.keep_alive_enable = true;
    cfg.user_agent = "alphapi-radio/1.0";
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        SetState(PlayerState::kError, "HTTP 初始化失败", generation);
        network_finished_ = generation;
        return;
    }
    esp_http_client_set_header(client, "Icy-MetaData", "0");
    bool opened = false;
    bool valid = false;
    int http_status = 0;
    for (int redirect = 0; redirect <= 4 && Active(generation); ++redirect) {
        headers = {};
        opened = esp_http_client_open(client, 0) == ESP_OK;
        if (!opened) break;
        // Live streams need not have Content-Length. Only the HTTP status
        // determines whether the connection succeeded.
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        http_status = status;
        valid = status >= 200 && status < 300 && !headers.wrong_type;
        if (valid || status < 300 || status >= 400) break;
        if (esp_http_client_set_redirection(client) != ESP_OK) break;
        esp_http_client_close(client);
        opened = false;
    }
    if (!valid) {
        if (opened) {
            esp_http_client_close(client);
        }
        esp_http_client_cleanup(client);
        network_finished_ = generation;
        if (state_.load() != PlayerState::kPlaying) {
            if (http_status >= 400) {
                const std::string detail = "HTTP " + std::to_string(http_status);
                SetState(PlayerState::kError, detail.c_str(), generation);
            } else {
                SetState(PlayerState::kError, opened ? "无响应" : "连接失败", generation);
            }
        }
        return;
    }

    const int metaint = headers.metaint;
    // Connect/headers retain the longer deadline. Body reads use short waits
    // so a station change does not wait seven seconds on the previous stream.
    // EAGAIN is temporary starvation, not a reason to tear down the socket.
    esp_http_client_set_timeout_ms(client, 500);
    int64_t last_progress_us = esp_timer_get_time();
    auto read_stream = [&](uint8_t* target, int want) {
        while (Active(generation) && NetworkUp()) {
            const int n = esp_http_client_read(client, reinterpret_cast<char*>(target), want);
            if (n > 0) { last_progress_us = esp_timer_get_time(); return n; }
            if (n != -ESP_ERR_HTTP_EAGAIN &&
                (n < 0 || esp_http_client_is_complete_data_received(client))) return n;
            if (esp_timer_get_time() - last_progress_us >= 7 * 1000000LL) return -1;
            vTaskDelay(1);
        }
        return -1;
    };
    size_t countdown = metaint > 0 ? static_cast<size_t>(metaint) : 0;
    std::array<uint8_t, kNetworkRead> bytes = {};
    while (Active(generation) && NetworkUp()) {
        const int want = static_cast<int>(metaint > 0 ? std::min(bytes.size(), countdown)
                                                     : bytes.size());
        const int got = read_stream(bytes.data(), want);
        if (!Active(generation)) {
            break;
        }
        if (got <= 0) break;
        received_bytes_.fetch_add(static_cast<uint32_t>(got));
        last_rx_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        if (!PushCompressed(bytes.data(), static_cast<size_t>(got), generation)) {
            break;
        }
        if (metaint > 0) {
            countdown -= static_cast<size_t>(got);
            if (countdown == 0) {
                uint8_t length = 0;
                if (read_stream(&length, 1) != 1) {
                    break;
                }
                size_t left = static_cast<size_t>(length) * 16;
                while (left > 0 && Active(generation)) {
                    const int n = read_stream(bytes.data(), static_cast<int>(std::min(left, bytes.size())));
                    if (n <= 0) {
                        break;
                    }
                    left -= static_cast<size_t>(n);
                }
                if (left > 0) {
                    break;
                }
                countdown = static_cast<size_t>(metaint);
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    network_finished_ = generation;
}

void Player::Decode() {
    std::vector<uint8_t> raw(kDecodeInput);
    std::vector<uint8_t> pcm(kDecodeOutput);
    std::vector<int16_t> mono(kDecodeOutput / sizeof(int16_t));
    std::vector<int16_t> resampled;
    // Keep the FIR working set in fast internal RAM when available (6.4 KiB).
    // Large PCM/network queues still use PSRAM.
    void* memory = heap_caps_malloc(sizeof(AudioResampler), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory) memory = heap_caps_malloc(sizeof(AudioResampler), MALLOC_CAP_8BIT);
    if (!memory) { ESP_LOGE(kTag, "resampler allocation failed"); vTaskDelete(nullptr); return; }
    auto destroy = [](AudioResampler* p) { p->~AudioResampler(); heap_caps_free(p); };
    std::unique_ptr<AudioResampler, decltype(destroy)> resampler(new(memory) AudioResampler(), destroy);
    esp_audio_simple_dec_handle_t decoder = nullptr;
    uint32_t active_generation = 0;
    uint32_t previous_rate = 0;
    size_t buffered = 0;
    resampled.reserve(kDecodeOutput / sizeof(int16_t));

    for (;;) {
        const uint32_t generation = generation_.load();
        if (generation != active_generation) {
            active_generation = generation;
            buffered = 0;
            previous_rate = 0;
            if (decoder != nullptr) {
                esp_audio_simple_dec_close(decoder);
                decoder = nullptr;
            }
        }
        if (!Active(generation)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (decoder == nullptr) {
            esp_audio_simple_dec_cfg_t cfg = {};
            cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
            cfg.use_frame_dec = false;
            if (esp_audio_simple_dec_open(&cfg, &decoder) != ESP_AUDIO_ERR_OK) {
                SetState(PlayerState::kError, "解码器异常", generation);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }
        size_t added = 0;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (Active(generation)) {
                added = compressed_queue_.Pop(raw.data() + buffered, raw.size() - buffered);
            }
        }
        buffered += added;
        if (!Active(generation)) {
            continue;
        }
        if (buffered == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        const int64_t work_started = esp_timer_get_time();
        esp_audio_simple_dec_raw_t in = {};
        in.buffer = raw.data();
        in.len = static_cast<uint32_t>(buffered);
        esp_audio_simple_dec_out_t out = {};
        out.buffer = pcm.data();
        out.len = static_cast<uint32_t>(pcm.size());
        const esp_audio_err_t result = esp_audio_simple_dec_process(decoder, &in, &out);
        if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > pcm.size()) {
            pcm.resize(out.needed_size);
            continue;
        }
        if (in.consumed > 0 && in.consumed <= buffered) {
            buffered -= in.consumed;
            std::memmove(raw.data(), raw.data() + in.consumed, buffered);
        }
        if (out.decoded_size == 0) {
            if (in.consumed == 0) {
                if (buffered == raw.size()) {
                    buffered = 0;
                }
                // Yield to the lower-priority HTTP task while awaiting bytes.
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            continue;
        }
        if (!Active(generation)) {
            continue;
        }
        esp_audio_simple_dec_info_t info = {};
        if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK ||
            info.bits_per_sample != 16 || info.channel == 0 || info.channel > 2 ||
            info.sample_rate == 0) {
            continue;
        }
        const auto* samples = reinterpret_cast<const int16_t*>(pcm.data());
        const size_t frames = out.decoded_size / sizeof(int16_t) / info.channel;
        if (frames == 0) {
            continue;
        }
        if (mono.size() < frames) {
            mono.resize(frames);
        }
        if (info.channel == 1) {
            std::memcpy(mono.data(), samples, frames * sizeof(int16_t));
        } else {
            for (size_t i = 0; i < frames; ++i) {
                mono[i] = static_cast<int16_t>((static_cast<int32_t>(samples[i * 2]) +
                                               samples[i * 2 + 1]) / 2);
            }
        }
        if (previous_rate != info.sample_rate) {
            previous_rate = info.sample_rate;
            sample_rate_ = info.sample_rate;
            resampler->Reset(info.sample_rate, kOutputSampleRate, [] { vTaskDelay(1); });
        }
        const int16_t* feed = mono.data();
        size_t feed_frames = frames;
        if (info.sample_rate != static_cast<uint32_t>(kOutputSampleRate)) {
            resampler->Process(mono.data(), frames, resampled);
            feed = resampled.data();
            feed_frames = resampled.size();
        }
        const uint32_t work_us = static_cast<uint32_t>(esp_timer_get_time() - work_started);
        decode_work_us_.fetch_add(work_us);
        if (work_us > decode_max_us_.load()) decode_max_us_ = work_us;
        decoded_samples_.fetch_add(static_cast<uint32_t>(feed_frames));
        PushSamples(feed, feed_frames, generation);
        // The decoder is above the UI in priority. Don't monopolize the single
        // core while filling the PCM reserve from already cached data.
        vTaskDelay(1);
    }
}

void Player::Feed() {
    std::array<int16_t, kChunkSamples> block = {};
    auto& audio = PcmSink::Instance();
    uint32_t active_generation = 0;
    bool primed = false;
    int64_t prime_started_us = esp_timer_get_time();
    int64_t empty_since_us = 0;

    for (;;) {
        const uint32_t generation = generation_.load();
        if (generation != active_generation) {
            active_generation = generation;
            audio.StopRecord();
            audio.InvalidateStream();
            primed = false;
            prime_started_us = esp_timer_get_time();
            empty_since_us = 0;
        }
        // The UART owner applies volume between PCM blocks, also while paused
        // or buffering. A failed ACK leaves the latest request pending.
        if (!audio.ApplyPendingVolume()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!Active(generation)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t available;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            available = pcm_queue_.size();
        }
        const int64_t now = esp_timer_get_time();
        if (!primed) {
            if (available == 0) {
                prime_started_us = now;
            }
            const size_t target = now - prime_started_us >= kPrimeTimeoutUs
                                      ? kPrimeFallbackSamples : kPrimeSamples;
            if (available < target) {
                // Keep actionable connection errors until the network retries.
                if (state_.load() != PlayerState::kError || available > 0) {
                    SetState(PlayerState::kBuffering,
                             network_finished_.load() == generation ? "重连中" : "缓冲中",
                             generation);
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            audio.InvalidateStream();
            primed = true;
            empty_since_us = 0;
        }

        size_t count;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            count = Active(generation) ? pcm_queue_.Pop(block.data(), block.size()) : 0;
        }
        if (!Active(generation)) {
            continue;
        }
        if (count == 0) {
            if (empty_since_us == 0) {
                empty_since_us = now;
            } else if (now - empty_since_us >= kFeedWaitUs) {
                underruns_.fetch_add(1);
                primed = false;
                prime_started_us = now;
                audio.StopRecord();
                audio.InvalidateStream();
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        empty_since_us = 0;

        size_t sent = 0;
        bool failed = false;
        while (sent < count && Active(generation)) {
            const int written = audio.Write(block.data() + sent, static_cast<int>(count - sent));
            if (written <= 0) {
                failed = true;
                break;
            }
            sent += static_cast<size_t>(written);
        }
        if (!Active(generation)) {
            continue;
        }
        if (failed) {
            uart_failures_.fetch_add(1);
            audio.StopRecord();
            audio.InvalidateStream();
            primed = false;
            prime_started_us = esp_timer_get_time();
            SetState(PlayerState::kBuffering, "重连中", generation);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        // Announce playback only after PCM was accepted, not after an HTTP read.
        SetState(PlayerState::kPlaying, nullptr, generation);
    }
}

}  // namespace radio
