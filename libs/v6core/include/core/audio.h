#pragma once

#include <atomic>
#include <array>
#include <vector>
#include "core/timer_i8253.h"
#include "core/sound_ay8910.h"

namespace dev
{
    class Audio
    {
    private:
        static constexpr int INPUT_RATE = 1500000; // 1.5 MHz timer
        static constexpr int OUTPUT_RATE = 50000; // 50 KHz
        static constexpr int DOWNSAMPLE_RATE = INPUT_RATE / OUTPUT_RATE;
        static constexpr int BUFFER_SIZE = 4000; // ring buffer capacity in samples

        TimerI8253& m_timer;
        AYWrapper& m_aywrapper;
        float m_muteMul = 1.0f;

        std::array<float, BUFFER_SIZE> m_buffer;
        std::atomic_uint64_t m_readBuffIdx = 0;
        std::atomic_uint64_t m_writeBuffIdx = 0;

        std::atomic_bool m_inited = false;
        std::atomic_int m_downsampleRate = DOWNSAMPLE_RATE;

        bool Downsample(float& _sample);

    public:
        Audio(TimerI8253& _timer, AYWrapper& _aywrapper);
        ~Audio() = default;
        void Init();
        void Pause(bool _pause);
        void Mute(const bool _mute);
        void Clock(int _cycles, const float _beeper);
        void Reset();

        // Read up to _maxSamples from the ring buffer into _out.
        // Returns the number of samples actually read.
        int ReadSamples(float* _out, int _maxSamples);

        int GetOutputRate() const { return OUTPUT_RATE; }
        int GetAvailableSamples() const { return static_cast<int>(m_writeBuffIdx - m_readBuffIdx); }
    };

}