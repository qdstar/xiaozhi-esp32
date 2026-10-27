#ifndef LXP_LD2450_H_
#define LXP_LD2450_H_

#include <cstdint>

/** HLK-LD2450 millimetre-wave radar (UART target report frames). */
class LxpLd2450 {
public:
    struct Reading {
        bool present = false;
        int nearest_mm = 0;
        int speed_mm_s = 0;
        int x_mm = 0;
        int y_mm = 0;
    };

    static LxpLd2450& GetInstance();

    /** Install UART + start RX. Safe to call once. */
    bool Start();

    /** Drain UART, parse frames, apply presence hold. Call every 100–200 ms. */
    Reading Poll();

    /** RX / frame counters for wiring diagnosis. */
    void GetDiag(uint32_t* rx_bytes, uint32_t* frames_ok, uint32_t* frames_bad);

private:
    LxpLd2450() = default;

    bool started_ = false;
    int64_t last_present_us_ = 0;
    Reading last_{};

    static constexpr int kFrameLen = 30;
    static constexpr int kRxBuf = 512;
    uint8_t accum_[kRxBuf] = {};
    int accum_len_ = 0;

    void Feed(const uint8_t* data, int len);
    bool TryParseOneFrame();
};

#endif  // LXP_LD2450_H_
