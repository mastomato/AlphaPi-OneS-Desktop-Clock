#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "stream_queue.h"

namespace radio {

enum class PlayerState {
    kIdle,
    kBuffering,
    kPlaying,
    kPaused,
    kError,
};

struct PlayerStatus {
    PlayerState state;
    std::string detail;
    uint32_t revision;
};

struct PlayerDiagnostics {
    uint32_t pcm_ms, compressed_bytes, received_bytes, decoded_samples;
    uint32_t decode_work_us, decode_max_us, underruns, uart_failures, connections;
    uint32_t sample_rate, last_rx_age_ms;
};

// Independent HTTP, MP3 decoder and UART tasks, with bounded queues between
// them. Generations reject old data after a station change.
class Player {
public:
    static Player& Instance();
    bool Init();
    PlayerStatus StatusSnapshot();
    PlayerDiagnostics Diagnostics();
    uint32_t state_revision() const { return state_revision_.load(); }
    void Play(size_t station_index);
    void Stop();
    void Toggle();

    bool playing() const { return playing_.load(); }
    PlayerState state() const { return state_.load(); }
    size_t station_index() const { return station_.load(); }

private:
    Player() = default;
    static void TaskEntry(void* arg);
    static void DecodeEntry(void* arg);
    static void FeedEntry(void* arg);
    void Run();
    void Decode();
    void Feed();
    void PumpStream(uint32_t generation, size_t station);
    bool Active(uint32_t generation) const;
    bool PushCompressed(const uint8_t* data, size_t count, uint32_t generation);
    bool PushSamples(const int16_t* data, size_t count, uint32_t generation);
    void SetState(PlayerState state, const char* detail, uint32_t generation);

    std::mutex queue_mutex_;
    StreamQueue<uint8_t> compressed_queue_;
    StreamQueue<int16_t> pcm_queue_;
    std::atomic<bool> playing_{false};
    std::atomic<size_t> station_{0};
    std::atomic<uint32_t> generation_{0};
    std::atomic<uint32_t> network_finished_{0};

    std::atomic<uint32_t> received_bytes_{0}, decoded_samples_{0};
    std::atomic<uint32_t> decode_work_us_{0}, decode_max_us_{0};
    std::atomic<uint32_t> underruns_{0}, uart_failures_{0}, connections_{0};
    std::atomic<uint32_t> sample_rate_{0}, last_rx_ms_{0};

    std::mutex state_mutex_;
    std::atomic<PlayerState> state_{PlayerState::kIdle};
    std::string detail_;
    std::atomic<uint32_t> state_revision_{0};
};

}  // namespace radio
