#pragma once

#include <JuceHeader.h>

#include <vector>

namespace ceilingIO::dsp
{
    enum class LimiterMode
    {
        Independent, // per-channel gain reduction (stereo default — bit-identical to the inline limiter)
        Linked       // min gain reduction across all channels, applied uniformly (Atmos — preserves spatial image)
    };

    // Lookahead brickwall limiter with a monotonic-queue sliding-max peak
    // detector and per-channel delay buffers. Extracted verbatim from
    // MainAudioProcessor; Independent mode reproduces the original per-channel
    // loop exactly. Linked mode is a new path used only by the Atmos renderer.
    class LookaheadLimiter
    {
    public:
        void prepare (double sampleRate, int samplesPerBlock, int numChannels);
        void setCeilingDbtp (float ceilingDbtp) noexcept { ceilingDbtp_ = ceilingDbtp; }
        void setDriveGain (float linearGain) noexcept { driveGain_ = linearGain; }
        void setMode (LimiterMode mode) noexcept { mode_ = mode; }

        void process (juce::AudioBuffer<float>& buffer);

    private:
        // Sliding-window maximum via a monotonic deque (preallocated, no allocs
        // in process()). Verbatim from the former MainAudioProcessor::MonotonicQueue.
        struct MonotonicQueue
        {
            std::vector<float> vals;
            std::vector<long long> idxs;
            int head = 0;
            int size = 0;
            int capacity = 0;

            void init (int cap)
            {
                capacity = cap;
                vals.assign ((size_t) cap, 0.0f);
                idxs.assign ((size_t) cap, 0);
                head = 0;
                size = 0;
            }

            inline bool empty() const noexcept { return size == 0; }
            inline float back_val() const noexcept { return vals[(head + size - 1) % capacity]; }
            inline long long back_idx() const noexcept { return idxs[(head + size - 1) % capacity]; }
            inline float front_val() const noexcept { return vals[head]; }
            inline long long front_idx() const noexcept { return idxs[head]; }

            inline void pop_back() noexcept { if (size > 0) --size; }
            inline void pop_front() noexcept { if (size > 0) { head = (head + 1) % capacity; --size; } }
            inline void push_back (long long idx, float v) noexcept
            {
                vals[(head + size) % capacity] = v;
                idxs[(head + size) % capacity] = idx;
                ++size;
            }
        };

        std::vector<std::vector<float>> delayBuffers_;
        std::vector<int> writeIndex_;
        std::vector<int> readIndex_;
        std::vector<MonotonicQueue> queues_;
        std::vector<long long> sampleCounters_;
        float ceilingDbtp_ = -1.0f;
        float driveGain_ = 1.0f;
        LimiterMode mode_ = LimiterMode::Independent;
        int lookaheadSamples_ = 0;
        int bufferLen_ = 0;
    };
} // namespace ceilingIO::dsp