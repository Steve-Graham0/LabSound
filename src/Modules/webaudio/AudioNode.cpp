/*
 * Copyright (C) 2010, Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "LabSoundConfig.h"
#include "AudioNode.h"

#include "AudioContext.h"
#include "AudioContextLock.h"
#include "AudioNodeInput.h"
#include "AudioNodeOutput.h"
#include "AudioParam.h"
#include "WTF/Atomics.h"
//#include "WTF/MainThread.h"

#if DEBUG_AUDIONODE_REFERENCES
#include <stdio.h>
#endif

using namespace std;

namespace WebCore {

AudioNode::AudioNode(float sampleRate)
    : m_isInitialized(false)
    , m_nodeType(NodeTypeUnknown)
    , m_sampleRate(sampleRate)
    , m_lastProcessingTime(-1)
    , m_lastNonSilentTime(-1)
    , m_normalRefCount(1) // start out with normal refCount == 1 (like WTF::RefCounted class)
    , m_connectionRefCount(0)
    , m_isMarkedForDeletion(false)
    , m_isDisabled(false)
    , m_inputCount(0)
    , m_outputCount(0)
{
#if DEBUG_AUDIONODE_REFERENCES
    if (!s_isNodeCountInitialized) {
        s_isNodeCountInitialized = true;
        atexit(AudioNode::printNodeCounts);
    }
#endif
}

AudioNode::~AudioNode()
{
    /// @TODO the problem is that the AudioNodeOutput retains a pointer to AudioNode even though AudioNode is dead.
    /// AudioNode should disconnect all here, but it can't because it needs context locks.
    /// it can't get context locks because it's a destructor.
    /// This needs an architectural revision
    /// because setting initialized false only works if the memory is not reclaimed before the processing loop is called again.
    /// BAD.
    
    m_isInitialized = false;    // mark in case a dead pointer was retained somewhere so it can be seen while debugging
#if 0
    ContextRenderLock r;
    ContextGraphLock g;
    for (int i = 0; i < AUDIONODE_MAXOUTPUTS; ++i)
        if (auto ptr = m_outputs[i])
            AudioNodeOutput::disconnectAll(g, r, ptr);
#endif
    
#if DEBUG_AUDIONODE_REFERENCES
    --s_nodeCount[nodeType()];
    fprintf(stderr, "%p: %d: AudioNode::~AudioNode() %d %d\n", this, nodeType(), m_normalRefCount, m_connectionRefCount);
#endif
}

void AudioNode::initialize()
{
    m_isInitialized = true;
}

void AudioNode::uninitialize()
{
    m_isInitialized = false;
}

void AudioNode::setNodeType(NodeType type)
{
    m_nodeType = type;

#if DEBUG_AUDIONODE_REFERENCES
    ++s_nodeCount[type];
#endif
}

void AudioNode::lazyInitialize()
{
    if (!isInitialized())
        initialize();
}

void AudioNode::addInput(std::shared_ptr<AudioNodeInput> input)
{
    static mutex inputLock;
    lock_guard<mutex> lock(inputLock);
    if (m_inputCount < AUDIONODE_MAXINPUTS) {
        m_inputs[m_inputCount] = input;
        ++m_inputCount;
    }
    else {
        ASSERT(0 == "Too many inputs");
    }
}

void AudioNode::addOutput(std::shared_ptr<AudioNodeOutput> output)
{
    static mutex outputLock;
    lock_guard<mutex> lock(outputLock);
    if (m_outputCount < AUDIONODE_MAXOUTPUTS) {
        m_outputs[m_outputCount] = output;
        ++m_outputCount;
    }
    else {
        ASSERT(0 == "Too many outputs");
    }
}

std::shared_ptr<AudioNodeInput> AudioNode::input(unsigned i)
{
    if (i < AUDIONODE_MAXINPUTS)
        return m_inputs[i];
    return 0;
}

std::shared_ptr<AudioNodeOutput> AudioNode::output(unsigned i)
{
    if (i < AUDIONODE_MAXOUTPUTS)
        return m_outputs[i];
    return 0;
}

void AudioNode::connect(AudioContext* context,
                        AudioNode* destination, unsigned outputIndex, unsigned inputIndex, ExceptionCode& ec)
{
    if (!context) {
        ec = SYNTAX_ERR;
        return;
    }

    if (!destination) {
        ec = SYNTAX_ERR;
        return;
    }

    // Sanity check input and output indices.
    if (outputIndex >= numberOfOutputs()) {
        ec = INDEX_SIZE_ERR;
        return;
    }

    if (destination && inputIndex >= destination->numberOfInputs()) {
        ec = INDEX_SIZE_ERR;
        return;
    }

    auto input = destination->input(inputIndex);
    auto output = this->output(outputIndex);
    
    // &&& no need to defer this any more? If so remove connect from context and context from connect param list
    context->connect(input, output);
}

void AudioNode::connect(std::shared_ptr<AudioParam> param, unsigned outputIndex, ExceptionCode& ec)
{
    if (!param) {
        ec = SYNTAX_ERR;
        return;
    }

    if (outputIndex >= numberOfOutputs()) {
        ec = INDEX_SIZE_ERR;
        return;
    }

    AudioParam::connect(param, this->output(outputIndex));
}

void AudioNode::disconnect(AudioContext* context, unsigned outputIndex, ExceptionCode& ec)
{
    // Sanity check input and output indices.
    if (outputIndex >= numberOfOutputs()) {
        ec = INDEX_SIZE_ERR;
        return;
    }

    context->disconnect(this->output(outputIndex));
}

void AudioNode::processIfNecessary(ContextRenderLock& r, size_t framesToProcess)
{
    if (!r.context())
        return;
    
    if (!isInitialized())
        return;

    auto ac = r.context();
    
    // Ensure that we only process once per rendering quantum.
    // This handles the "fanout" problem where an output is connected to multiple inputs.
    // The first time we're called during this time slice we process, but after that we don't want to re-process,
    // instead our output(s) will already have the results cached in their bus;
    double currentTime = ac->currentTime();
    if (m_lastProcessingTime != currentTime) {
        m_lastProcessingTime = currentTime; // important to first update this time because of feedback loops in the rendering graph

        pullInputs(r, framesToProcess);

        bool silentInputs = inputsAreSilent();
        if (!silentInputs)
            m_lastNonSilentTime = (ac->currentSampleFrame() + framesToProcess) / static_cast<double>(m_sampleRate);

        bool ps = propagatesSilence(r.context()->currentTime());
        if (silentInputs && ps)
            silenceOutputs();
        else {
            process(r, framesToProcess);
            unsilenceOutputs();
        }
    }
}

void AudioNode::checkNumberOfChannelsForInput(ContextRenderLock& r, AudioNodeInput* input)
{
    ASSERT(r.context());
    for (int i = 0; i < AUDIONODE_MAXINPUTS; ++i) {
        if (m_inputs[i].get() == input) {
            input->updateInternalBus(r);
            break;
        }
    }
}

bool AudioNode::propagatesSilence(double now) const
{
    return m_lastNonSilentTime + latencyTime() + tailTime() < now;
}

void AudioNode::pullInputs(ContextRenderLock& r, size_t framesToProcess)
{
    ASSERT(r.context());
    // Process all of the AudioNodes connected to our inputs.
    for (unsigned i = 0; i < AUDIONODE_MAXINPUTS; ++i)
        if (auto in = input(i))
            in->pull(r, 0, framesToProcess);
}

bool AudioNode::inputsAreSilent()
{
    for (unsigned i = 0; i < AUDIONODE_MAXINPUTS; ++i) {
        if (auto in = input(i))
            if (!in->bus()->isSilent())
                return false;
    }
    return true;
}

void AudioNode::silenceOutputs()
{
    for (unsigned i = 0; i < AUDIONODE_MAXOUTPUTS; ++i)
        if (auto out = output(i))
            out->bus()->zero();
}

void AudioNode::unsilenceOutputs()
{
    for (unsigned i = 0; i < AUDIONODE_MAXOUTPUTS; ++i)
        if (auto out = output(i))
            out->bus()->clearSilentFlag();
}

void AudioNode::enableOutputsIfNecessary(ContextGraphLock& g)
{
    if (m_isDisabled && m_connectionRefCount > 0) {
        m_isDisabled = false;
        for (unsigned i = 0; i < AUDIONODE_MAXOUTPUTS; ++i)
            if (auto out = output(i))
                AudioNodeOutput::enable(g, out);
    }
}

void AudioNode::disableOutputsIfNecessary(ContextGraphLock& g)
{
    // Disable outputs if appropriate. We do this if the number of connections is 0 or 1. The case
    // of 0 is from finishDeref() where there are no connections left. The case of 1 is from
    // AudioNodeInput::disable() where we want to disable outputs when there's only one connection
    // left because we're ready to go away, but can't quite yet.
    if (m_connectionRefCount <= 1 && !m_isDisabled) {
        // Still may have JavaScript references, but no more "active" connection references, so put all of our outputs in a "dormant" disabled state.
        // Garbage collection may take a very long time after this time, so the "dormant" disabled nodes should not bog down the rendering...

        // As far as JavaScript is concerned, our outputs must still appear to be connected.
        // But internally our outputs should be disabled from the inputs they're connected to.
        // disable() can recursively deref connections (and call disable()) down a whole chain of connected nodes.

        // FIXME: we special case the convolver and delay since they have a significant tail-time and shouldn't be disconnected simply
        // because they no longer have any input connections. This needs to be handled more generally where AudioNodes have
        // a tailTime attribute. Then the AudioNode only needs to remain "active" for tailTime seconds after there are no
        // longer any active connections.
        if (nodeType() != NodeTypeConvolver && nodeType() != NodeTypeDelay) {
            m_isDisabled = true;
            for (unsigned i = 0; i < AUDIONODE_MAXOUTPUTS; ++i)
                if (auto out = output(i))
                    AudioNodeOutput::disable(g, out);
        }
    }
}

void AudioNode::ref(ContextGraphLock& g, RefType refType)
{
    if (refType == RefTypeNormal)
        atomicIncrement(&m_normalRefCount);
    if (refType == RefTypeConnection) {
        atomicIncrement(&m_connectionRefCount);
        
        // See the disabling code in finishDeref() below. This handles the case where a node
        // is being re-connected after being used at least once and disconnected.
        // In this case, we need to re-enable.
        enableOutputsIfNecessary(g);
    }

#if DEBUG_AUDIONODE_REFERENCES
    fprintf(stderr, "%p: %d: AudioNode::ref(%d) %d %d\n", this, nodeType(), refType, m_normalRefCount, m_connectionRefCount);
#endif
}

void AudioNode::deref(ContextGraphLock& g, RefType refType)
{
    switch (refType) {
        case RefTypeNormal:
            ASSERT(m_normalRefCount > 0);
            atomicDecrement(&m_normalRefCount);
            break;
        case RefTypeConnection:
            ASSERT(m_connectionRefCount > 0);
            atomicDecrement(&m_connectionRefCount);
            break;
        default:
            ASSERT_NOT_REACHED();
    }
    
#if DEBUG_AUDIONODE_REFERENCES
    fprintf(stderr, "%p: %d: AudioNode::deref(%d) %d %d\n", this, nodeType(), refType, m_normalRefCount, m_connectionRefCount);
#endif
    
    if (!m_connectionRefCount) {
        if (!m_normalRefCount) {
            if (!m_isMarkedForDeletion) {
                // All references are gone - this node needs to go away.
                for (unsigned i = 0; i < AUDIONODE_MAXOUTPUTS; ++i)
                    if (auto out = output(i))
                        AudioNodeOutput::disconnectAll(g, out); // This will deref() nodes we're connected to.
                
                // Mark for deletion at end of each render quantum or when context shuts down.
                //                if (!shuttingDown)
                //                    ac->markForDeletion(this);
                
                m_isMarkedForDeletion = true;
            }
        }
        else if (refType == RefTypeConnection)
            disableOutputsIfNecessary(g);
    }
}

#if DEBUG_AUDIONODE_REFERENCES

bool AudioNode::s_isNodeCountInitialized = false;
int AudioNode::s_nodeCount[NodeTypeEnd];

void AudioNode::printNodeCounts()
{
    fprintf(stderr, "\n\n");
    fprintf(stderr, "===========================\n");
    fprintf(stderr, "AudioNode: reference counts\n");
    fprintf(stderr, "===========================\n");

    for (unsigned i = 0; i < NodeTypeEnd; ++i)
        fprintf(stderr, "%d: %d\n", i, s_nodeCount[i]);

    fprintf(stderr, "===========================\n\n\n");
}

#endif // DEBUG_AUDIONODE_REFERENCES

} // namespace WebCore
