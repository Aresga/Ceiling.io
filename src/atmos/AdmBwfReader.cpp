#include "atmos/AdmBwfReader.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace ceilingIO::atmos
{
    namespace
    {
        dsp::ChannelLayout layoutForBedCount (int bedChannels) noexcept
        {
            switch (bedChannels)
            {
                case 2:  return dsp::ChannelLayout::Stereo;
                case 6:  return dsp::ChannelLayout::Surround51;
                case 8:  return dsp::ChannelLayout::Atmos712;
                case 10: return dsp::ChannelLayout::Atmos714;
                default: break;
            }
            return dsp::ChannelLayout::Stereo; // fallback (flat weights)
        }

        juce::uint32 readU32LE (const juce::uint8* p) noexcept
        {
            return (juce::uint32) p[0] | ((juce::uint32) p[1] << 8)
                 | ((juce::uint32) p[2] << 16) | ((juce::uint32) p[3] << 24);
        }

        // Scan RIFF chunks for an "axml" chunk and return its UTF-8 contents.
        juce::String extractAxmlFromData (const void* data, size_t size)
        {
            const auto* p = static_cast<const juce::uint8*> (data);
            if (size < 12 || std::memcmp (p, "RIFF", 4) != 0 || std::memcmp (p + 8, "WAVE", 4) != 0)
                return {};

            size_t offset = 12;
            while (offset + 8 <= size)
            {
                const auto* chunkHeader = p + offset;
                const juce::uint32 chunkSize = readU32LE (chunkHeader + 4);
                if (offset + 8 + chunkSize > size)
                    break;

                if (std::memcmp (chunkHeader, "axml", 4) == 0)
                    return juce::String::fromUTF8 ((const char*) (chunkHeader + 8), (int) chunkSize);

                // chunks are word-aligned (pad to even)
                offset += 8 + chunkSize + (chunkSize & 1u);
            }
            return {};
        }

        int countTagRecursive (const juce::XmlElement* e, const juce::String& tag)
        {
            int count = 0;
            for (auto* child = e->getFirstChildElement(); child != nullptr; child = child->getNextElement())
            {
                if (child->getTagName() == tag)
                    ++count;
                count += countTagRecursive (child, tag);
            }
            return count;
        }

        int countObjectsRecursive (const juce::XmlElement* e)
        {
            int count = 0;
            for (auto* child = e->getFirstChildElement(); child != nullptr; child = child->getNextElement())
            {
                if (child->getTagName() == "audioObject"
                    && child->getStringAttribute ("typeDefinition") == "Objects")
                    ++count;
                count += countObjectsRecursive (child);
            }
            return count;
        }
    }

    bool AdmBwfReader::open (const juce::File& file)
    {
        juce::MemoryBlock fileData;
        if (! file.loadFileAsData (fileData))
            return false;

        // Extract axml straight from the RIFF bytes (more robust than relying
        // on JUCE's metadata chunk handling).
        juce::String axml = extractAxmlFromData (fileData.getData(), fileData.getSize());

        auto stream = file.createInputStream();
        if (stream == nullptr)
            return false;

        juce::WavAudioFormat wav;
        reader_.reset (wav.createReaderFor (stream.release(), true)); // reader owns the stream
        if (reader_ == nullptr)
            return false;

        return parseAxml (axml);
    }

    bool AdmBwfReader::openFromMemory (const void* data, size_t size)
    {
        if (data == nullptr || size == 0)
            return false;

        // Non-owning MemoryInputStream referencing the caller's data. The WAV
        // reader (deleteStream = true) owns and deletes the stream; the caller's
        // data must outlive the reader.
        auto* stream = new juce::MemoryInputStream (data, size, false);
        juce::WavAudioFormat wav;
        reader_.reset (wav.createReaderFor (stream, true));
        if (reader_ == nullptr)
            return false;

        juce::String axml = reader_->metadataValues.getValue ("axml", {});
        if (axml.isEmpty())
            axml = extractAxmlFromData (data, size);

        return parseAxml (axml);
    }

    bool AdmBwfReader::parseAxml (const juce::String& axml)
    {
        if (axml.isEmpty())
            return false; // not an ADM BWF

        axmlChunk_ = axml;
        sampleRate_ = reader_->sampleRate;
        lengthInSamples_ = reader_->lengthInSamples;
        const int totalChannels = (int) reader_->numChannels;

        std::unique_ptr<juce::XmlElement> xml (juce::XmlDocument::parse (axml));
        if (xml == nullptr)
            return false;

        const int trackUidCount = countTagRecursive (xml.get(), "audioTrackUID");
        int objectCount         = countObjectsRecursive (xml.get());
        (void) trackUidCount; // sanity only; reader channel count is authoritative

        int bedChannels = totalChannels - objectCount;
        if (bedChannels < 0)
            bedChannels = 0; // malformed axml (more objects than channels) — clamp

        // Derive objectCount from the clamped bed so every trackIndex is
        // guaranteed in [0, totalChannels). Defends readBlock against malformed ADM.
        objectCount = totalChannels - bedChannels;

        // Accept known bed layouts; reject anything we cannot weight correctly
        // (unless there are objects, in which case we fall back to flat bed weights).
        const bool bedOk = (bedChannels == 2 || bedChannels == 6
                            || bedChannels == 8 || bedChannels == 10);
        if (totalChannels <= 0 || (! bedOk && objectCount == 0))
            return false;

        bedChannels_ = bedChannels;
        bedLayout_   = layoutForBedCount (bedChannels_);

        objects_.clear();
        objects_.resize ((size_t) objectCount);
        for (int i = 0; i < objectCount; ++i)
        {
            objects_[(size_t) i].trackIndex = bedChannels + i;
            // position/size/programme left at defaults — minimal parser.
        }

        scratch_.setSize (totalChannels, 1024);
        return true;
    }

    bool AdmBwfReader::readBlock (juce::AudioBuffer<float>& bedBuffer,
                                  std::vector<juce::AudioBuffer<float>>& objectBuffers,
                                  juce::int64 startSample, int numSamples)
    {
        if (reader_ == nullptr || numSamples <= 0)
            return false;

        const int totalChannels = (int) reader_->numChannels;
        if (scratch_.getNumChannels() != totalChannels || scratch_.getNumSamples() < numSamples)
            scratch_.setSize (totalChannels, numSamples, false, false, true);

        // Multi-channel read into the scratch buffer (channel-for-channel).
        std::vector<float*> ptrs ((size_t) totalChannels);
        for (int ch = 0; ch < totalChannels; ++ch)
            ptrs[(size_t) ch] = scratch_.getWritePointer (ch);

        if (! reader_->read (ptrs.data(), totalChannels, startSample, numSamples))
            return false;

        bedBuffer.setSize (bedChannels_, numSamples, false, false, true);
        if ((int) objectBuffers.size() < (int) objects_.size())
            objectBuffers.resize (objects_.size());

        for (int ch = 0; ch < bedChannels_; ++ch)
            bedBuffer.copyFrom (ch, 0, scratch_, ch, 0, numSamples);

        for (size_t i = 0; i < objects_.size(); ++i)
        {
            const int srcCh = objects_[i].trackIndex;
            objectBuffers[i].setSize (1, numSamples, false, false, true);
            objectBuffers[i].copyFrom (0, 0, scratch_, srcCh, 0, numSamples);
        }

        return true;
    }
} // namespace ceilingIO::atmos