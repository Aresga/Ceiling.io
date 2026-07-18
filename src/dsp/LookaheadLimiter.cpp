#include "dsp/LookaheadLimiter.h"

#include <algorithm>
#include <cmath>

namespace ceilingIO::dsp
{
    void LookaheadLimiter::prepare (double sampleRate, int samplesPerBlock, int numChannels)
    {
        // Verbatim from MainAudioProcessor::prepareToPlay (limiter section).
        lookaheadSamples_ = std::max (1, static_cast<int> (std::floor (0.005 * sampleRate)));
        bufferLen_ = lookaheadSamples_ + samplesPerBlock + 4;

        delayBuffers_.clear();
        delayBuffers_.resize ((size_t) numChannels);
        writeIndex_.assign ((size_t) numChannels, 0);
        readIndex_.assign ((size_t) numChannels, 0);

        for (size_t ch = 0; ch < delayBuffers_.size(); ++ch)
        {
            delayBuffers_[ch].assign ((size_t) bufferLen_, 0.0f);
            writeIndex_[ch] = 0;
            readIndex_[ch] = (writeIndex_[ch] + bufferLen_ - lookaheadSamples_) % bufferLen_;
        }

        queues_.clear();
        queues_.resize ((size_t) numChannels);
        sampleCounters_.assign ((size_t) numChannels, 0);
        const int mqCap = lookaheadSamples_ + 4;
        for (size_t ch = 0; ch < queues_.size(); ++ch)
            queues_[ch].init (mqCap);
    }

    void LookaheadLimiter::process (juce::AudioBuffer<float>& buffer)
    {
        const int numChannels = buffer.getNumChannels();
        const int numSamples  = buffer.getNumSamples();

        if (numChannels <= 0 || numSamples <= 0)
            return;

        const float linearCeiling = juce::Decibels::decibelsToGain (ceilingDbtp_);

        if (mode_ == LimiterMode::Independent)
        {
            // Verbatim per-channel loop from the former MainAudioProcessor::processBlock.
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* channelData = buffer.getWritePointer (ch);
                auto& delayBuf = delayBuffers_[(size_t) ch];
                int writeIdx = writeIndex_[(size_t) ch];
                int readIdx = readIndex_[(size_t) ch];
                auto& mq = queues_[(size_t) ch];
                long long& counter = sampleCounters_[(size_t) ch];

                for (int i = 0; i < numSamples; ++i)
                {
                    float x = channelData[i] * driveGain_;
                    if (std::isnan (x) || std::isinf (x))
                        x = 0.0f;

                    delayBuf[(size_t) writeIdx] = x;

                    float absx = std::abs (x);
                    long long idx = counter++;

                    while (mq.size > 0 && mq.back_val() <= absx)
                        mq.pop_back();

                    mq.push_back (idx, absx);

                    while (mq.size > 0 && mq.front_idx() <= idx - lookaheadSamples_)
                        mq.pop_front();

                    float peak = mq.empty() ? 0.0f : mq.front_val();

                    float requiredGain = 1.0f;
                    if (peak > 1e-12f)
                        requiredGain = std::min (1.0f, linearCeiling / peak);

                    float out = delayBuf[(size_t) readIdx] * requiredGain;

                    if (std::isnan (out) || std::isinf (out))
                        out = 0.0f;
                    out = juce::jlimit (-1.0f, 1.0f, out);

                    channelData[i] = out;

                    ++writeIdx; if (writeIdx >= bufferLen_) writeIdx = 0;
                    ++readIdx;  if (readIdx  >= bufferLen_) readIdx  = 0;
                }

                writeIndex_[(size_t) ch] = writeIdx;
                readIndex_[(size_t) ch] = readIdx;
            }
        }
        else // LimiterMode::Linked — Atmos path only
        {
            // Per-sample: compute each channel's required gain from its own
            // sliding-max peak, take the min across all channels, and apply that
            // single gain to every channel's delayed sample. Preserves the
            // spatial image by moving all channels together.
            for (int i = 0; i < numSamples; ++i)
            {
                float minRequiredGain = 1.0f;

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* channelData = buffer.getWritePointer (ch);
                    auto& delayBuf = delayBuffers_[(size_t) ch];
                    auto& mq = queues_[(size_t) ch];
                    long long& counter = sampleCounters_[(size_t) ch];
                    int writeIdx = writeIndex_[(size_t) ch];

                    float x = channelData[i] * driveGain_;
                    if (std::isnan (x) || std::isinf (x))
                        x = 0.0f;

                    delayBuf[(size_t) writeIdx] = x;

                    float absx = std::abs (x);
                    long long idx = counter++;

                    while (mq.size > 0 && mq.back_val() <= absx)
                        mq.pop_back();

                    mq.push_back (idx, absx);

                    while (mq.size > 0 && mq.front_idx() <= idx - lookaheadSamples_)
                        mq.pop_front();

                    float peak = mq.empty() ? 0.0f : mq.front_val();

                    float requiredGain = 1.0f;
                    if (peak > 1e-12f)
                        requiredGain = std::min (1.0f, linearCeiling / peak);

                    if (requiredGain < minRequiredGain)
                        minRequiredGain = requiredGain;
                }

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* channelData = buffer.getWritePointer (ch);
                    auto& delayBuf = delayBuffers_[(size_t) ch];
                    int readIdx = readIndex_[(size_t) ch];

                    float out = delayBuf[(size_t) readIdx] * minRequiredGain;

                    if (std::isnan (out) || std::isinf (out))
                        out = 0.0f;
                    out = juce::jlimit (-1.0f, 1.0f, out);

                    channelData[i] = out;

                    ++writeIndex_[(size_t) ch]; if (writeIndex_[(size_t) ch] >= bufferLen_) writeIndex_[(size_t) ch] = 0;
                    ++readIndex_[(size_t) ch];  if (readIndex_[(size_t) ch]  >= bufferLen_) readIndex_[(size_t) ch]  = 0;
                }
            }
        }
    }
} // namespace ceilingIO::dsp