#include "atmos/AdmBwfWriter.h"

#include <algorithm>
#include <cmath>

namespace ceilingIO::atmos
{
    namespace
    {
        // WAVE_FORMAT_EXTENSIBLE channel masks for the bed layouts. Object
        // channels have no standard mask bit; the axml chunk carries the full
        // ADM mapping, so the mask only needs to cover the bed.
        juce::uint32 channelMaskFor (ceilingIO::dsp::ChannelLayout layout) noexcept
        {
            // FL=0x1 FR=0x2 FC=0x4 LFE=0x8 BL=0x10 BR=0x20
            // TFL=0x2000 TFR=0x8000 TBL=0x10000 TBR=0x40000
            switch (layout)
            {
                case ceilingIO::dsp::ChannelLayout::Stereo:    return 0x00000003; // FL|FR
                case ceilingIO::dsp::ChannelLayout::Surround51: return 0x0000003F; // FL|FR|FC|LFE|BL|BR
                case ceilingIO::dsp::ChannelLayout::Atmos712:  return 0x0000A03F; // + TFL|TFR
                case ceilingIO::dsp::ChannelLayout::Atmos714:  return 0x0005A03F; // + TBL|TBR
            }
            return 0;
        }

        // KSDATAFORMAT_SUBTYPE_PCM GUID, little-endian on disk.
        const juce::uint8 kPcmSubFormat[16] = {
            0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
            0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
        };
    }

    void AdmBwfWriter::writeU16LE (juce::uint16 v)
    {
        const juce::uint8 b[2] = { (juce::uint8) (v & 0xFF), (juce::uint8) ((v >> 8) & 0xFF) };
        if (! out_->write (b, 2)) ok_ = false;
    }

    void AdmBwfWriter::writeU32LE (juce::uint32 v)
    {
        const juce::uint8 b[4] = {
            (juce::uint8) (v & 0xFF), (juce::uint8) ((v >> 8) & 0xFF),
            (juce::uint8) ((v >> 16) & 0xFF), (juce::uint8) ((v >> 24) & 0xFF)
        };
        if (! out_->write (b, 4)) ok_ = false;
    }

    void AdmBwfWriter::writeS24LE (float sample)
    {
        int v = (int) std::lround (sample * 8388607.0);
        v = juce::jlimit (-8388608, 8388607, v);
        const juce::uint8 b[3] = {
            (juce::uint8) (v & 0xFF),
            (juce::uint8) ((v >> 8) & 0xFF),
            (juce::uint8) ((v >> 16) & 0xFF)
        };
        if (! out_->write (b, 3)) ok_ = false;
    }

    bool AdmBwfWriter::open (juce::OutputStream& stream, double sampleRate, int numChannels,
                              int bitsPerSample, dsp::ChannelLayout bedLayout, const juce::String& axml)
    {
        out_ = &stream;
        numChannels_   = numChannels;
        bitsPerSample_ = bitsPerSample;
        bytesPerSample_ = bitsPerSample / 8;
        axml_ = axml;
        ok_ = true;

        if (bitsPerSample != 24 && bitsPerSample != 16)
            return false; // minimal writer supports 16/24-bit only

        const juce::uint32 blockAlign = (juce::uint32) (numChannels * bytesPerSample_);
        const juce::uint32 avgBytesPerSec = (juce::uint32) (sampleRate * blockAlign);

        // RIFF header (size patched at close)
        if (! out_->write ("RIFF", 4)) ok_ = false;
        riffSizeFieldOffset_ = (juce::uint64) out_->getPosition();
        writeU32LE (0);
        if (! out_->write ("WAVE", 4)) ok_ = false;

        // fmt chunk (WAVE_FORMAT_EXTENSIBLE, 40 bytes)
        if (! out_->write ("fmt ", 4)) ok_ = false;
        writeU32LE (40);
        writeU16LE (0xFFFE);                          // wFormatTag = EXTENSIBLE
        writeU16LE ((juce::uint16) numChannels);
        writeU32LE ((juce::uint32) (juce::int64) sampleRate);
        writeU32LE (avgBytesPerSec);
        writeU16LE ((juce::uint16) blockAlign);
        writeU16LE ((juce::uint16) bitsPerSample);
        writeU16LE (22);                              // cbSize
        writeU16LE ((juce::uint16) bitsPerSample);   // wValidBitsPerSample
        writeU32LE (channelMaskFor (bedLayout));     // dwChannelMask
        if (! out_->write (kPcmSubFormat, 16)) ok_ = false;  // SubFormat GUID

        // data chunk (size patched at close)
        if (! out_->write ("data", 4)) ok_ = false;
        dataSizeFieldOffset_ = (juce::uint64) out_->getPosition();
        writeU32LE (0);

        return ok_;
    }

    bool AdmBwfWriter::writeBlock (const juce::AudioBuffer<float>& bedBuffer,
                                    const std::vector<juce::AudioBuffer<float>>& objectBuffers,
                                    int numSamples)
    {
        if (out_ == nullptr)
            return false;

        const int bedCh = bedBuffer.getNumChannels();
        const int numObj = (int) objectBuffers.size();

        // Interleave: for each frame, bed channels then object channels.
        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < bedCh; ++ch)
                writeS24LE (bedBuffer.getReadPointer (ch)[i]);

            for (int k = 0; k < numObj; ++k)
            {
                float v = 0.0f;
                if (k < (int) objectBuffers.size() && objectBuffers[(size_t) k].getNumChannels() > 0
                    && i < objectBuffers[(size_t) k].getNumSamples())
                    v = objectBuffers[(size_t) k].getReadPointer (0)[i];
                writeS24LE (v);
            }
        }

        dataBytesWritten_ += (juce::uint64) numSamples * (juce::uint64) (bedCh + numObj) * (juce::uint64) bytesPerSample_;
        return ok_;
    }

    bool AdmBwfWriter::close()
    {
        if (out_ == nullptr)
            return false;

        // axml chunk (verbatim pass-through)
        if (axml_.isNotEmpty())
        {
            const juce::String axmlText = axml_;
            if (! out_->write ("axml", 4)) ok_ = false;
            const juce::uint32 axmlBytes = (juce::uint32) axmlText.getNumBytesAsUTF8();
            writeU32LE (axmlBytes);
            if (! out_->write (axmlText.toRawUTF8(), (size_t) axmlBytes)) ok_ = false;
            if (axmlBytes % 2 != 0) // RIFF chunks are word-aligned
                if (! out_->writeByte (0)) ok_ = false;
        }

        // Patch data chunk size
        const juce::uint64 dataPos = (juce::uint64) out_->getPosition();
        out_->setPosition ((juce::int64) dataSizeFieldOffset_);
        writeU32LE ((juce::uint32) dataBytesWritten_);

        // Patch RIFF size (file size - 8)
        out_->setPosition ((juce::int64) riffSizeFieldOffset_);
        const juce::uint32 riffSize = (juce::uint32) (dataPos - 8);
        writeU32LE (riffSize);

        out_->setPosition ((juce::int64) dataPos);
        out_->flush();
        return ok_;
    }
} // namespace ceilingIO::atmos