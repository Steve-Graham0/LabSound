// License: BSD 3 Clause
// Copyright (C) 2010, Google Inc. All rights reserved.
// Copyright (C) 2015+, The LabSound Authors. All rights reserved.

#include "LabSound/core/ChannelMergerNode.h"
#include "LabSound/core/AudioBus.h"
#include "LabSound/core/AudioContext.h"
#include "LabSound/core/AudioNodeInput.h"
#include "LabSound/core/AudioNodeOutput.h"

#include "internal/Assertions.h"

using namespace std;

namespace lab
{

ChannelMergerNode::ChannelMergerNode(size_t numberOfInputs_)
    : AudioNode()
{
    addInputs(numberOfInputs_);
    addOutput(std::unique_ptr<AudioNodeOutput>(new AudioNodeOutput(this, 1)));
    initialize();  // initialize only sets a flag, no need to allocate memory according to input count
}

void ChannelMergerNode::addInputs(size_t n)
{
    // Create the requested number of inputs.
    for (uint32_t i = 0; i < n; ++i)
        addInput(std::unique_ptr<AudioNodeInput>(new AudioNodeInput(this)));
}

void ChannelMergerNode::process(ContextRenderLock & r, int bufferSize, int offset, int count)
{
    auto output = this->output(0);
    ASSERT_UNUSED(bufferSize, bufferSize == output->bus(r)->length());

    // Output bus not updated yet, so just output silence. See Note * in checkNumberOfChannelsForInput
    if (m_desiredNumberOfOutputChannels != output->numberOfChannels())
    {
        output->bus(r)->zero();
        return;
    }

    // Merge all the channels from all the inputs into one output.
    uint32_t outputChannelIndex = 0;
    for (int i = 0; i < numberOfInputs(); ++i)
    {
        auto input = this->input(i);

        if (input->isConnected())
        {
            size_t numberOfInputChannels = input->bus(r)->numberOfChannels();

            // Merge channels from this particular input.
            for (int j = 0; j < numberOfInputChannels; ++j)
            {
                AudioChannel * inputChannel = input->bus(r)->channel(j);
                AudioChannel * outputChannel = output->bus(r)->channel(outputChannelIndex);

                outputChannel->copyFrom(inputChannel);
                ++outputChannelIndex;
            }
        }
    }

    ASSERT(outputChannelIndex == output->numberOfChannels());
}

void ChannelMergerNode::reset(ContextRenderLock &)
{
}

// Any time a connection or disconnection happens on any of our inputs, we potentially need to change the
// number of channels of our output.
void ChannelMergerNode::checkNumberOfChannelsForInput(ContextRenderLock & r, AudioNodeInput * input)
{
    // Count how many channels we have all together from all of the inputs.
    int numberOfOutputChannels = 0;

    for (int i = 0; i < numberOfInputs(); ++i)
    {
        auto input = this->input(i);

        if (input->isConnected())
        {
            numberOfOutputChannels += input->bus(r)->numberOfChannels();
        }
    }

    // Set the correct number of channels on the output
    auto output = this->output(0);
    output->setNumberOfChannels(r, numberOfOutputChannels);

    // Note * There can in rare cases be a slight delay before the output bus is updated to the new number of
    // channels because of tryLocks() in the context's updating system. So record the new number of
    // output channels here.
    m_desiredNumberOfOutputChannels = numberOfOutputChannels;

    AudioNode::checkNumberOfChannelsForInput(r, input);
}

}  // namespace lab
