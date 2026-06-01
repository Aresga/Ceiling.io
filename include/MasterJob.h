#pragma once

#include <JuceHeader.h>
#include "JobModels.h"
#include "JobStore.h"

namespace server
{
    class MasterJob final : public juce::ThreadPoolJob
    {
    public:
        MasterJob (JobStore& storeToUse, SubmitRequest requestToUse, juce::String jobIdToUse);
        ~MasterJob() override = default;

        // The core worker loop executed by the thread pool
        JobStatus runJob() override;

    private:
        JobStore& store;
        SubmitRequest request;
        juce::String jobId;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterJob)
    };
}
