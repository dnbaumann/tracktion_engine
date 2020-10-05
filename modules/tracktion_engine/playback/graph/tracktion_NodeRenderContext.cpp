/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2018
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

#pragma once

namespace tracktion_engine
{

namespace
{
    static Plugin::Array findAllPlugins (tracktion_graph::Node& node)
    {
        Plugin::Array plugins, insideRacks;

        for (auto n : getNodes (node, VertexOrdering::preordering))
            if (auto pluginNode = dynamic_cast<PluginNode*> (n))
                plugins.add (&pluginNode->getPlugin());

        for (auto plugin : plugins)
            if (auto rack = dynamic_cast<RackInstance*> (plugin))
                if (auto type = rack->type)
                    for (auto p : type->getPlugins())
                        insideRacks.addIfNotAlreadyThere (p);

        plugins.addArray (insideRacks);
        return plugins;
    }
}


//==============================================================================
NodeRenderContext::NodeRenderContext (Renderer::RenderTask& owner_, Renderer::Parameters& p,
                                      std::unique_ptr<Node> n,
                                      std::unique_ptr<tracktion_graph::PlayHead> playHead_,
                                      std::unique_ptr<tracktion_graph::PlayHeadState> playHeadState_,
                                      std::unique_ptr<ProcessState> processState_,
                                      juce::AudioFormatWriter::ThreadedWriter::IncomingDataReceiver* sourceToUpdate_)
    : owner (owner_),
      r (p), originalParams (p),
      playHead (std::move (playHead_)),
      playHeadState (std::move (playHeadState_)),
      processState (std::move (processState_)),
      status (juce::Result::ok()),
      ditherers (256, r.bitDepth),
      sourceToUpdate (sourceToUpdate_)
{
    CRASH_TRACER
    TRACKTION_ASSERT_MESSAGE_THREAD
    jassert (r.engine != nullptr);
    jassert (r.edit != nullptr);
    jassert (r.time.getLength() > 0.0);

    nodePlayer = std::make_unique<TracktionNodePlayer> (std::move (n), *processState, r.sampleRateForAudio, r.blockSizeForAudio,
                                                        getPoolCreatorFunction (ThreadPoolStrategy::realTime));
    nodePlayer->setNumThreads ((size_t) p.engine->getEngineBehaviour().getNumberOfCPUsToUseForAudio() - 1);
    
    numLatencySamplesToDrop = nodePlayer->getNode()->getNodeProperties().latencyNumSamples;
    r.time.end += sampleToTime (numLatencySamplesToDrop, r.sampleRateForAudio);

    if (r.edit->getTransport().isPlayContextActive())
    {
        jassertfalse;
        TRACKTION_LOG_ERROR("Rendering whilst attached to audio device");
    }

    if (r.shouldNormalise || r.trimSilenceAtEnds || r.shouldNormaliseByRMS)
    {
        needsToNormaliseAndTrim = true;

        r.audioFormat = r.engine->getAudioFileFormatManager().getFrozenFileFormat();

        intermediateFile = std::make_unique<juce::TemporaryFile> (r.destFile.withFileExtension (r.audioFormat->getFileExtensions()[0]));
        r.destFile = intermediateFile->getFile();

        r.shouldNormalise = false;
        r.trimSilenceAtEnds = false;
        r.shouldNormaliseByRMS = false;
    }

    numOutputChans = 2;

    {
        auto props = nodePlayer->getNode()->getNodeProperties();

        if (! props.hasAudio)
        {
            status = juce::Result::fail (TRANS("Didn't find any audio to render"));
            return;
        }

        if (r.mustRenderInMono || (r.canRenderInMono && (props.numberOfChannels < 2)))
            numOutputChans = 1;
    }

    AudioFileUtils::addBWAVStartToMetadata (r.metadata, (int64_t) (r.time.getStart() * r.sampleRateForAudio));

    writer = std::make_unique<AudioFileWriter> (AudioFile (*originalParams.engine, r.destFile),
                                                r.audioFormat, numOutputChans, r.sampleRateForAudio,
                                                r.bitDepth, r.metadata, r.quality);

    if (r.destFile != juce::File() && ! writer->isOpen())
    {
        status = juce::Result::fail (TRANS("Couldn't write to target file"));
        return;
    }

    blockLength = r.blockSizeForAudio / r.sampleRateForAudio;

    // number of blank blocks to play before starting, to give plugins time to warm up
    numPreRenderBlocks = (int) ((r.sampleRateForAudio / 2) / r.blockSizeForAudio + 1);

    // how long each block must take in real-time
    realTimePerBlock = (int) (blockLength * 1000.0 + 0.99);
    lastTime = juce::Time::getMillisecondCounterHiRes();
    sleepCounter = 10;

    currentTempoPosition = std::make_unique<TempoSequencePosition> (r.edit->tempoSequence);

    peak = 0.0001f;
    rmsTotal = 0.0;
    rmsNumSamps = 0;
    numNonZeroSamps = 0;
    streamTime = r.time.getStart();

    precount = numPreRenderBlocks;
    streamTime -= precount * blockLength;

    plugins = findAllPlugins (*nodePlayer->getNode());

    // Set the realtime property before preparing to play
    Renderer::RenderTask::setAllPluginsRealtime (plugins, r.realTimeRender);
    nodePlayer->prepareToPlay (r.sampleRateForAudio, r.blockSizeForAudio);
    Renderer::RenderTask::flushAllPlugins (plugins, r.sampleRateForAudio, r.blockSizeForAudio);

    samplesTrimmed = 0;
    hasStartedSavingToFile = ! r.trimSilenceAtEnds;

    playHead->stop();
    playHead->setPosition (timeToSample (r.time.getStart(), r.sampleRateForAudio));

    samplesToWrite = juce::roundToInt ((r.time.getLength() + r.endAllowance) * r.sampleRateForAudio);

    if (sourceToUpdate != nullptr)
        sourceToUpdate->reset (numOutputChans, r.sampleRateForAudio, samplesToWrite);
}

NodeRenderContext::~NodeRenderContext()
{
    CRASH_TRACER
    r.resultMagnitude = owner.params.resultMagnitude = peak;
    r.resultRMS = owner.params.resultRMS = rmsNumSamps > 0 ? (float) (rmsTotal / rmsNumSamps) : 0.0f;
    r.resultAudioDuration = owner.params.resultAudioDuration = float (numNonZeroSamps / owner.params.sampleRateForAudio);

    playHead->stop();
    Renderer::RenderTask::setAllPluginsRealtime (plugins, true);

    if (writer != nullptr)
        writer->closeForWriting();

    callBlocking ([this] { nodePlayer.reset(); });

    if (needsToNormaliseAndTrim)
        owner.performNormalisingAndTrimming (originalParams, r);
}

bool NodeRenderContext::renderNextBlock (std::atomic<float>& progressToUpdate)
{
    CRASH_TRACER
    jassert (! r.edit->getTransport().isPlayContextActive());

    if (--sleepCounter <= 0)
    {
        sleepCounter = sleepCounterMax;
        juce::Thread::sleep (1);
    }

    if (owner.shouldExit())
    {
        writer->closeForWriting();
        r.destFile.deleteFile();

        playHead->stop();
        Renderer::RenderTask::setAllPluginsRealtime (plugins, true);

        return true;
    }

    auto blockEnd = streamTime + blockLength;

    if (precount > 0)
        blockEnd = juce::jmin (r.time.getStart(), blockEnd);

    if (precount > numPreRenderBlocks / 2)
        playHead->setPosition (timeToSample (streamTime, r.sampleRateForAudio));
    else if (precount == numPreRenderBlocks / 2)
        playHead->playSyncedToRange ({ timeToSample (streamTime, r.sampleRateForAudio), std::numeric_limits<int64_t>::max() });

    if (precount == 0)
    {
        streamTime = r.time.getStart();
        blockEnd = streamTime + blockLength;
        
        playHead->playSyncedToRange (timeToSample (EditTimeRange (streamTime, Edit::maximumLength), r.sampleRateForAudio));
        playHeadState->update (tracktion_graph::timeToSample (EditTimeRange (streamTime, blockEnd), r.sampleRateForAudio));
    }

    if (r.realTimeRender)
    {
        auto timeNow = juce::Time::getMillisecondCounterHiRes();
        auto timeToWait = (int) (realTimePerBlock - (timeNow - lastTime));
        lastTime = timeNow;

        if (timeToWait > 0)
            juce::Thread::sleep (timeToWait);
    }

    currentTempoPosition->setTime (streamTime);

    resetFP();

    const EditTimeRange streamTimeRange (streamTime, blockEnd);

    // Update modifier timers
    r.edit->updateModifierTimers (streamTime, r.blockSizeForAudio);

    // Wait for any nodes to render their sources or proxies
    auto leafNodesReady = [this]
    {
        for (auto node : getNodes (*nodePlayer->getNode(), VertexOrdering::postordering))
            if (node->getDirectInputNodes().empty() && ! node->isReadyToProcess())
                return false;
        
        return true;
    }();
    
    while (! (leafNodesReady || owner.shouldExit()))
        return false;

    juce::AudioBuffer<float> renderingBuffer (numOutputChans, r.blockSizeForAudio + 256);
    renderingBuffer.clear();
    midiBuffer.clear();
    const auto referenceSampleRange = tracktion_graph::timeToSample (streamTimeRange, originalParams.sampleRateForAudio);
    juce::dsp::AudioBlock<float> destBlock (renderingBuffer.getArrayOfWritePointers(),
                                            (size_t) renderingBuffer.getNumChannels(), (size_t) referenceSampleRange.getLength());
    nodePlayer->process ({ referenceSampleRange, { destBlock, midiBuffer} });

    if (precount <= 0)
    {
        jassert (playHeadState->isContiguousWithPreviousBlock());

        int numSamplesDone = (int) juce::jmin (samplesToWrite, (int64_t) r.blockSizeForAudio);
        samplesToWrite -= numSamplesDone;

        size_t blockSize = size_t (numSamplesDone);
        size_t blockOffset = 0;

        if (numLatencySamplesToDrop > 0)
        {
            const int numToDrop = std::min (numLatencySamplesToDrop, numSamplesDone);
            numLatencySamplesToDrop -= numToDrop;
            numSamplesDone -= numToDrop;
            
            blockSize = (size_t) numSamplesDone;
            blockOffset = destBlock.getNumSamples() - blockSize;
        }

        if (blockSize > 0)
        {
            jassert (blockSize <= destBlock.getNumSamples());

            if (writeAudioBlock (destBlock.getSubBlock (blockOffset, blockSize)) == WriteResult::failed)
                return true;
        }
    }
    else
    {
        // for the pre-count blocks, sleep to give things a chance to get going
        juce::Thread::sleep ((int) (blockLength * 1000));
    }

    if (streamTime > r.time.getEnd() + r.endAllowance)
    {
        // Ending after end time and end allowance has elapsed
        return true;
    }
    else if (streamTime > r.time.getEnd()
             && renderingBuffer.getMagnitude (0, r.blockSizeForAudio) <= thresholdForStopping)
    {
        // Ending during end allowance period due to low magnitude
        return true;
    }

    auto prog = (float) ((streamTime - r.time.getStart()) / juce::jmax (1.0, r.time.getLength()));

    if (needsToNormaliseAndTrim)
        prog *= 0.9f;

    jassert (! std::isnan (prog));
    progressToUpdate = juce::jlimit (0.0f, 1.0f, prog);
    --precount;
    streamTime = blockEnd;

    return false;
}

//==============================================================================
NodeRenderContext::WriteResult NodeRenderContext::writeAudioBlock (juce::dsp::AudioBlock<float> block)
{
    CRASH_TRACER
    // Prepare buffer to use
    const int blockSizeSamples = (int) block.getNumSamples();
    
    float* chans[32];

    for (int i = 0; i < numOutputChans; ++i)
        chans[i] = block.getChannelPointer ((size_t) i);

    juce::AudioBuffer<float> buffer (chans, numOutputChans, blockSizeSamples);

    // Apply dithering and mag/rms analysis
    if (r.ditheringEnabled && r.bitDepth < 32)
        ditherers.apply (buffer, blockSizeSamples);

    auto mag = buffer.getMagnitude (0, blockSizeSamples);
    peak = juce::jmax (peak, mag);

    if (! hasStartedSavingToFile)
        hasStartedSavingToFile = (mag > 0.0f);

    for (int i = buffer.getNumChannels(); --i >= 0;)
    {
        rmsTotal += buffer.getRMSLevel (i, 0, blockSizeSamples);
        ++rmsNumSamps;
    }

    for (int i = blockSizeSamples; --i >= 0;)
        if (buffer.getMagnitude (i, 1) > 0.0001)
            numNonZeroSamps++;

    if (! hasStartedSavingToFile)
        samplesTrimmed += blockSizeSamples;

    // Update thumbnail source
    if (sourceToUpdate != nullptr && blockSizeSamples > 0)
    {
        sourceToUpdate->addBlock (numSamplesWrittenToSource, buffer, 0, blockSizeSamples);
        numSamplesWrittenToSource += blockSizeSamples;
    }

    // And finally write to the file
    // NB buffer gets trashed by this call
    if (blockSizeSamples > 0 && hasStartedSavingToFile
         && writer->isOpen()
         && ! writer->appendBuffer (buffer, blockSizeSamples))
        return WriteResult::failed;
    
    return WriteResult::succeeded;
}

//==============================================================================
juce::String NodeRenderContext::renderMidi (Renderer::RenderTask& owner,
                                            Renderer::Parameters& r,
                                            std::unique_ptr<tracktion_graph::Node> n,
                                            std::unique_ptr<tracktion_graph::PlayHead> playHead,
                                            std::unique_ptr<tracktion_graph::PlayHeadState> playHeadState,
                                            std::unique_ptr<ProcessState> processState,
                                            std::atomic<float>& progress)
{
    const int samplesPerBlock = r.blockSizeForAudio;
    const double sampleRate = r.sampleRateForAudio;
    const double blockLength = tracktion_graph::sampleToTime (samplesPerBlock, sampleRate);
    double streamTime = r.time.getStart();

    auto nodePlayer = std::make_unique<TracktionNodePlayer> (std::move (n), *processState,
                                                             sampleRate, samplesPerBlock,
                                                             getPoolCreatorFunction (ThreadPoolStrategy::hybrid));
    nodePlayer->setNumThreads ((size_t) r.engine->getEngineBehaviour().getNumberOfCPUsToUseForAudio() - 1);
    
    //TODO: Should really purge any non-MIDI nodes here then return if no MIDI has been found
    
    playHead->stop();
    playHead->setPosition (tracktion_graph::timeToSample (streamTime, sampleRate));
    playHead->playSyncedToRange (tracktion_graph::timeToSample ({ streamTime, Edit::maximumLength }, sampleRate));

    playHeadState->update (tracktion_graph::timeToSample ({ streamTime, streamTime + blockLength }, sampleRate));

    // Wait for any nodes to render their sources or proxies
    auto leafNodesReady = [nodes = getNodes (*nodePlayer->getNode(), VertexOrdering::postordering)]
    {
        for (auto node : nodes)
            if (node->getDirectInputNodes().empty() && ! node->isReadyToProcess())
                return false;
        
        return true;
    };
    
    while (! leafNodesReady())
    {
        juce::Thread::sleep (100);
        
        if (owner.shouldExit())
            return TRANS("Render cancelled");
    }

    // Then render the blocks
    TempoSequencePosition currentTempoPosition (r.edit->tempoSequence);
    
    juce::AudioBuffer<float> renderingBuffer (2, samplesPerBlock + 256);
    tracktion_engine::MidiMessageArray blockMidiBuffer;
    juce::MidiMessageSequence outputSequence;

    for (;;)
    {
        if (owner.shouldExit())
            return TRANS("Render cancelled");

        if (streamTime > r.time.getEnd())
            break;

        auto blockEnd = streamTime + blockLength;
        const EditTimeRange streamTimeRange (streamTime, blockEnd);
        
        // Update modifier timers
        r.edit->updateModifierTimers (streamTime, samplesPerBlock);
        
        // Then once eveything is ready, render the block
        currentTempoPosition.setTime (streamTime);

        renderingBuffer.clear();
        blockMidiBuffer.clear();
        const auto referenceSampleRange = tracktion_graph::timeToSample (streamTimeRange, sampleRate);
        juce::dsp::AudioBlock<float> destBlock (renderingBuffer.getArrayOfWritePointers(),
                                                (size_t) renderingBuffer.getNumChannels(), (size_t) referenceSampleRange.getLength());
        nodePlayer->process ({ referenceSampleRange, { destBlock, blockMidiBuffer} });

        // Set MIDI messages to beats and update final sequence
        for (auto& m : blockMidiBuffer)
        {
            TempoSequencePosition eventPos (currentTempoPosition);
            eventPos.setTime (m.getTimeStamp() + streamTime - r.time.getStart());

            outputSequence.addEvent (juce::MidiMessage (m, Edit::ticksPerQuarterNote * eventPos.getPPQTime()));
        }

        streamTime = blockEnd;

        progress = juce::jlimit (0.0f, 1.0f, (float) ((streamTime - r.time.getStart()) / r.time.getLength()));
    }
    
    playHead->stop();
    
    if (outputSequence.getNumEvents() == 0)
        return TRANS("No MIDI found to render");

    if (! Renderer::RenderTask::addMidiMetaDataAndWriteToFile (r.destFile, std::move (outputSequence), r.edit->tempoSequence))
        return TRANS("Unable to write to destination file");

    return {};
}


} // namespace tracktion_engine
