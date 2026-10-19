#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
constexpr int defaultBlockSize = 512;
constexpr int defaultTimeoutSeconds = 30;

struct HostError final : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

std::string utf8 (const juce::String& text)
{
    return text.toStdString();
}

juce::var makeObject()
{
    return juce::var (new juce::DynamicObject());
}

void set (juce::var& object, const juce::Identifier& key, juce::var value)
{
    object.getDynamicObject()->setProperty (key, std::move (value));
}

juce::String parameterId (const juce::AudioProcessorParameter& parameter)
{
    if (const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*> (&parameter))
        return hosted->getParameterID();

    return {};
}

class PluginLoader final
{
public:
    PluginLoader()
    {
        juce::addDefaultFormatsToManager (manager);
    }

    juce::PluginDescription scan (const juce::String& path)
    {
        std::cerr << "ReferencePluginHost: scanning " << utf8 (path) << '\n';
        juce::OwnedArray<juce::PluginDescription> found;
        juce::StringArray attempted;

        for (auto* format : manager.getFormats())
        {
            if (! format->fileMightContainThisPluginType (path))
                continue;

            attempted.add (format->getName());
            format->findAllTypesForFile (found, path);
        }

        if (found.isEmpty())
            throw HostError ("No plug-in type found at '" + utf8 (path)
                             + "' (formats tried: " + utf8 (attempted.joinIntoString (", ")) + ")");

        std::cerr << "ReferencePluginHost: found " << utf8 (found.getFirst()->name) << '\n';
        return *found.getFirst();
    }

    std::unique_ptr<juce::AudioPluginInstance> instantiate (const juce::PluginDescription& description,
                                                            double sampleRate,
                                                            int blockSize)
    {
        std::cerr << "ReferencePluginHost: instantiating at " << sampleRate << " Hz\n";
        juce::String error;
        std::unique_ptr<juce::AudioPluginInstance> instance;
        bool completed = false;
        manager.createPluginInstanceAsync (
            description, sampleRate, blockSize,
            [&instance, &error, &completed] (std::unique_ptr<juce::AudioPluginInstance> created,
                                             const juce::String& message)
            {
                instance = std::move (created);
                error = message;
                completed = true;
#if ! JUCE_MAC
                juce::MessageManager::getInstance()->stopDispatchLoop();
#endif
            });

#if JUCE_MAC
        while (! completed)
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
#else
        juce::MessageManager::getInstance()->runDispatchLoop();
#endif
        if (! completed)
            throw HostError ("Plug-in creation event loop exited without a result");

        if (instance == nullptr)
            throw HostError ("Plug-in instantiation failed: " + utf8 (error));

        std::cerr << "ReferencePluginHost: instantiated at " << sampleRate << " Hz\n";
        return instance;
    }

private:
    juce::AudioPluginFormatManager manager;
};

juce::var inspectParameter (const juce::AudioProcessorParameter& parameter, int index)
{
    auto result = makeObject();
    const auto defaultValue = parameter.getDefaultValue();
    const auto numSteps = parameter.getNumSteps();

    set (result, "index", index);
    set (result, "id", parameterId (parameter));
    set (result, "name", parameter.getName (1024));
    set (result, "label", parameter.getLabel());
    set (result, "normalized_min", 0.0);
    set (result, "normalized_max", 1.0);
    set (result, "default_normalized", static_cast<double> (defaultValue));
    set (result, "default_text", parameter.getText (defaultValue, 1024));
    set (result, "min_text", parameter.getText (0.0f, 1024));
    set (result, "max_text", parameter.getText (1.0f, 1024));
    set (result, "num_steps", numSteps);
    set (result, "is_discrete", parameter.isDiscrete());
    set (result, "is_boolean", parameter.isBoolean());
    set (result, "is_automatable", parameter.isAutomatable());
    set (result, "is_meta", parameter.isMetaParameter());

    if (parameter.isDiscrete() && numSteps > 1 && numSteps <= 128)
    {
        juce::Array<juce::var> values;
        values.ensureStorageAllocated (numSteps);
        for (int step = 0; step < numSteps; ++step)
        {
            const auto normalized = static_cast<float> (step) / static_cast<float> (numSteps - 1);
            auto value = makeObject();
            set (value, "normalized", static_cast<double> (normalized));
            set (value, "text", parameter.getText (normalized, 1024));
            values.add (std::move (value));
        }
        set (result, "values", std::move (values));
    }

    return result;
}

juce::var inspectPreparedInstance (juce::AudioPluginInstance& instance,
                                   double sampleRate,
                                   int blockSize,
                                   bool includeParameters)
{
    instance.setNonRealtime (true);
    instance.setPlayHead (nullptr);
    instance.setRateAndBufferSizeDetails (sampleRate, blockSize);
    instance.prepareToPlay (sampleRate, blockSize);

    const auto channels = juce::jmax (1, juce::jmax (instance.getTotalNumInputChannels(),
                                                     instance.getTotalNumOutputChannels()));
    juce::AudioBuffer<float> silence (channels, blockSize);
    silence.clear();
    juce::MidiBuffer midi;
    instance.processBlock (silence, midi);

    bool finite = true;
    for (int channel = 0; channel < silence.getNumChannels(); ++channel)
        for (int sample = 0; sample < silence.getNumSamples(); ++sample)
            finite = finite && std::isfinite (silence.getSample (channel, sample));

    auto result = makeObject();
    set (result, "sample_rate", sampleRate);
    set (result, "block_size", blockSize);
    set (result, "latency_samples", instance.getLatencySamples());
    set (result, "silence_process_finite", finite);

    if (includeParameters)
    {
        juce::Array<juce::var> parameters;
        const auto& source = instance.getParameters();
        parameters.ensureStorageAllocated (source.size());
        for (int index = 0; index < source.size(); ++index)
            parameters.add (inspectParameter (*source[index], index));
        set (result, "parameters", std::move (parameters));
    }

    instance.releaseResources();
    return result;
}

juce::var inspectPlugin (PluginLoader& loader, const juce::String& path, int blockSize)
{
    const auto description = loader.scan (path);
    auto result = makeObject();
    set (result, "path", path);
    set (result, "name", description.name);
    set (result, "descriptive_name", description.descriptiveName);
    set (result, "manufacturer", description.manufacturerName);
    set (result, "version", description.version);
    set (result, "format", description.pluginFormatName);
    set (result, "category", description.category);
    set (result, "identifier", description.createIdentifierString());
    set (result, "licensing", "The public plug-in host API exposes no licence status. Successful load and finite headless processing are reported, but cannot prove an unrestricted licence.");
    set (result, "transport", "disabled (AudioPlayHead is null)");

    juce::Array<juce::var> preparations;
    auto instance = loader.instantiate (description, 44100.0, blockSize);
    for (const auto sampleRate : { 44100.0, 48000.0 })
    {
        if (preparations.isEmpty())
        {
            set (result, "input_channels", instance->getTotalNumInputChannels());
            set (result, "output_channels", instance->getTotalNumOutputChannels());
            set (result, "has_editor", instance->hasEditor());
        }
        preparations.add (inspectPreparedInstance (*instance, sampleRate, blockSize,
                                                   preparations.isEmpty()));
    }
    set (result, "preparations", std::move (preparations));
    return result;
}

juce::String normalizedKey (juce::String text)
{
    text = text.toLowerCase();
    juce::String result;
    for (const auto character : text)
        if (juce::CharacterFunctions::isLetterOrDigit (character))
            result += character;
    return result;
}

juce::AudioProcessorParameter* findParameter (juce::AudioPluginInstance& instance,
                                              const juce::String& requested)
{
    juce::Array<juce::AudioProcessorParameter*> exact;
    juce::Array<juce::AudioProcessorParameter*> normalized;
    const auto wantedNormalized = normalizedKey (requested);

    for (auto* parameter : instance.getParameters())
    {
        const auto id = parameterId (*parameter);
        const auto name = parameter->getName (1024);
        if (id == requested || name.equalsIgnoreCase (requested))
            exact.add (parameter);
        else if (normalizedKey (id) == wantedNormalized || normalizedKey (name) == wantedNormalized)
            normalized.add (parameter);
    }

    const auto& matches = exact.isEmpty() ? normalized : exact;
    if (matches.isEmpty())
        throw HostError ("No parameter matches '" + utf8 (requested) + "'");
    if (matches.size() != 1)
        throw HostError ("Ambiguous parameter key '" + utf8 (requested) + "'");
    return matches.getFirst();
}

struct ParameterSetting
{
    juce::String key;
    float normalizedValue = 0.0f;
};

struct RenderOptions
{
    juce::String pluginPath;
    juce::File inputFile;
    juce::File outputFile;
    int blockSize = defaultBlockSize;
    double tailSeconds = 0.0;
    bool trimLatency = false;
    bool overwrite = false;
    std::vector<ParameterSetting> settings;
};

bool filesReferToSameLocation (const juce::File& lhs, const juce::File& rhs)
{
    if (lhs == rhs)
        return true;

    if (! lhs.exists() || ! rhs.exists())
        return false;

    std::error_code error;
    const auto same = std::filesystem::equivalent (
        std::filesystem::path (lhs.getFullPathName().toStdString()),
        std::filesystem::path (rhs.getFullPathName().toStdString()), error);
    return ! error && same;
}

ParameterSetting parseSetting (const juce::String& specification)
{
    const auto separator = specification.lastIndexOfChar ('=');
    if (separator <= 0 || separator == specification.length() - 1)
        throw HostError ("Parameter setting must be KEY=NORMALIZED: " + utf8 (specification));

    const auto key = specification.substring (0, separator).trim();
    const auto valueText = specification.substring (separator + 1).trim();
    double value = 0.0;
    try
    {
        std::size_t parsed = 0;
        const auto utf8Value = valueText.toStdString();
        value = std::stod (utf8Value, &parsed);
        if (parsed != utf8Value.size())
            throw HostError ("Invalid normalized parameter value: " + utf8 (specification));
    }
    catch (const std::invalid_argument&)
    {
        throw HostError ("Invalid normalized parameter value: " + utf8 (specification));
    }
    catch (const std::out_of_range&)
    {
        throw HostError ("Normalized parameter value is out of range: " + utf8 (specification));
    }
    if (! std::isfinite (value) || value < 0.0 || value > 1.0)
        throw HostError ("Normalized parameter value must be in [0, 1]: " + utf8 (specification));

    return { key, static_cast<float> (value) };
}

juce::var renderPlugin (PluginLoader& loader, const RenderOptions& options)
{
    if (filesReferToSameLocation (options.inputFile, options.outputFile))
        throw HostError ("Input and output must refer to different files");
    if (options.outputFile.exists() && ! options.overwrite)
        throw HostError ("Output already exists; pass --overwrite to replace it safely: "
                         + utf8 (options.outputFile.getFullPathName()));
    if (! options.outputFile.getParentDirectory().createDirectory())
        throw HostError ("Cannot create output directory: "
                         + utf8 (options.outputFile.getParentDirectory().getFullPathName()));

    juce::AudioFormatManager audioFormats;
    audioFormats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (audioFormats.createReaderFor (options.inputFile));
    if (reader == nullptr)
        throw HostError ("Cannot read input audio: " + utf8 (options.inputFile.getFullPathName()));
    if (reader->lengthInSamples < 0)
        throw HostError ("Input audio has an invalid length");

    const auto description = loader.scan (options.pluginPath);
    auto instance = loader.instantiate (description, reader->sampleRate, options.blockSize);
    const auto inputChannels = instance->getTotalNumInputChannels();
    const auto outputChannels = instance->getTotalNumOutputChannels();
    if (inputChannels <= 0 || outputChannels <= 0)
        throw HostError ("Only audio effects with input and output channels are supported");
    if (static_cast<int> (reader->numChannels) != inputChannels || inputChannels != outputChannels)
        throw HostError ("WAV channels must equal the plug-in's input/output channels (WAV "
                         + std::to_string (reader->numChannels) + ", plug-in "
                         + std::to_string (inputChannels) + " in/" + std::to_string (outputChannels)
                         + " out)");

    juce::Array<juce::var> appliedSettings;
    for (const auto& setting : options.settings)
    {
        auto* parameter = findParameter (*instance, setting.key);
        parameter->setValueNotifyingHost (setting.normalizedValue);

        auto applied = makeObject();
        set (applied, "requested_key", setting.key);
        set (applied, "id", parameterId (*parameter));
        set (applied, "name", parameter->getName (1024));
        set (applied, "normalized", static_cast<double> (parameter->getValue()));
        set (applied, "text", parameter->getCurrentValueAsText());
        appliedSettings.add (std::move (applied));
    }

    instance->setNonRealtime (true);
    instance->setPlayHead (nullptr);
    instance->setRateAndBufferSizeDetails (reader->sampleRate, options.blockSize);
    instance->prepareToPlay (reader->sampleRate, options.blockSize);
    const auto latencySamples = instance->getLatencySamples();

    const auto tailSamples = static_cast<juce::int64> (
        std::ceil (options.tailSeconds * reader->sampleRate));
    const auto trimSamples = options.trimLatency ? static_cast<juce::int64> (latencySamples) : 0;
    const auto framesToProcess = reader->lengthInSamples + tailSamples + trimSamples;
    const auto expectedOutputFrames = framesToProcess - trimSamples;

    juce::TemporaryFile temporaryOutput (options.outputFile);
    std::unique_ptr<juce::OutputStream> outputStream = temporaryOutput.getFile().createOutputStream();
    if (outputStream == nullptr)
        throw HostError ("Cannot open output audio: " + utf8 (options.outputFile.getFullPathName()));

    juce::WavAudioFormat wav;
    const auto writerOptions = juce::AudioFormatWriterOptions()
        .withSampleRate (reader->sampleRate)
        .withNumChannels (outputChannels)
        .withBitsPerSample (32)
        .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = wav.createWriterFor (outputStream, writerOptions);
    if (writer == nullptr)
        throw HostError ("Cannot create 32-bit float WAV writer");

    juce::AudioBuffer<float> buffer (outputChannels, options.blockSize);
    juce::MidiBuffer midi;
    juce::int64 processed = 0;
    juce::int64 written = 0;
    while (processed < framesToProcess)
    {
        const auto block = static_cast<int> (
            juce::jmin<juce::int64> (options.blockSize, framesToProcess - processed));
        buffer.setSize (outputChannels, block, false, false, true);
        buffer.clear();

        if (processed < reader->lengthInSamples)
        {
            const auto readable = static_cast<int> (juce::jmin<juce::int64> (
                block, reader->lengthInSamples - processed));
            if (! reader->read (&buffer, 0, readable, processed, true, true))
                throw HostError ("Failed while reading input audio");
        }

        midi.clear();
        instance->processBlock (buffer, midi);

        const auto skip = static_cast<int> (juce::jlimit<juce::int64> (
            0, block, trimSamples - processed));
        const auto samplesToWrite = block - skip;
        if (samplesToWrite > 0
            && ! writer->writeFromAudioSampleBuffer (buffer, skip, samplesToWrite))
            throw HostError ("Failed while writing output audio");
        written += samplesToWrite;
        processed += block;
    }

    instance->releaseResources();
    writer.reset();

    if (written != expectedOutputFrames)
        throw HostError ("Internal output-length mismatch");
    if (! temporaryOutput.overwriteTargetFileWithTemporary())
        throw HostError ("Completed render could not replace output: "
                         + utf8 (options.outputFile.getFullPathName()));

    auto result = makeObject();
    set (result, "plugin", description.name);
    set (result, "plugin_path", options.pluginPath);
    set (result, "input", options.inputFile.getFullPathName());
    set (result, "output", options.outputFile.getFullPathName());
    set (result, "sample_rate", reader->sampleRate);
    set (result, "channels", outputChannels);
    set (result, "input_frames", static_cast<double> (reader->lengthInSamples));
    set (result, "output_frames", static_cast<double> (written));
    set (result, "block_size", options.blockSize);
    set (result, "reported_latency_samples", latencySamples);
    set (result, "latency_trimmed", options.trimLatency);
    set (result, "tail_seconds", options.tailSeconds);
    set (result, "tail_samples", static_cast<double> (tailSamples));
    set (result, "transport", "disabled (AudioPlayHead is null)");
    set (result, "applied_settings", std::move (appliedSettings));
    return result;
}

class ProcessWatchdog final
{
public:
    explicit ProcessWatchdog (int timeoutSeconds)
        : worker ([this, timeoutSeconds]
          {
              std::unique_lock lock (mutex);
              if (condition.wait_for (lock, std::chrono::seconds (timeoutSeconds),
                                      [this] { return finished; }))
                  return;
              std::cerr << "ReferencePluginHost: timed out after " << timeoutSeconds
                        << " seconds (plug-in scan/load/process may be blocked)\n";
              std::_Exit (124);
          })
    {
    }

    ~ProcessWatchdog()
    {
        {
            std::lock_guard lock (mutex);
            finished = true;
        }
        condition.notify_one();
        worker.join();
    }

private:
    std::mutex mutex;
    std::condition_variable condition;
    bool finished = false;
    std::thread worker;
};

void printUsage()
{
    std::cerr
        << "Usage:\n"
        << "  ReferencePluginHost --inspect PLUGIN_PATH [--block-size N]\n"
        << "  ReferencePluginHost --render PLUGIN_PATH INPUT.wav OUTPUT.wav [options]\n"
        << "\nRender options:\n"
        << "  --set ID_OR_NAME=NORMALIZED   Set a parameter in [0, 1] (repeatable)\n"
        << "  --tail-seconds N              Append zero input for effect decay\n"
        << "  --trim-latency                Drop the reported initial latency\n"
        << "  --block-size N                Offline process block size (default 512)\n"
        << "  --overwrite                   Replace an existing output after render succeeds\n"
        << "\nThe process exits 124 after 30 seconds so a blocked third-party loader is bounded.\n"
        << "Without --trim-latency, output is input + tail and retains plug-in delay.\n"
        << "With --trim-latency, the host processes extra latency samples, drops the initial\n"
        << "reported latency, and still writes exactly input + tail frames. Transport is null.\n";
}

int run (const juce::StringArray& arguments)
{
    if (arguments.isEmpty() || arguments[0] == "--help" || arguments[0] == "-h")
    {
        printUsage();
        return arguments.isEmpty() ? 2 : 0;
    }

    PluginLoader loader;
    if (arguments[0] == "--inspect")
    {
        if (arguments.size() < 2)
            throw HostError ("--inspect requires a plug-in path");

        auto blockSize = defaultBlockSize;
        for (int index = 2; index < arguments.size(); ++index)
        {
            if (arguments[index] == "--block-size" && index + 1 < arguments.size())
                blockSize = arguments[++index].getIntValue();
            else
                throw HostError ("Unknown argument: " + utf8 (arguments[index]));
        }
        if (blockSize <= 0)
            throw HostError ("Block size must be positive");

        std::cout << utf8 (juce::JSON::toString (
            inspectPlugin (loader, arguments[1], blockSize), true)) << '\n';
        return 0;
    }

    if (arguments[0] == "--render")
    {
        if (arguments.size() < 4)
            throw HostError ("--render requires PLUGIN_PATH INPUT.wav OUTPUT.wav");
        RenderOptions options;
        options.pluginPath = arguments[1];
        options.inputFile = juce::File (arguments[2]);
        options.outputFile = juce::File (arguments[3]);
        for (int index = 4; index < arguments.size(); ++index)
        {
            if (arguments[index] == "--set" && index + 1 < arguments.size())
                options.settings.push_back (parseSetting (arguments[++index]));
            else if (arguments[index] == "--tail-seconds" && index + 1 < arguments.size())
                options.tailSeconds = arguments[++index].getDoubleValue();
            else if (arguments[index] == "--block-size" && index + 1 < arguments.size())
                options.blockSize = arguments[++index].getIntValue();
            else if (arguments[index] == "--trim-latency")
                options.trimLatency = true;
            else if (arguments[index] == "--overwrite")
                options.overwrite = true;
            else
                throw HostError ("Unknown argument: " + utf8 (arguments[index]));
        }
        if (options.blockSize <= 0)
            throw HostError ("Block size must be positive");
        if (! std::isfinite (options.tailSeconds) || options.tailSeconds < 0.0)
            throw HostError ("Tail seconds must be finite and non-negative");

        std::cout << utf8 (juce::JSON::toString (renderPlugin (loader, options), true)) << '\n';
        return 0;
    }

    throw HostError ("First argument must be --inspect or --render");
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    ProcessWatchdog watchdog (defaultTimeoutSeconds);
    juce::StringArray arguments;
    for (int index = 1; index < argc; ++index)
        arguments.add (juce::String::fromUTF8 (argv[index]));

    try
    {
        return run (arguments);
    }
    catch (const std::exception& error)
    {
        std::cerr << "ReferencePluginHost: " << error.what() << '\n';
        return 1;
    }
}
