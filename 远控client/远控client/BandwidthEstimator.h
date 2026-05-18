#pragma once
#include <atomic>
#include <thread>
#include <functional>

class BandwidthEstimator {
public:
    explicit BandwidthEstimator(std::function<void(int)> onBitrateChange)
        : m_onBitrateChange(std::move(onBitrateChange)) {}

    ~BandwidthEstimator() { Stop(); }

    void OnBytesReceived(int bytes) {
        m_bytesAccum.fetch_add(bytes, std::memory_order_relaxed);
    }

    void Start() {
        m_running = true;
        m_thread = std::thread(&BandwidthEstimator::Loop, this);
    }

    void Stop() {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
    }

private:
    static constexpr int INIT_BITRATE = 4'000'000;
    static constexpr int MAX_BITRATE  = 8'000'000;
    static constexpr int MIN_BITRATE  = 500'000;

    std::function<void(int)>  m_onBitrateChange;
    std::atomic<int64_t>      m_bytesAccum{0};
    std::atomic<bool>         m_running{false};
    std::thread               m_thread;

    void Loop() {
        int currentBitrate = INIT_BITRATE;
        int upCount = 0;

        m_onBitrateChange(currentBitrate);

        while (m_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            int64_t bytes = m_bytesAccum.exchange(0, std::memory_order_relaxed);
            int64_t throughputBps = bytes * 8;

            if (throughputBps < 6) {
                // 网络几乎断流，立即降码率
                upCount = 0;
                currentBitrate = (std::max)(MIN_BITRATE,
                    (int)(currentBitrate * 0.7));
                m_onBitrateChange(currentBitrate);
            } else if (throughputBps > (int64_t)(currentBitrate * 0.85)) {
                upCount++;
                if (upCount >= 3) {
                    upCount = 0;
                    currentBitrate = (std::min)(MAX_BITRATE,
                        (int)(currentBitrate * 1.2));
                    m_onBitrateChange(currentBitrate);
                }
            } else {
                upCount = 0;
            }
        }
    }
};
