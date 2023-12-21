/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
KadenzeDelayAudioProcessor::KadenzeDelayAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
    mIsPingPongEnabled(true)
#endif
{
    addParameter(mDryWetParameter = new juce::AudioParameterFloat("drywet",
                                                                  "Dry Wet",
                                                                  0.0,
                                                                  1.0,
                                                                  0.5));
    
    addParameter(mFeedbackParameter = new juce::AudioParameterFloat("feedback",
                                                                    "Feedback",
                                                                    0.01,
                                                                    0.98,
                                                                    0.5));
    
    addParameter(mDelayTimeLeftParameter = new juce::AudioParameterFloat("delayTimeLeft",
                                                                     "Delay Time Left",
                                                                     0.01,
                                                                     MAX_DELAY_TIME,
                                                                     0.5));
    addParameter(mDelayTimeRightParameter = new juce::AudioParameterFloat("delayTimeRight",
                                                                     "Delay Time Right",
                                                                     0.01,
                                                                     MAX_DELAY_TIME,
                                                                     1.0));

    
    
    mDelayTimeLeftSmoothed = 0;
    mDelayTimeRightSmoothed = 0;
    
    mCircularBufferLeft = nullptr;
    mCircularBufferRight = nullptr;
    mCircularBufferWriteHeadLeft = 0;
    mCircularBufferWriteHeadRight = 0;

    mCircularBufferLength = 0;
    mDelayTimeLeftInSamples = 0;
    mDelayTimeRightInSamples = 0;

    mDelayReadHeadLeft = 0;
    mDelayReadHeadRight = 0;
    
    mFeedbackLeft = 0;
    mFeedbackRight = 0;
}

KadenzeDelayAudioProcessor::~KadenzeDelayAudioProcessor()
{
    if (mCircularBufferLeft != nullptr){
        delete [] mCircularBufferLeft;
        mCircularBufferLeft = nullptr;
    }
    
    if (mCircularBufferRight != nullptr){
        delete [] mCircularBufferRight;
        mCircularBufferRight = nullptr;
    }
}

//==============================================================================
const juce::String KadenzeDelayAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool KadenzeDelayAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool KadenzeDelayAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool KadenzeDelayAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double KadenzeDelayAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int KadenzeDelayAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int KadenzeDelayAudioProcessor::getCurrentProgram()
{
    return 0;
}

void KadenzeDelayAudioProcessor::setCurrentProgram (int index)
{
}

const juce::String KadenzeDelayAudioProcessor::getProgramName (int index)
{
    return {};
}

void KadenzeDelayAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

//==============================================================================
void KadenzeDelayAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    mDelayTimeLeftInSamples = sampleRate * *mDelayTimeLeftParameter;
    mDelayTimeRightInSamples = sampleRate * *mDelayTimeRightParameter;
    
    mCircularBufferLength = sampleRate * MAX_DELAY_TIME;
    
    if (mCircularBufferLeft == nullptr){
        mCircularBufferLeft = new float[mCircularBufferLength];
    }
    
    juce::zeromem(mCircularBufferLeft, mCircularBufferLength * sizeof(float));
    
    if (mCircularBufferRight == nullptr){
        mCircularBufferRight = new float[mCircularBufferLength];
    }
    
    juce::zeromem(mCircularBufferRight, mCircularBufferLength * sizeof(float));
    
    mCircularBufferWriteHeadLeft = 0;
    mCircularBufferWriteHeadRight = 0;
    
    mDelayTimeLeftSmoothed = *mDelayTimeLeftParameter;
    mDelayTimeRightSmoothed = *mDelayTimeRightParameter;

}

void KadenzeDelayAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool KadenzeDelayAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

void KadenzeDelayAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // clear any output channels that don't contain input data
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());
    
    // get pointers to the left and right channels of the audio buffer
    float* leftChannel = buffer.getWritePointer(0);
    float* rightChannel = buffer.getWritePointer(1);

    // iterate through each sample in the audio buffer
    for (int i = 0; i < buffer.getNumSamples(); i++) {
        // smooth the delay time parameter
        mDelayTimeLeftSmoothed = mDelayTimeLeftSmoothed - 0.001 * (mDelayTimeLeftSmoothed - *mDelayTimeLeftParameter);
        mDelayTimeRightSmoothed = mDelayTimeRightSmoothed - 0.001 * (mDelayTimeRightSmoothed - *mDelayTimeRightParameter);

        // calculate delay time in samples based on current smoothed delay time
        mDelayTimeLeftInSamples = getSampleRate() * mDelayTimeLeftSmoothed;
        mDelayTimeRightInSamples = getSampleRate() * mDelayTimeRightSmoothed;

        
        // write input samples to circular buffer with feedback
        mCircularBufferLeft[mCircularBufferWriteHeadLeft] = rightChannel[i] + mFeedbackRight;
        mCircularBufferRight[mCircularBufferWriteHeadRight] =  leftChannel[i] + mFeedbackLeft;
        
        // calculate read head position in circular
        mDelayReadHeadLeft = mCircularBufferWriteHeadLeft - mDelayTimeLeftInSamples;
        mDelayReadHeadRight = mCircularBufferWriteHeadRight - mDelayTimeRightInSamples;

        
        // handle wrap-around if read head position position is negative
        if (mDelayReadHeadLeft < 0 ) {
            mDelayReadHeadLeft += mCircularBufferLength;
        }
        
        if (mDelayReadHeadRight < 0) {
            mDelayReadHeadRight += mCircularBufferLength;
        }
        // intergear and fractional parts of the read head position
        int readHeadL_x = (int)mDelayReadHeadLeft;
        int readHeadL_x1 = readHeadL_x + 1;
        float readHeadFloatL = mDelayReadHeadLeft - readHeadL_x;
        
        int readHeadR_x = (int)mDelayReadHeadRight;
        int readHeadR_x1 = readHeadR_x + 1;
        float readHeadFloatR = mDelayReadHeadRight - readHeadR_x;


        // hand wrap-around for the next sample if necessary
        if (readHeadL_x1 >=mCircularBufferLength) {
            readHeadL_x1 -= mCircularBufferLength;
        }
        
        if ( readHeadR_x1 >= mCircularBufferLength) {
            readHeadR_x1 -= mCircularBufferLength;
        }
        
        // perform linear interpolation to get the delayed samples
        float delay_sample_left = lin_interp(mCircularBufferLeft[readHeadL_x], mCircularBufferLeft[readHeadL_x1], readHeadFloatL);
        float delay_sample_right = lin_interp(mCircularBufferRight[readHeadR_x], mCircularBufferRight[readHeadR_x1], readHeadFloatR);
        
        // update feedback values based on the delayed samples
        mFeedbackLeft = delay_sample_left * *mFeedbackParameter;
        mFeedbackRight = delay_sample_right * *mFeedbackParameter;
        
        // increment circular buffer write head
        mCircularBufferWriteHeadLeft++;
        mCircularBufferWriteHeadRight++;

        
        // apply dry-wet mix to the output samples and update the audio buffer
        buffer.setSample(0, i, buffer.getSample(0, i) * (1 - *mDryWetParameter) + delay_sample_left * *mDryWetParameter);
        buffer.setSample(1, i, buffer.getSample(1, i) * (1 - *mDryWetParameter) + delay_sample_right * *mDryWetParameter);
        
        // handle wrap-around for circular buffer write head
        if (mCircularBufferWriteHeadLeft >= mCircularBufferLength) {
            mCircularBufferWriteHeadLeft = 0;
        }
        
        if (mCircularBufferWriteHeadRight >= mCircularBufferLength ) {
            mCircularBufferWriteHeadRight = 0;
        }
    }
}

//==============================================================================
bool KadenzeDelayAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* KadenzeDelayAudioProcessor::createEditor()
{
    return new KadenzeDelayAudioProcessorEditor (*this);
}

//==============================================================================
void KadenzeDelayAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // You should use this method to store your parameters in the memory block.
    // You could do that either as raw data, or use the XML or ValueTree classes
    // as intermediaries to make it easy to save and load complex data.
}

void KadenzeDelayAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // You should use this method to restore your parameters from this memory block,
    // whose contents will have been created by the getStateInformation() call.
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KadenzeDelayAudioProcessor();
}

float KadenzeDelayAudioProcessor::lin_interp(float sample_x, float sample_x1, float inPhase)
{
    return (1-inPhase) * sample_x + inPhase * sample_x1;
}
