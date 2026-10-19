#include <cmath>
#include <cstdio>
#include <memory>
#include <atomic>
#include <thread>

#include <juce_gui_basics/juce_gui_basics.h>

#include "plugins/spectral/PluginProcessor.h"
#include "plugins/spectral/SpectralWorkflow.h"
#include "plugins/spectral/ui/SpectralAnalyzerView.h"
#include "plugins/spectral/ui/WeightBandLayer.h"

namespace {
int failures = 0;

void check(bool condition, const char* message) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

void checkNear(float actual, float expected, const char* message) {
    check(std::abs(actual - expected) < 1.0e-5f, message);
}

void setParam(SpectralProcessor& processor, const char* id, float value) {
    // Explicit UI boundary; worker calls remain host automation and return
    // from the workflow entry points before taking any workflow lock.
    processor.workflow.beginUiEdit();
    processor.workflow.markUiParameter(id);
    auto* parameter = processor.apvts.getParameter(id);
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    parameter->endChangeGesture();
    processor.workflow.endUiEdit();
    processor.apvts.copyState();
}

float getParam(const SpectralProcessor& processor, const char* id) {
    return processor.apvts.getRawParameterValue(id)->load();
}

float valueInState(const juce::ValueTree& parameters, const char* id) {
    for (const auto& child : parameters)
        if (child["id"].toString() == id) return static_cast<float>(child["value"]);
    return -1000.0f;
}

juce::ValueTree parameterInState(const juce::ValueTree& parameters, const char* id) {
    for (const auto& child : parameters)
        if (child["id"].toString() == id) return child;
    return {};
}

void removeParameterRecursively(juce::ValueTree tree, const char* id) {
    if (tree.hasType("PARAMETERS")) {
        for (int i = tree.getNumChildren(); --i >= 0;)
            if (tree.getChild(i)["id"].toString() == id)
                tree.removeChild(i, nullptr);
    }
    for (int i = 0; i < tree.getNumChildren(); ++i)
        removeParameterRecursively(tree.getChild(i), id);
}

void restoreHostTree(SpectralProcessor& processor, const juce::ValueTree& tree) {
    juce::MemoryBlock bytes;
    if (auto xml = tree.createXml())
        juce::AudioProcessor::copyXmlToBinary(*xml, bytes);
    processor.setStateInformation(bytes.getData(), static_cast<int>(bytes.getSize()));
}

struct ParameterCounter final : juce::AudioProcessorParameter::Listener {
    void parameterValueChanged(int, float) override { ++valueChanges; }
    void parameterGestureChanged(int, bool) override {}
    int valueChanges = 0;
};

void testDetectionModeParameterContract() {
    SpectralProcessor processor;
    auto* parameter = processor.apvts.getParameter("detectionMode");
    check(parameter != nullptr, "detectionMode exists on the live APVTS");
    if (parameter == nullptr) return;
    const auto* choice = dynamic_cast<juce::AudioParameterChoice*>(parameter);
    check(choice != nullptr && choice->choices == juce::StringArray{"soft", "hard"},
          "detectionMode exposes Soft/Hard choice indices");
    checkNear(processor.apvts.getRawParameterValue("detectionMode")->load(), 0.0f,
              "detectionMode defaults Soft");
    const int modeIndex = parameter->getParameterIndex();
    check(modeIndex >= 0 && modeIndex + 2 < processor.getParameters().size()
              && processor.getParameters()[modeIndex + 1] == processor.apvts.getParameter("externalSidechain")
              && processor.getParameters()[modeIndex + 2] == processor.apvts.getParameter("sidechainListen"),
          "sidechain parameters append after detectionMode without reordering existing ids");
}

void testLegacyBareStateAddsSoftWithoutRenormalisingExistingParameters() {
    SpectralProcessor source;
    setParam(source, "depth", 7.25f);
    setParam(source, "attack", 73.4f);
    setParam(source, "output", -3.2f);
    setParam(source, "msMode", 1.0f);
    setParam(source, "wband2Type", 6.0f);
    setParam(source, "wband2Freq", 4321.0f);
    auto legacy = source.apvts.copyState();
    removeParameterRecursively(legacy, "detectionMode");
    removeParameterRecursively(legacy, "externalSidechain");
    removeParameterRecursively(legacy, "sidechainListen");

    SpectralProcessor restored;
    setParam(restored, "detectionMode", 1.0f);
    setParam(restored, "externalSidechain", 1.0f);
    setParam(restored, "sidechainListen", 1.0f);
    restoreHostTree(restored, legacy);
    checkNear(getParam(restored, "detectionMode"), 0.0f,
              "legacy bare state inserts Soft detection mode");
    checkNear(getParam(restored, "externalSidechain"), 0.0f, "legacy bare state disables external sidechain");
    checkNear(getParam(restored, "sidechainListen"), 0.0f, "legacy bare state disables sidechain listen");
    for (auto* sourceParameter : source.getParameters()) {
        auto* sourceRanged = dynamic_cast<juce::RangedAudioParameter*>(sourceParameter);
        if (sourceRanged == nullptr || sourceRanged->paramID == "detectionMode") continue;
        auto* restoredParameter = restored.apvts.getParameter(sourceRanged->paramID);
        check(restoredParameter != nullptr
                  && std::abs(restoredParameter->getValue() - sourceRanged->getValue()) < 1.0e-6f,
              ("legacy state preserves normalized " + sourceRanged->paramID).toRawUTF8());
    }
}

void testLegacyWrappedStateMigratesBothSlotsAndCleanSnapshots() {
    SpectralProcessor source;
    const auto presetA = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getNonexistentChildFile("fixxy-legacy-slot-a", ".fixxyspectral", false);
    const auto presetB = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getNonexistentChildFile("fixxy-legacy-slot-b", ".fixxyspectral", false);
    source.workflow.createBand(0, 120.0f, 0.0f);
    setParam(source, "wband0On", 0.0f);
    setParam(source, "depth", 2.0f);
    check(source.workflow.savePreset(presetA).wasOk(), "legacy slot A identity saves");
    setParam(source, "depth", 3.0f);
    source.workflow.selectSlot(SpectralWorkflow::Slot::b);
    source.workflow.createBand(1, 1800.0f, 2.0f);
    setParam(source, "wband1On", 0.0f);
    setParam(source, "depth", 8.0f);
    check(source.workflow.savePreset(presetB).wasOk(), "legacy slot B identity saves");
    setParam(source, "output", -2.0f);
    source.workflow.setSidebarExpanded(false);
    juce::ValueTree legacy("LPNGSPECTRAL_STATE");
    legacy.copyPropertiesAndChildrenFrom(source.workflow.createProcessorState(), nullptr);
    legacy.setProperty("stateVersion", 1, nullptr);
    removeParameterRecursively(legacy, "detectionMode");
    removeParameterRecursively(legacy, "externalSidechain");
    removeParameterRecursively(legacy, "sidechainListen");

    SpectralProcessor restored;
    setParam(restored, "detectionMode", 1.0f);
    check(restored.workflow.restoreProcessorState(legacy), "legacy wrapped state is accepted");
    checkNear(getParam(restored, "detectionMode"), 0.0f, "legacy active B snapshot migrates to Soft");
    check(restored.workflow.activeSlot() == SpectralWorkflow::Slot::b,
          "legacy wrapper retains active B identity");
    check(!restored.workflow.isSidebarExpanded(), "legacy wrapper retains sidebar metadata");
    check(restored.workflow.currentPresetName() == presetB.getFileNameWithoutExtension()
              && restored.workflow.isCurrentPresetDirty() && restored.workflow.isBandPresent(1)
              && !restored.workflow.isBandPresent(0),
          "legacy wrapper retains B preset clean/dirty identity and bypassed presence");
    restored.workflow.selectSlot(SpectralWorkflow::Slot::a);
    checkNear(getParam(restored, "detectionMode"), 0.0f, "legacy inactive A snapshot migrates to Soft");
    checkNear(getParam(restored, "depth"), 3.0f, "legacy inactive A values survive migration");
    check(restored.workflow.currentPresetName() == presetA.getFileNameWithoutExtension()
              && restored.workflow.isCurrentPresetDirty() && restored.workflow.isBandPresent(0)
              && !restored.workflow.isBandPresent(1),
          "legacy wrapper migrates A clean snapshot without changing dirty identity or presence");
    setParam(restored, "depth", 2.0f);
    check(!restored.workflow.isCurrentPresetDirty(), "legacy A clean values and presence survive migration");
    restored.workflow.selectSlot(SpectralWorkflow::Slot::b);
    setParam(restored, "output", 0.0f);
    check(!restored.workflow.isCurrentPresetDirty(), "legacy B clean values and presence survive migration");
    check(presetA.deleteFile(), "legacy slot A preset removed");
    check(presetB.deleteFile(), "legacy slot B preset removed");
}

void testLegacyPresetsAddSoftAndModernHardRoundTrips() {
    SpectralProcessor source;
    check(juce::String(spectralstate::kPresetExtension) == ".fixxyspectral",
          "new preset file default extension is .fixxyspectral");
    check(juce::String(spectralstate::kLegacyPresetExtension) == ".lpngspectral",
          "legacy preset file extension remains available for reads");
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto legacyV1 = directory.getNonexistentChildFile("fixxy-legacy-v1", ".lpngspectral", false);
    const auto legacyUnversioned = directory.getNonexistentChildFile("fixxy-legacy-unversioned", ".lpngspectral", false);
    const auto legacyModern = directory.getNonexistentChildFile("fixxy-legacy-modern", ".lpngspectral", false);
    const auto modern = directory.getNonexistentChildFile("fixxy-modern-hard", ".fixxyspectral", false);
    setParam(source, "depth", 4.0f);
    check(source.workflow.savePreset(legacyV1).wasOk(), "legacy v1 source preset saves");
    auto legacyXml = juce::XmlDocument::parse(legacyV1);
    auto legacyTree = juce::ValueTree::fromXml(*legacyXml);
    legacyTree.setProperty("presetFormat", "LpngSpectral", nullptr);
    legacyTree.setProperty("presetVersion", 1, nullptr);
    removeParameterRecursively(legacyTree, "detectionMode");
    removeParameterRecursively(legacyTree, "externalSidechain");
    removeParameterRecursively(legacyTree, "sidechainListen");
    check(legacyV1.replaceWithText(legacyTree.createXml()->toString()), "legacy v1 fixture writes");
    legacyTree.removeProperty("presetVersion", nullptr);
    check(legacyUnversioned.replaceWithText(legacyTree.createXml()->toString()),
          "legacy unversioned fixture writes");

    SpectralProcessor target;
    setParam(target, "detectionMode", 1.0f);
    setParam(target, "externalSidechain", 1.0f);
    setParam(target, "sidechainListen", 1.0f);
    check(target.workflow.loadPreset(legacyV1).wasOk(), "legacy v1 preset loads");
    checkNear(getParam(target, "detectionMode"), 0.0f, "legacy v1 preset defaults Soft");
    checkNear(getParam(target, "externalSidechain"), 0.0f, "legacy v1 preset defaults SC off");
    checkNear(getParam(target, "sidechainListen"), 0.0f, "legacy v1 preset defaults listen off");
    setParam(target, "detectionMode", 1.0f);
    setParam(target, "externalSidechain", 1.0f);
    setParam(target, "sidechainListen", 1.0f);
    check(target.workflow.loadPreset(legacyUnversioned).wasOk(), "legacy unversioned preset loads");
    checkNear(getParam(target, "detectionMode"), 0.0f, "legacy unversioned preset defaults Soft");
    checkNear(getParam(target, "externalSidechain"), 0.0f, "unversioned preset defaults SC off");
    checkNear(getParam(target, "sidechainListen"), 0.0f, "unversioned preset defaults listen off");

    setParam(source, "detectionMode", 1.0f);
    check(source.workflow.savePreset(modern).wasOk(), "modern Hard preset saves");
    auto modernXml = juce::XmlDocument::parse(modern);
    const auto modernTree = juce::ValueTree::fromXml(*modernXml);
    check(modernTree["presetFormat"].toString() == "FixxySpectral",
          "new presets declare the FixxySpectral brand");
    check(static_cast<int>(modernTree["presetVersion"]) == 3,
          "new presets declare format version 3");
    setParam(target, "detectionMode", 0.0f);
    check(target.workflow.loadPreset(modern).wasOk(), "modern Hard preset loads");
    checkNear(getParam(target, "detectionMode"), 1.0f, "modern preset recalls Hard");
    for (int version : {2, 3}) {
        auto legacyModernTree = modernTree.createCopy();
        legacyModernTree.setProperty("presetFormat", "LpngSpectral", nullptr);
        legacyModernTree.setProperty("presetVersion", version, nullptr);
        check(legacyModern.replaceWithText(legacyModernTree.createXml()->toString()),
              "legacy-branded modern preset fixture writes with the legacy extension");
        setParam(target, "detectionMode", 0.0f);
        check(target.workflow.loadPreset(legacyModern).wasOk(), "legacy-branded version 2/3 preset loads");
        checkNear(getParam(target, "detectionMode"), 1.0f, "legacy-branded version 2/3 preset recalls Hard");
    }

    source.workflow.copyActiveToOther();
    source.workflow.selectSlot(SpectralWorkflow::Slot::b);
    setParam(source, "detectionMode", 0.0f);
    source.workflow.selectSlot(SpectralWorkflow::Slot::a);
    auto state = source.workflow.createProcessorState();
    check(state.hasType("FIXXY_SPECTRAL_STATE"), "new host state declares the Fixxy processor root");
    check(static_cast<int>(state["stateVersion"]) == 3,
          "new host state declares format version 3");
    SpectralProcessor hostRestored;
    check(hostRestored.workflow.restoreProcessorState(state), "modern host state restores");
    checkNear(getParam(hostRestored, "detectionMode"), 1.0f, "modern active A recalls Hard");
    hostRestored.workflow.selectSlot(SpectralWorkflow::Slot::b);
    checkNear(getParam(hostRestored, "detectionMode"), 0.0f, "modern inactive B recalls Soft");

    check(legacyV1.deleteFile(), "legacy v1 preset removed");
    check(legacyUnversioned.deleteFile(), "legacy unversioned preset removed");
    check(legacyModern.deleteFile(), "legacy-branded modern preset removed");
    check(modern.deleteFile(), "modern Hard preset removed");
}

void testMalformedModernModeIsRejectedAtomically() {
    SpectralProcessor source;
    auto modernState = source.workflow.createProcessorState();
    modernState.setProperty("stateVersion", 2, nullptr);
    removeParameterRecursively(modernState.getChildWithName("PARAMETERS"), "detectionMode");
    auto changedDepth = parameterInState(modernState.getChildWithName("PARAMETERS"), "depth");
    changedDepth.setProperty("value", 2.0f, nullptr);

    SpectralProcessor target;
    setParam(target, "depth", 7.0f);
    setParam(target, "detectionMode", 1.0f);
    check(!target.workflow.restoreProcessorState(modernState),
          "modern host state missing detectionMode is rejected");
    checkNear(getParam(target, "depth"), 7.0f, "rejected host state cannot partially change depth");
    checkNear(getParam(target, "detectionMode"), 1.0f, "rejected host state cannot change mode");
    juce::ValueTree legacyMalformed("LPNGSPECTRAL_STATE");
    legacyMalformed.copyPropertiesAndChildrenFrom(modernState, nullptr);
    check(!target.workflow.restoreProcessorState(legacyMalformed),
          "legacy-branded version 2 host state missing detectionMode is rejected");
    checkNear(getParam(target, "depth"), 7.0f, "legacy-branded rejected host state cannot change depth");
    for (const auto* snapshot : {"CURRENT", "CLEAN"}) {
        auto incomplete = source.workflow.createProcessorState();
        incomplete.setProperty("stateVersion", 2, nullptr);
        auto parameters = incomplete.getChildWithName("WORKFLOW").getChildWithName("SLOT_B")
                                   .getChildWithName(snapshot).getChildWithName("PARAMETERS");
        removeParameterRecursively(parameters, "detectionMode");
        check(!target.workflow.restoreProcessorState(incomplete), "modern inactive/clean missing mode rejected before mutation");
        checkNear(getParam(target, "depth"), 7.0f, "snapshot rejection preserves active parameters");
    }

    const auto preset = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getNonexistentChildFile("fixxy-modern-invalid-mode", ".fixxyspectral", false);
    check(source.workflow.savePreset(preset).wasOk(), "modern invalid-mode source preset saves");
    auto xml = juce::XmlDocument::parse(preset);
    auto tree = juce::ValueTree::fromXml(*xml);
    tree.setProperty("presetVersion", 2, nullptr);
    removeParameterRecursively(tree, "detectionMode");
    check(preset.replaceWithText(tree.createXml()->toString()), "modern missing-mode fixture writes");
    check(target.workflow.loadPreset(preset).failed(), "modern preset missing detectionMode is rejected");
    checkNear(getParam(target, "depth"), 7.0f, "missing-mode preset rejection is atomic");
    tree = source.apvts.copyState();
    tree.setProperty("presetVersion", 2, nullptr);
    parameterInState(tree, "detectionMode").setProperty("value", 0.5f, nullptr);
    check(preset.replaceWithText(tree.createXml()->toString()), "modern fractional-mode fixture writes");
    check(target.workflow.loadPreset(preset).failed(), "modern fractional detectionMode is rejected");
    checkNear(getParam(target, "detectionMode"), 1.0f, "invalid-mode preset rejection is atomic");
    check(preset.deleteFile(), "modern invalid-mode preset removed");
}

void testSidechainStateVersionsMigrateAllSnapshotsAtomically() {
    SpectralProcessor source;
    auto* external = source.apvts.getParameter("externalSidechain");
    auto* listen = source.apvts.getParameter("sidechainListen");
    check(external != nullptr && listen != nullptr, "sidechain state parameters exist");
    if (external == nullptr || listen == nullptr) return;
    checkNear(getParam(source, "externalSidechain"), 0.0f, "external sidechain defaults disabled");
    checkNear(getParam(source, "sidechainListen"), 0.0f, "sidechain listen defaults disabled");
    source.workflow.createBand(0, 1234.0f, 3.0f);
    setParam(source, "wband0On", 0.0f);
    setParam(source, "detectionMode", 1.0f);
    const auto preset = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getNonexistentChildFile("fixxy-sidechain-state", ".fixxyspectral", false);
    check(source.workflow.savePreset(preset).wasOk(), "sidechain migration identity preset saves");
    setParam(source, "depth", 7.0f);
    source.workflow.copyActiveToOther();
    source.workflow.selectSlot(SpectralWorkflow::Slot::b);
    setParam(source, "depth", 8.0f);
    source.workflow.selectSlot(SpectralWorkflow::Slot::a);
    juce::ValueTree legacy("LPNGSPECTRAL_STATE");
    legacy.copyPropertiesAndChildrenFrom(source.workflow.createProcessorState(), nullptr);
    legacy.setProperty("stateVersion", 2, nullptr);
    removeParameterRecursively(legacy, "externalSidechain");
    removeParameterRecursively(legacy, "sidechainListen");
    const auto legacyXml = legacy.createXml()->toString();
    SpectralProcessor target;
    setParam(target, "externalSidechain", 1.0f);
    setParam(target, "sidechainListen", 1.0f);
    check(target.workflow.restoreProcessorState(legacy), "version 2 host state migrates missing sidechain flags");
    checkNear(getParam(target, "detectionMode"), 1.0f, "version 2 migration preserves Hard");
    checkNear(getParam(target, "externalSidechain"), 0.0f, "version 2 migration inserts disabled SC");
    checkNear(getParam(target, "sidechainListen"), 0.0f, "version 2 migration inserts disabled SC listen");
    check(target.workflow.currentPresetName() == preset.getFileNameWithoutExtension()
              && target.workflow.isCurrentPresetDirty() && target.workflow.isBandPresent(0),
          "version 2 migration preserves identity dirty baseline and bypassed band presence");
    check(legacy.createXml()->toString() == legacyXml, "migration does not mutate caller-owned input tree");
    target.workflow.selectSlot(SpectralWorkflow::Slot::b);
    checkNear(getParam(target, "depth"), 8.0f, "version 2 legacy wrapper retains inactive B values");
    check(target.workflow.currentPresetName() == preset.getFileNameWithoutExtension()
              && target.workflow.isCurrentPresetDirty() && target.workflow.isBandPresent(0),
          "version 2 legacy wrapper retains inactive B identity dirty baseline and presence");
    setParam(target, "depth", 5.0f);
    check(!target.workflow.isCurrentPresetDirty(), "version 2 legacy wrapper retains B clean values and presence");
    target.workflow.selectSlot(SpectralWorkflow::Slot::a);
    auto migrated = target.workflow.createProcessorState();
    const auto workflow = migrated.getChildWithName("WORKFLOW");
    for (const auto* slot : {"SLOT_A", "SLOT_B"}) {
        for (const auto* snapshot : {"CURRENT", "CLEAN"}) {
            const auto parameters = workflow.getChildWithName(slot).getChildWithName(snapshot)
                                            .getChildWithName("PARAMETERS");
            checkNear(valueInState(parameters, "externalSidechain"), 0.0f,
                      "legacy current/clean sidechain flags are filled in both slots");
            checkNear(valueInState(parameters, "sidechainListen"), 0.0f,
                      "legacy current/clean listen flags are filled in both slots");
        }
    }

    setParam(source, "externalSidechain", 1.0f);
    setParam(source, "sidechainListen", 1.0f);
    auto current = source.workflow.createProcessorState();
    check(target.workflow.restoreProcessorState(current), "version 3 host state restores both SC flags");
    checkNear(getParam(target, "externalSidechain"), 1.0f, "version 3 restores SC enable");
    checkNear(getParam(target, "sidechainListen"), 1.0f, "version 3 restores independent listen");
    const auto before = target.workflow.createProcessorState().createXml()->toString();
    for (const auto* id : {"externalSidechain", "sidechainListen", "detectionMode"}) {
        for (int location = 0; location < 3; ++location) {
            auto malformed = current.createCopy();
            auto parameters = location == 0 ? malformed.getChildWithName("PARAMETERS")
                : malformed.getChildWithName("WORKFLOW").getChildWithName("SLOT_B")
                    .getChildWithName(location == 1 ? "CURRENT" : "CLEAN").getChildWithName("PARAMETERS");
            removeParameterRecursively(parameters, id);
            check(!target.workflow.restoreProcessorState(malformed),
                  "version 3 missing active/inactive/clean parameter is rejected");
            check(target.workflow.createProcessorState().createXml()->toString() == before,
                  "version 3 snapshot rejection leaves full live state unchanged");
            malformed = current.createCopy();
            parameters = location == 0 ? malformed.getChildWithName("PARAMETERS")
                : malformed.getChildWithName("WORKFLOW").getChildWithName("SLOT_B")
                    .getChildWithName(location == 1 ? "CURRENT" : "CLEAN").getChildWithName("PARAMETERS");
            parameterInState(parameters, id).setProperty("value", .5f, nullptr);
            check(!target.workflow.restoreProcessorState(malformed),
                  "version 3 fractional active/inactive/clean mode flag is rejected");
            check(target.workflow.createProcessorState().createXml()->toString() == before,
                  "version 3 malformed flag rejection is atomic");
        }
    }

    check(source.workflow.savePreset(preset).wasOk(), "version 3 sidechain preset saves");
    setParam(target, "externalSidechain", 0.0f);
    setParam(target, "sidechainListen", 0.0f);
    check(target.workflow.loadPreset(preset).wasOk(), "version 3 sidechain preset loads");
    checkNear(getParam(target, "externalSidechain"), 1.0f, "version 3 preset recalls SC enable");
    checkNear(getParam(target, "sidechainListen"), 1.0f, "version 3 preset recalls independent listen");
    check(target.workflow.undo(), "sidechain preset flags undo as one workflow operation");
    checkNear(getParam(target, "externalSidechain"), 0.0f, "sidechain preset undo restores prior SC flag");
    checkNear(getParam(target, "sidechainListen"), 0.0f, "sidechain preset undo restores prior listen flag");

    auto presetTree = source.apvts.copyState();
    presetTree.setProperty("presetFormat", "LpngSpectral", nullptr);
    presetTree.setProperty("presetVersion", 2, nullptr);
    removeParameterRecursively(presetTree, "externalSidechain");
    removeParameterRecursively(presetTree, "sidechainListen");
    check(preset.replaceWithText(presetTree.createXml()->toString()), "version 2 sidechain-free preset fixture writes");
    check(target.workflow.loadPreset(preset).wasOk(), "version 2 preset migrates missing SC controls");
    checkNear(getParam(target, "detectionMode"), 1.0f, "version 2 preset retains Hard");
    checkNear(getParam(target, "externalSidechain"), 0.0f, "version 2 preset defaults SC off");
    checkNear(getParam(target, "sidechainListen"), 0.0f, "version 2 preset defaults listen off");
    presetTree.setProperty("presetVersion", 3, nullptr);
    check(preset.replaceWithText(presetTree.createXml()->toString()), "version 3 incomplete preset fixture writes");
    check(target.workflow.loadPreset(preset).failed(), "version 3 preset cannot silently fill missing SC flags");
    check(preset.deleteFile(), "sidechain migration temporary preset removed");
}

void testABSlotsRestoreAndCopyWithHostNotification() {
    SpectralProcessor processor;
    auto* depth = processor.apvts.getParameter("depth");
    ParameterCounter counter;
    depth->addListener(&counter);

    setParam(processor, "depth", 2.0f);
    processor.workflow.selectSlot(SpectralWorkflow::Slot::b);
    checkNear(getParam(processor, "depth"), 5.0f, "B starts from the factory baseline");

    setParam(processor, "depth", 8.0f);
    processor.workflow.selectSlot(SpectralWorkflow::Slot::a);
    checkNear(getParam(processor, "depth"), 2.0f, "switching back restores A");

    processor.workflow.selectSlot(SpectralWorkflow::Slot::b);
    checkNear(getParam(processor, "depth"), 8.0f, "switching again restores B");
    check(counter.valueChanges >= 5, "A/B restores notify the host parameter surface");

    setParam(processor, "depth", 6.0f);
    processor.workflow.copyActiveToOther();
    processor.workflow.selectSlot(SpectralWorkflow::Slot::a);
    checkNear(getParam(processor, "depth"), 6.0f, "copy replaces the inactive slot");
    depth->removeListener(&counter);
}

void testSlotUndoHistoriesSurviveNavigation() {
    SpectralProcessor processor;
    setParam(processor, "depth", 2.0f);
    processor.workflow.selectSlot(SpectralWorkflow::Slot::b);
    setParam(processor, "depth", 8.0f);
    processor.workflow.selectSlot(SpectralWorkflow::Slot::a);
    check(processor.workflow.undo(), "A undo survives a visit to B");
    checkNear(getParam(processor, "depth"), 5.0f, "A undo restores A's own starting value");
    processor.workflow.selectSlot(SpectralWorkflow::Slot::b);
    check(processor.workflow.undo(), "B undo survives a visit to A");
    checkNear(getParam(processor, "depth"), 5.0f, "B undo restores B's own starting value");
    processor.workflow.selectSlot(SpectralWorkflow::Slot::a);
    check(processor.workflow.redo(), "A redo survives a visit to B");
    checkNear(getParam(processor, "depth"), 2.0f, "A redo does not replay B's edit");
    processor.workflow.selectSlot(SpectralWorkflow::Slot::b);
    check(processor.workflow.redo(), "B redo survives a visit to A");
    checkNear(getParam(processor, "depth"), 8.0f, "B redo does not replay A's edit");

    check(processor.workflow.undo(), "B has a separate redo branch");
    processor.workflow.selectSlot(SpectralWorkflow::Slot::a);
    setParam(processor, "detail", 7.0f);
    processor.workflow.selectSlot(SpectralWorkflow::Slot::b);
    check(processor.workflow.redo(), "editing A does not discard B's redo branch");
    checkNear(getParam(processor, "depth"), 8.0f, "B redo remains intact after an A edit");
    processor.workflow.copyActiveToOther();
    check(processor.workflow.canUndo(), "A/B copy preserves source history");
    processor.workflow.selectSlot(SpectralWorkflow::Slot::a);
    check(!processor.workflow.canUndo() && !processor.workflow.canRedo(),
          "A/B copy deliberately clears destination history");
}

void testGroupedUiEditsPreserveUnrelatedAutomation() {
    SpectralProcessor processor;
    processor.workflow.beginUiEdit("Grouped controls");
    setParam(processor, "depth", 3.0f);
    processor.workflow.beginUiEdit("Nested control");
    setParam(processor, "detail", 7.0f);
    processor.workflow.endUiEdit();
    std::thread automation([&] {
        setParam(processor, "output", -6.0f);
        setParam(processor, "wband1On", 1.0f);
    });
    automation.join();
    auto* hostMix = processor.apvts.getParameter("mix");
    hostMix->setValueNotifyingHost(hostMix->convertTo0to1(37.0f));
    processor.workflow.endUiEdit();
    check(processor.workflow.undo(), "explicit nested UI group is undoable");
    checkNear(getParam(processor, "depth"), 5.0f, "group undo restores depth");
    checkNear(getParam(processor, "detail"), 5.0f, "group undo restores Detail atomically");
    checkNear(getParam(processor, "output"), -6.0f, "group undo preserves concurrent output automation");
    checkNear(getParam(processor, "mix"), 37.0f, "group undo preserves untouched message-thread host automation");
    checkNear(getParam(processor, "wband1On"), 1.0f, "group undo preserves unrelated band automation");
    check(processor.workflow.isBandPresent(1), "group undo preserves host-created band presence");
    check(!processor.workflow.canUndo(), "nested UI group creates exactly one history entry");
    processor.workflow.beginUiEdit("No change");
    processor.workflow.endUiEdit();
    check(processor.workflow.canRedo(), "no-op UI group retains the redo branch");
    check(processor.workflow.redo(), "explicit UI group redoes once");
    checkNear(getParam(processor, "detail"), 7.0f, "group redo restores Detail");
    checkNear(getParam(processor, "output"), -6.0f, "group redo does not replace the whole parameter tree");

    juce::MemoryBlock state;
    processor.getStateInformation(state);
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    check(!processor.workflow.canUndo() && !processor.workflow.canRedo(),
          "host restore clears transient active-slot history");
    processor.workflow.selectSlot(SpectralWorkflow::Slot::b);
    check(!processor.workflow.canUndo() && !processor.workflow.canRedo(),
          "host restore clears inactive-slot history");

    std::thread hostGesture([&] { setParam(processor, "depth", 8.0f); });
    hostGesture.join();
    check(!processor.workflow.canUndo(), "worker-thread host gestures do not become UI edits");
    for (int i = 0; i < 80; ++i)
        setParam(processor, "depth", i % 2 == 0 ? 2.0f : 8.0f);
    int undoCount = 0;
    while (processor.workflow.undo()) ++undoCount;
    check(undoCount == 64, "per-slot UI history retains a bounded 64 edit window");
}

void testPresetAndFactoryLoadsAreUndoable() {
    SpectralProcessor processor;
    const auto preset = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getNonexistentChildFile("fixxy-spectral-workflow", ".fixxyspectral", false);

    setParam(processor, "depth", 3.0f);
    setParam(processor, "output", -6.0f);
    check(processor.workflow.savePreset(preset).wasOk(), "preset saves");

    setParam(processor, "depth", 9.0f);
    setParam(processor, "output", 4.0f);
    const auto loadResult = processor.workflow.loadPreset(preset);
    if (loadResult.failed()) std::printf("preset load error: %s\n", loadResult.getErrorMessage().toRawUTF8());
    check(loadResult.wasOk(), "preset loads");
    checkNear(getParam(processor, "depth"), 3.0f, "preset restores depth");
    checkNear(getParam(processor, "output"), -6.0f, "preset restores output");

    check(processor.workflow.undo(), "preset load can be undone");
    checkNear(getParam(processor, "depth"), 9.0f, "undo restores pre-load depth");
    checkNear(getParam(processor, "output"), 4.0f, "undo restores pre-load output");
    check(processor.workflow.redo(), "preset load can be redone");
    checkNear(getParam(processor, "depth"), 3.0f, "redo restores loaded depth");
    checkNear(getParam(processor, "output"), -6.0f, "redo restores loaded output");

    setParam(processor, "depth", 7.0f);
    processor.workflow.loadFactoryBaseline();
    checkNear(getParam(processor, "depth"), 5.0f, "factory baseline restores defaults");
    check(processor.workflow.undo(), "factory baseline load can be undone");
    checkNear(getParam(processor, "depth"), 7.0f, "factory undo restores the prior value");

    check(preset.deleteFile(), "temporary preset removed");
}

void testPresetUndoRestoresIdentityAndCleanBaseline() {
    SpectralProcessor processor;
    const auto temporaryDirectory = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto presetA = temporaryDirectory.getNonexistentChildFile("fixxy-identity-a", ".fixxyspectral", false);
    const auto presetB = temporaryDirectory.getNonexistentChildFile("fixxy-identity-b", ".fixxyspectral", false);
    processor.workflow.createBand(0, 120.0f, 0.0f);
    setParam(processor, "wband0On", 0.0f);
    setParam(processor, "depth", 3.0f);
    check(processor.workflow.savePreset(presetA).wasOk(), "identity preset A saves");
    processor.workflow.deleteBand(0);
    setParam(processor, "depth", 8.0f);
    check(processor.workflow.savePreset(presetB).wasOk(), "identity preset B saves");
    check(processor.workflow.loadPreset(presetA).wasOk(), "identity preset A loads over B");
    check(processor.workflow.undo(), "identity preset load can be undone");
    checkNear(getParam(processor, "depth"), 8.0f, "identity undo restores preset B sound");
    check(processor.workflow.currentPresetName() == presetB.getFileNameWithoutExtension(),
          "identity undo restores preset B name");
    check(!processor.workflow.isBandPresent(0), "identity undo restores preset B absent band");
    check(!processor.workflow.isCurrentPresetDirty(), "identity undo restores preset B clean baseline");
    check(processor.workflow.redo(), "identity preset load can be redone");
    check(processor.workflow.currentPresetName() == presetA.getFileNameWithoutExtension(),
          "identity redo restores preset A name");
    check(processor.workflow.isBandPresent(0), "identity redo restores preset A retained bypassed band");
    check(!processor.workflow.isCurrentPresetDirty(), "identity redo restores preset A clean baseline");

    setParam(processor, "depth", 4.0f);
    processor.workflow.loadFactoryBaseline();
    check(processor.workflow.undo(), "identity factory load can be undone");
    checkNear(getParam(processor, "depth"), 4.0f, "factory identity undo restores modified sound");
    check(processor.workflow.currentPresetName() == presetA.getFileNameWithoutExtension(),
          "factory identity undo restores prior preset name");
    check(processor.workflow.isCurrentPresetDirty(), "factory identity undo restores prior dirty status");
    check(processor.workflow.redo(), "identity factory load can be redone");
    check(processor.workflow.currentPresetName() == "Factory baseline",
          "factory identity redo restores factory name");
    check(!processor.workflow.isCurrentPresetDirty(), "factory identity redo restores factory clean baseline");
    check(processor.workflow.undo(), "factory identity can return to the prior modified preset again");
    setParam(processor, "depth", 3.0f);
    check(!processor.workflow.isCurrentPresetDirty(), "factory identity undo restores the prior clean snapshot");
    check(presetA.deleteFile(), "identity preset A removed");
    check(presetB.deleteFile(), "identity preset B removed");
}

juce::TextButton* findTextButton(juce::AudioProcessorEditor& editor, const juce::String& text) {
    for (auto* child : editor.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*>(child);
            button != nullptr && button->getButtonText() == text)
            return button;
    return nullptr;
}

void pumpMessageLoop() {
#if JUCE_MAC
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
#else
    juce::Thread::sleep(100);
    juce::Timer::callPendingTimersSynchronously();
#endif
}

juce::MouseEvent mouseEvent(juce::Component& component, juce::Point<float> position,
                            juce::Point<float> start, bool dragging = false) {
    const auto now = juce::Time::getCurrentTime();
    return {juce::Desktop::getInstance().getMainMouseSource(), position,
            juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &component, &component, now, start,
            now, 1, dragging};
}

void testActualEditorGesturesUseSlotHistory() {
    SpectralProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    juce::Slider* depth = nullptr;
    WeightBandLayer* bands = nullptr;
    for (auto* child : editor->getChildren()) {
        if (child->getComponentID() == "depth") depth = dynamic_cast<juce::Slider*>(child);
        if (auto* layer = dynamic_cast<WeightBandLayer*>(child)) bands = layer;
    }
    check(depth != nullptr && bands != nullptr, "real editor exposes depth and band gesture surfaces");
    if (depth == nullptr || bands == nullptr) return;

    const auto point = depth->getLocalBounds().toFloat().getCentre();
    depth->mouseDown(mouseEvent(*depth, point, point));
    depth->mouseDrag(mouseEvent(*depth, point.translated(0, -35), point, true));
    depth->mouseUp(mouseEvent(*depth, point.translated(0, -35), point, true));
    check(std::abs(getParam(processor, "depth") - 5.0f) > .01f, "actual slider drag changes depth");
    check(processor.workflow.undo(), "actual slider drag creates an undo entry");
    checkNear(getParam(processor, "depth"), 5.0f, "slider drag undo restores the whole gesture");
    check(!processor.workflow.canUndo(), "slider drag does not record each value notification");

    juce::Label* text = nullptr;
    for (auto* child : depth->getChildren())
        if (auto* label = dynamic_cast<juce::Label*>(child)) text = label;
    check(text != nullptr, "real depth text box is available");
    if (text == nullptr) return;
    text->setText("3.25", juce::sendNotificationSync);
    checkNear(getParam(processor, "depth"), 3.25f, "slider text commit reaches the real parameter");
    depth->mouseDoubleClick(mouseEvent(*depth, point, point));
    checkNear(getParam(processor, "depth"), 5.0f, "actual slider double-click restores default");
    check(processor.workflow.undo(), "slider reset is independently undoable");
    checkNear(getParam(processor, "depth"), 3.25f, "reset undo preserves the typed value");
    check(processor.workflow.undo(), "slider text commit is independently undoable");
    checkNear(getParam(processor, "depth"), 5.0f, "text commit undo restores its starting value");

    auto* hard = findTextButton(*editor, "Hard");
    check(hard != nullptr, "real detector mode button exists");
    if (hard == nullptr) return;
    hard->onClick();
    processor.workflow.selectSlot(SpectralWorkflow::Slot::b);
    setParam(processor, "depth", 8.0f);
    processor.workflow.selectSlot(SpectralWorkflow::Slot::a);
    check(processor.workflow.undo(), "actual mode gesture survives an A/B roundtrip");
    checkNear(getParam(processor, "detectionMode"), 0.0f, "actual mode undo restores Soft");
    check(processor.workflow.redo(), "actual mode gesture redoes in A");
    checkNear(getParam(processor, "detectionMode"), 1.0f, "actual mode redo restores Hard");

    const auto bandPoint = bands->getLocalBounds().toFloat().getCentre();
    bands->mouseDoubleClick(mouseEvent(*bands, bandPoint, bandPoint));
    check(processor.workflow.isBandPresent(0), "real graph double-click creates a band");
    const float frequency = getParam(processor, "wband0Freq");
    const float gain = getParam(processor, "wband0Gain");
    bands->mouseDown(mouseEvent(*bands, bandPoint, bandPoint));
    bands->mouseDrag(mouseEvent(*bands, bandPoint.translated(40, -20), bandPoint, true));
    bands->mouseUp(mouseEvent(*bands, bandPoint.translated(40, -20), bandPoint, true));
    check(std::abs(getParam(processor, "wband0Freq") - frequency) > 1.0f
              && std::abs(getParam(processor, "wband0Gain") - gain) > .01f,
          "actual node drag changes frequency and gain");
    check(processor.workflow.undo(), "actual paired node gesture is undoable");
    checkNear(getParam(processor, "wband0Freq"), frequency, "node undo restores frequency");
    checkNear(getParam(processor, "wband0Gain"), gain, "node undo restores gain in the same transaction");
    check(processor.workflow.undo(), "graph band creation is the preceding transaction");
    check(!processor.workflow.isBandPresent(0), "graph creation undo restores absence atomically");
    editor.reset();
    editor.reset(processor.createEditor());
    check(processor.workflow.canRedo(), "editor reopening preserves workflow redo history");
}

void testGestureCloseRestoreAndDiscreteControls() {
    SpectralProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    const auto depthSlider = [&]() {
        return dynamic_cast<juce::Slider*>(editor->findChildWithID("depth"));
    };
    auto* depth = depthSlider();
    if (depth == nullptr) { check(false, "close/restore test finds depth slider"); return; }
    const auto point = depth->getLocalBounds().toFloat().getCentre();
    depth->mouseDown(mouseEvent(*depth, point, point));
    depth->mouseDrag(mouseEvent(*depth, point.translated(0, -30), point, true));
    editor.reset();
    editor.reset(processor.createEditor());
    check(processor.workflow.undo(), "closing an editor during a slider drag finishes the UI transaction");
    checkNear(getParam(processor, "depth"), 5.0f, "mid-drag close undo restores the initial depth");
    const auto state = processor.workflow.createProcessorState();
    depth = depthSlider();
    depth->mouseDown(mouseEvent(*depth, point, point));
    depth->mouseDrag(mouseEvent(*depth, point.translated(0, -30), point, true));
    check(processor.workflow.restoreProcessorState(state), "host state can replace an in-flight UI drag");
    depth->mouseUp(mouseEvent(*depth, point.translated(0, -30), point, true));
    check(!processor.workflow.canUndo() && !processor.workflow.canRedo(),
          "post-restore mouse-up does not resurrect the canceled drag history");
    for (const auto* id : {"bypass", "delta"}) {
        juce::Button* button = nullptr;
        for (auto* child : editor->getChildren())
            if (auto* candidate = dynamic_cast<juce::Button*>(child);
                candidate != nullptr && candidate->getButtonText() == id) button = candidate;
        check(button != nullptr, "real footer toggle is available");
        if (button == nullptr) continue;
        button->triggerClick();
        pumpMessageLoop();
        checkNear(getParam(processor, id), 1.0f, "real footer click updates its parameter");
        check(processor.workflow.undo(), "real footer toggle has one explicit undo transaction");
        checkNear(getParam(processor, id), 0.0f, "footer undo captures the value before the click");
    }
    WeightBandLayer* bands = nullptr;
    for (auto* child : editor->getChildren())
        if (auto* layer = dynamic_cast<WeightBandLayer*>(child)) bands = layer;
    if (bands == nullptr) { check(false, "discrete test finds the band layer"); return; }
    const auto bandPoint = bands->getLocalBounds().toFloat().getCentre();
    bands->mouseDoubleClick(mouseEvent(*bands, bandPoint, bandPoint));
    juce::ComboBox* shape = nullptr;
    for (auto* child : editor->getChildren())
        if (auto* combo = dynamic_cast<juce::ComboBox*>(child)) shape = combo;
    check(shape != nullptr, "selected band has a real shape selector");
    if (shape != nullptr) {
        shape->setSelectedId(2, juce::sendNotificationSync);
        checkNear(getParam(processor, "wband0Type"), 1.0f, "actual band choice changes its parameter");
        check(processor.workflow.undo(), "actual band choice creates an explicit undo transaction");
        checkNear(getParam(processor, "wband0Type"), 0.0f, "choice undo restores the pre-change shape");
        pumpMessageLoop();
        check(shape->getSelectedId() == 1, "choice display silently follows undo");
    }
    const float frequency = getParam(processor, "wband0Freq");
    check(bands->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)), "selected band accepts a keyboard frequency edit");
    check(getParam(processor, "wband0Freq") > frequency, "keyboard edit actually changes frequency");
    check(processor.workflow.undo(), "keyboard band edit is undoable");
    checkNear(getParam(processor, "wband0Freq"), frequency, "keyboard undo restores the prior frequency");
    juce::MouseWheelDetails wheel{};
    wheel.deltaY = .1f;
    bands->mouseWheelMove(mouseEvent(*bands, bandPoint, bandPoint), wheel);
    check(getParam(processor, "wband0Q") > .7f, "actual node wheel gesture changes Q");
    check(processor.workflow.undo(), "node wheel gesture is undoable");
    checkNear(getParam(processor, "wband0Q"), .7f, "wheel undo restores prior Q");
    bands->mouseDoubleClick(mouseEvent(*bands, bandPoint, bandPoint));
    checkNear(getParam(processor, "wband0On"), 0.0f, "node double-click bypasses the band");
    check(processor.workflow.undo(), "node bypass gesture is undoable");
    checkNear(getParam(processor, "wband0On"), 1.0f, "node bypass undo restores enabled state");
    const juce::Point<float> boundary{6.0f, bandPoint.y};
    bands->mouseDown(mouseEvent(*bands, boundary, boundary));
    bands->mouseDrag(mouseEvent(*bands, boundary.translated(40, 0), boundary, true));
    bands->mouseUp(mouseEvent(*bands, boundary.translated(40, 0), boundary, true));
    check(getParam(processor, "rangeLo") > 20.0f, "actual range boundary drag changes its parameter");
    check(processor.workflow.undo(), "range boundary gesture is undoable");
    checkNear(getParam(processor, "rangeLo"), 20.0f, "range undo restores the full-range boundary");
}

void testSelectedBandRestoreDefersAttachmentReplacement() {
    SpectralProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    WeightBandLayer* bands = nullptr;
    for (auto* child : editor->getChildren())
        if (auto* layer = dynamic_cast<WeightBandLayer*>(child)) bands = layer;
    if (bands == nullptr) { check(false, "band restore test finds graph"); return; }
    const auto location = bands->getLocalBounds().toFloat().getCentre();
    bands->mouseDoubleClick(mouseEvent(*bands, location, location));
    pumpMessageLoop();  // Let the graph observe presence before the host removes it.
    juce::Slider* chip = nullptr;
    for (auto* child : editor->getChildren()) {
        auto* candidate = dynamic_cast<juce::Slider*>(child);
        if (candidate != nullptr && candidate->isVisible() && candidate->getSliderStyle() == juce::Slider::LinearBar
            && std::abs(candidate->getValue() - getParam(processor, "wband0Freq")) < .01) chip = candidate;
    }
    check(chip != nullptr, "selected band exposes the real frequency chip");
    if (chip == nullptr) return;
    struct GestureBalance final : juce::AudioProcessorParameter::Listener {
        void parameterValueChanged(int, float) override {}
        void parameterGestureChanged(int, bool starting) override { starting ? ++begins : ++ends; }
        int begins = 0, ends = 0;
    } oldParameter, otherParameter;
    auto* frequency = processor.apvts.getParameter("wband0Freq");
    auto* otherFrequency = processor.apvts.getParameter("wband1Freq");
    frequency->addListener(&oldParameter);
    otherFrequency->addListener(&otherParameter);
    const auto point = chip->getLocalBounds().toFloat().getCentre();
    chip->mouseDown(mouseEvent(*chip, point, point));
    chip->mouseDrag(mouseEvent(*chip, point.translated(10, 0), point, true));
    SpectralProcessor empty;
    const auto state = empty.workflow.createProcessorState();
    check(processor.workflow.restoreProcessorState(state), "host removes the selected band during chip drag");
    pumpMessageLoop();
    chip->mouseUp(mouseEvent(*chip, point.translated(10, 0), point, true));
    pumpMessageLoop();
    check(oldParameter.begins == 1 && oldParameter.ends == 1,
          "selected-band removal preserves the old chip parameter's gesture end");
    check(otherParameter.begins == 0 && otherParameter.ends == 0,
          "selected-band removal never sends an orphan end to a different parameter");
    check(!chip->isVisible(), "removed selected-band chip hides after the drag and deferred rebind finish");
    check(!processor.workflow.canUndo(), "removed-band mouse-up does not recreate cleared host history");
    bands->mouseDoubleClick(mouseEvent(*bands, location, location));
    pumpMessageLoop();
    chip->mouseDown(mouseEvent(*chip, point, point));
    chip->mouseDrag(mouseEvent(*chip, point.translated(10, 0), point, true));
    processor.workflow.restoreProcessorState(state);
    pumpMessageLoop();
    chip->mouseUp(mouseEvent(*chip, point.translated(10, 0), point, true));
    bands->mouseDoubleClick(mouseEvent(*bands, location, location));
    pumpMessageLoop();
    check(chip->isVisible() && processor.workflow.isBandPresent(0),
          "fresh band selection supersedes a queued stale deselection");
    frequency->removeListener(&oldParameter);
    otherFrequency->removeListener(&otherParameter);
}

void testSidechainToolbarGesturesRecallAndMinimumLayout() {
    SpectralProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    editor->setSize(892, 520);
    auto* external = findTextButton(*editor, "SC");
    auto* listen = findTextButton(*editor, "listen");
    juce::Label* level = nullptr;
    for (auto* child : editor->getChildren())
        if (child->getComponentID() == "sidechainLevel") level = dynamic_cast<juce::Label*>(child);
    check(external != nullptr && listen != nullptr && level != nullptr,
          "real toolbar exposes SC enable listen and input meter");
    if (external == nullptr || listen == nullptr || level == nullptr) return;
    check(!external->getToggleState() && !listen->getToggleState(), "SC toolbar starts disabled");
    const auto disconnected = level->getText();
    check(disconnected.length() == 2 && disconnected[0] == 0x2212 && disconnected[1] == 0x221e
              && disconnected == juce::String::fromUTF8("\xe2\x88\x92\xe2\x88\x9e"),
          "disconnected SC meter displays UTF-8 minus and infinity codepoints");
    check(level->getTooltip() == "External sidechain input: " + disconnected + "FS",
          "disconnected SC tooltip preserves the Unicode meter text");
    external->triggerClick();
    pumpMessageLoop();
    checkNear(getParam(processor, "externalSidechain"), 1.0f, "SC button enables external detection");
    listen->triggerClick();
    pumpMessageLoop();
    checkNear(getParam(processor, "sidechainListen"), 1.0f, "listen button enables key monitoring");
    external->triggerClick();
    pumpMessageLoop();
    checkNear(getParam(processor, "externalSidechain"), 0.0f, "SC button independently disables detection");
    checkNear(getParam(processor, "sidechainListen"), 1.0f, "SC disable preserves independent key listen");
    check(processor.workflow.undo(), "SC toolbar gesture participates in the slot journal");
    pumpMessageLoop();
    check(external->getToggleState() && listen->getToggleState(), "SC undo restores detection without changing listen");
    processor.workflow.selectSlot(SpectralWorkflow::Slot::b);
    pumpMessageLoop();
    check(!external->getToggleState() && !listen->getToggleState(), "B recalls its own SC flags in an open editor");
    processor.workflow.selectSlot(SpectralWorkflow::Slot::a);
    pumpMessageLoop();
    check(external->getToggleState() && listen->getToggleState(), "A recalls its own SC flags in an open editor");
    std::thread automation([&] {
        setParam(processor, "externalSidechain", 0.0f);
        setParam(processor, "sidechainListen", 0.0f);
    });
    automation.join();
    pumpMessageLoop();
    check(!external->getToggleState() && !listen->getToggleState(), "host automation refreshes both SC buttons");
    processor.sidechainLevelDb.store(-24.0f, std::memory_order_relaxed);
    pumpMessageLoop();
    check(level->getText() == "-24.0 dB", "toolbar meter consumes the audio-thread level publication");
    check(level->getTooltip().contains("dBFS"), "sidechain meter identifies its level units");
    setParam(processor, "externalSidechain", 1.0f);
    setParam(processor, "sidechainListen", 1.0f);
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    setParam(processor, "externalSidechain", 0.0f);
    setParam(processor, "sidechainListen", 0.0f);
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    pumpMessageLoop();
    check(external->getToggleState() && listen->getToggleState(), "host state recall refreshes open-editor SC buttons");
    for (auto* child : editor->getChildren()) {
        if (!child->isVisible() || child->getY() >= 44) continue;
        check(editor->getLocalBounds().contains(child->getBounds()) && child->getWidth() > 0,
              "toolbar controls fit the minimum 892-pixel editor");
        if (child == external || child == listen || child == level) continue;
        check(!child->getBounds().intersects(external->getBounds())
                  && !child->getBounds().intersects(listen->getBounds())
                  && !child->getBounds().intersects(level->getBounds()),
              "sidechain toolbar does not overlap existing controls");
    }
    check(!external->getBounds().intersects(listen->getBounds())
              && !listen->getBounds().intersects(level->getBounds()),
          "sidechain controls and meter have distinct bounds");
}

void testWorkflowOnlyHostRestoreRefreshesOpenEditor() {
    SpectralProcessor source;
    source.workflow.selectSlot(SpectralWorkflow::Slot::b);
    source.workflow.setSidebarExpanded(false);
    const auto preset = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getNonexistentChildFile("fixxy-host-metadata", ".fixxyspectral", false);
    check(source.workflow.savePreset(preset).wasOk(), "workflow-only host preset saves");
    juce::MemoryBlock collapsedState;
    source.getStateInformation(collapsedState);

    SpectralProcessor target;
    std::unique_ptr<juce::AudioProcessorEditor> editor(target.createEditor());
    SpectralAnalyzerView* display = nullptr;
    for (auto* child : editor->getChildren())
        if (auto* view = dynamic_cast<SpectralAnalyzerView*>(child)) display = view;
    check(display != nullptr, "host restore test finds the real analyzer component");
    if (display == nullptr) return;
    const auto expandedBounds = display->getBounds();
    target.setStateInformation(collapsedState.getData(), static_cast<int>(collapsedState.getSize()));
    pumpMessageLoop();
    check(findTextButton(*editor, "B to A") != nullptr, "workflow-only restore refreshes A/B toolbar");
    check(findTextButton(*editor, preset.getFileNameWithoutExtension()) != nullptr,
          "workflow-only restore refreshes preset identity");
    check(findTextButton(*editor, "details >") != nullptr, "workflow-only restore refreshes collapsed sidebar button");
    auto* lrButton = findTextButton(*editor, "L/R");
    check(lrButton != nullptr && !lrButton->isVisible(), "workflow-only restore hides sidebar controls");
    check(display->getX() < expandedBounds.getX() && display->getWidth() > expandedBounds.getWidth(),
          "workflow-only collapse relayouts the analyzer bounds");

    source.workflow.setSidebarExpanded(true);
    juce::MemoryBlock expandedState;
    source.getStateInformation(expandedState);
    target.setStateInformation(expandedState.getData(), static_cast<int>(expandedState.getSize()));
    pumpMessageLoop();
    check(findTextButton(*editor, "details <") != nullptr && lrButton != nullptr && lrButton->isVisible(),
          "workflow-only restore shows expanded sidebar controls");
    check(display->getBounds() == expandedBounds, "workflow-only expansion restores analyzer layout");
    check(preset.deleteFile(), "workflow-only host preset removed");
}

void testMalformedPresetIsRejectedAtomically(const char* presetFormat) {
    SpectralProcessor processor;
    setParam(processor, "depth", 7.0f);
    const auto preset = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getNonexistentChildFile("fixxy-spectral-invalid", ".fixxyspectral", false);

    juce::ValueTree incomplete("PARAMETERS");
    incomplete.setProperty("presetFormat", presetFormat, nullptr);
    incomplete.setProperty("presetVersion", 1, nullptr);
    check(preset.replaceWithText(incomplete.createXml()->toString()), "invalid preset fixture writes");
    check(processor.workflow.loadPreset(preset).failed(), "incomplete preset is rejected");
    checkNear(getParam(processor, "depth"), 7.0f, "rejected preset leaves parameters unchanged");

    auto nonfinite = processor.apvts.copyState();
    nonfinite.setProperty("presetFormat", presetFormat, nullptr);
    nonfinite.setProperty("presetVersion", 1, nullptr);
    for (auto child : nonfinite)
        if (child["id"].toString() == "depth") child.setProperty("value", INFINITY, nullptr);
    check(preset.replaceWithText(nonfinite.createXml()->toString()), "nonfinite preset fixture writes");
    check(processor.workflow.loadPreset(preset).failed(), "nonfinite preset value is rejected");
    checkNear(getParam(processor, "depth"), 7.0f, "nonfinite rejection is atomic");

    auto outOfRange = processor.apvts.copyState();
    outOfRange.setProperty("presetFormat", presetFormat, nullptr);
    outOfRange.setProperty("presetVersion", 1, nullptr);
    for (auto child : outOfRange)
        if (child["id"].toString() == "depth") child.setProperty("value", 100.0, nullptr);
    check(preset.replaceWithText(outOfRange.createXml()->toString()), "out-of-range preset fixture writes");
    check(processor.workflow.loadPreset(preset).failed(), "out-of-range preset value is rejected");
    checkNear(getParam(processor, "depth"), 7.0f, "range rejection is atomic");

    auto wrongVersion = processor.apvts.copyState();
    wrongVersion.setProperty("presetFormat", presetFormat, nullptr);
    wrongVersion.setProperty("presetVersion", 99, nullptr);
    check(preset.replaceWithText(wrongVersion.createXml()->toString()), "version preset fixture writes");
    check(processor.workflow.loadPreset(preset).failed(), "unsupported preset version is rejected");
    checkNear(getParam(processor, "depth"), 7.0f, "version rejection is atomic");

    auto fractionalBool = processor.apvts.copyState();
    fractionalBool.setProperty("presetFormat", presetFormat, nullptr);
    fractionalBool.setProperty("presetVersion", 1, nullptr);
    for (auto child : fractionalBool)
        if (child["id"].toString() == "wband0On") child.setProperty("value", 0.5, nullptr);
    check(preset.replaceWithText(fractionalBool.createXml()->toString()), "fractional bool fixture writes");
    check(processor.workflow.loadPreset(preset).failed(), "fractional bool preset value is rejected");
    checkNear(getParam(processor, "depth"), 7.0f, "fractional bool rejection is atomic");

    auto fractionalChoice = processor.apvts.copyState();
    fractionalChoice.setProperty("presetFormat", presetFormat, nullptr);
    fractionalChoice.setProperty("presetVersion", 1, nullptr);
    for (auto child : fractionalChoice)
        if (child["id"].toString() == "wband0Type") child.setProperty("value", 1.5, nullptr);
    check(preset.replaceWithText(fractionalChoice.createXml()->toString()), "fractional choice fixture writes");
    check(processor.workflow.loadPreset(preset).failed(), "fractional choice preset value is rejected");
    checkNear(getParam(processor, "depth"), 7.0f, "fractional choice rejection is atomic");
    for (const auto* wrongFormat : {"OtherProduct", "fixxyspectral", "lpngspectral"}) {
        auto wrongProduct = processor.apvts.copyState();
        wrongProduct.setProperty("presetFormat", wrongFormat, nullptr);
        wrongProduct.setProperty("presetVersion", 3, nullptr);
        check(preset.replaceWithText(wrongProduct.createXml()->toString()), "wrong product fixture writes");
        check(processor.workflow.loadPreset(preset).failed(), "wrong or case-mismatched preset brand is rejected");
        checkNear(getParam(processor, "depth"), 7.0f, "wrong product rejection is atomic");
    }
    check(preset.deleteFile(), "invalid temporary preset removed");
}

void testBandPresenceDistinguishesBypassDeleteAndReuse() {
    SpectralProcessor processor;
    check(processor.workflow.firstAbsentBand() == 0, "factory state has an available band slot");

    processor.workflow.createBand(0, 1800.0f, 4.0f);
    check(processor.workflow.isBandPresent(0), "adding marks a band present");
    checkNear(getParam(processor, "wband0On"), 1.0f, "an added band starts enabled");

    setParam(processor, "wband0On", 0.0f);
    check(processor.workflow.isBandPresent(0), "bypassing retains band presence");
    checkNear(getParam(processor, "wband0Freq"), 1800.0f, "bypassing retains band values");
    check(processor.workflow.firstAbsentBand() == 1, "a bypassed band is not reused");
    check(processor.workflow.isCurrentPresetDirty(), "retained presence contributes to preset dirty state");

    processor.workflow.deleteBand(0);
    check(!processor.workflow.isBandPresent(0), "delete clears band presence");
    checkNear(getParam(processor, "wband0On"), 0.0f, "delete leaves DSP On false");
    check(processor.workflow.firstAbsentBand() == 0, "a deleted band slot is reusable");

    check(processor.workflow.undo(), "band deletion can be undone");
    check(processor.workflow.isBandPresent(0), "undo restores retained band presence");
    checkNear(getParam(processor, "wband0On"), 0.0f, "undo restores the prior bypass state");
    checkNear(getParam(processor, "wband0Freq"), 1800.0f, "undo restores the band values");
    check(processor.workflow.redo(), "band deletion can be redone");
    check(!processor.workflow.isBandPresent(0), "redo removes the band again");
}

void testAutomationEnableCreatesRetainedPresence() {
    SpectralProcessor processor;
    setParam(processor, "wband3On", 1.0f);
    setParam(processor, "wband3On", 0.0f);
    check(processor.workflow.isBandPresent(3),
          "host enable then bypass is retained without an intervening UI query");
    check(processor.workflow.firstAbsentBand() != 3, "host-enabled band is not reused");
    const auto retainedMask = processor.workflow.bandPresenceMask();
    check(retainedMask == (1u << 3),
          "presence mask retains the host-enabled band");
    check(processor.workflow.isCurrentPresetDirty(), "presence-only change marks the preset modified");

    juce::MemoryBlock state;
    processor.getStateInformation(state);
    SpectralProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    check(restored.workflow.isBandPresent(3), "inferred band presence survives state recall");
    checkNear(getParam(restored, "wband3On"), 0.0f, "state recall preserves bypassed DSP state");
}

void testWorkflowSurvivesStateRoundTripAndEditorReopen() {
    SpectralProcessor source;
    const auto preset = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getNonexistentChildFile("fixxy-spectral-session", ".fixxyspectral", false);
    source.workflow.createBand(0, 1800.0f, 4.0f);
    setParam(source, "wband0On", 0.0f);
    setParam(source, "depth", 2.0f);
    check(source.workflow.savePreset(preset).wasOk(), "session preset saves");
    setParam(source, "depth", 3.0f);
    check(source.workflow.isCurrentPresetDirty(), "parameter edit marks preset modified");
    source.workflow.selectSlot(SpectralWorkflow::Slot::b);
    source.workflow.createBand(1, 3200.0f, 2.0f);
    setParam(source, "wband1On", 0.0f);
    setParam(source, "depth", 8.0f);
    source.workflow.setSidebarExpanded(false);

    {
        std::unique_ptr<juce::AudioProcessorEditor> first(source.createEditor());
        check(first != nullptr, "first editor opens");
    }
    {
        std::unique_ptr<juce::AudioProcessorEditor> reopened(source.createEditor());
        check(reopened != nullptr, "editor reopens");
        check(source.workflow.activeSlot() == SpectralWorkflow::Slot::b,
              "editor recreation retains the active slot");
        check(!source.workflow.isSidebarExpanded(),
              "editor recreation retains collapsed sidebar state");
    }

    juce::MemoryBlock state;
    source.getStateInformation(state);
    const auto stateXml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    check(stateXml != nullptr && stateXml->hasTagName("FIXXY_SPECTRAL_STATE"),
          "serialized host sessions use the Fixxy processor root");
    SpectralProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    check(restored.workflow.activeSlot() == SpectralWorkflow::Slot::b,
          "host state restores the active slot");
    check(!restored.workflow.isSidebarExpanded(), "host state restores sidebar state");
    checkNear(getParam(restored, "depth"), 8.0f, "host state restores active-slot parameters");
    restored.workflow.selectSlot(SpectralWorkflow::Slot::a);
    checkNear(getParam(restored, "depth"), 3.0f, "host state retains the inactive A snapshot");
    check(restored.workflow.currentPresetName() == preset.getFileNameWithoutExtension(),
          "host state retains preset identity");
    check(restored.workflow.isCurrentPresetDirty(), "host state retains modified preset status");
    juce::ValueTree legacy("LPNGSPECTRAL_STATE");
    legacy.copyPropertiesAndChildrenFrom(source.workflow.createProcessorState(), nullptr);
    const auto legacyXml = legacy.createXml()->toString();
    restoreHostTree(restored, legacy);
    check(restored.workflow.activeSlot() == SpectralWorkflow::Slot::b
              && !restored.workflow.isSidebarExpanded(),
          "legacy-branded version 3 host state restores active B and sidebar metadata");
    checkNear(getParam(restored, "depth"), 8.0f, "legacy-branded version 3 restores active B values");
    check(restored.workflow.currentPresetName() == "Factory baseline"
              && restored.workflow.isCurrentPresetDirty() && restored.workflow.isBandPresent(1)
              && !restored.workflow.isBandPresent(0),
          "legacy-branded version 3 retains B identity dirty baseline and bypassed presence");
    restored.workflow.selectSlot(SpectralWorkflow::Slot::a);
    checkNear(getParam(restored, "depth"), 3.0f, "legacy-branded version 3 restores inactive A values");
    check(restored.workflow.currentPresetName() == preset.getFileNameWithoutExtension()
              && restored.workflow.isCurrentPresetDirty() && restored.workflow.isBandPresent(0)
              && !restored.workflow.isBandPresent(1),
          "legacy-branded version 3 retains A identity dirty baseline and bypassed presence");
    setParam(restored, "depth", 2.0f);
    check(!restored.workflow.isCurrentPresetDirty(), "legacy-branded version 3 retains A clean values and presence");
    restored.workflow.selectSlot(SpectralWorkflow::Slot::b);
    restored.workflow.deleteBand(1);
    setParam(restored, "depth", 5.0f);
    check(!restored.workflow.isCurrentPresetDirty(), "legacy-branded version 3 retains B clean values and presence");
    check(legacy.createXml()->toString() == legacyXml, "legacy-branded version 3 restore preserves caller input");
    for (const auto* wrongType : {"OTHER_STATE", "fixxy_spectral_state", "lpngspectral_state"}) {
        juce::ValueTree wrongRoot(wrongType);
        wrongRoot.copyPropertiesAndChildrenFrom(legacy, nullptr);
        const auto before = restored.workflow.createProcessorState().createXml()->toString();
        check(!restored.workflow.restoreProcessorState(wrongRoot), "wrong or case-mismatched host root is rejected");
        check(restored.workflow.createProcessorState().createXml()->toString() == before,
              "wrong host root rejection preserves full live state");
    }
    check(preset.deleteFile(), "session temporary preset removed");
}

void testConcurrentHostSerializationIsSlotCoherent() {
    SpectralProcessor processor;
    setParam(processor, "depth", 2.0f);
    processor.workflow.selectSlot(SpectralWorkflow::Slot::b);
    setParam(processor, "depth", 8.0f);
    processor.workflow.selectSlot(SpectralWorkflow::Slot::a);

    std::atomic<bool> start{false};
    std::atomic<bool> inconsistent{false};
    std::thread switcher([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (int iteration = 0; iteration < 1000; ++iteration) {
            processor.workflow.selectSlot(SpectralWorkflow::Slot::b);
            processor.workflow.selectSlot(SpectralWorkflow::Slot::a);
        }
    });

    start.store(true, std::memory_order_release);
    for (int iteration = 0; iteration < 500; ++iteration) {
        juce::MemoryBlock bytes;
        processor.getStateInformation(bytes);
        auto xml = juce::AudioProcessor::getXmlFromBinary(bytes.getData(),
                                                           static_cast<int>(bytes.getSize()));
        if (xml == nullptr) {
            inconsistent = true;
            break;
        }
        const auto state = juce::ValueTree::fromXml(*xml);
        const auto parameters = state.getChildWithName("PARAMETERS");
        const auto workflow = state.getChildWithName(SpectralWorkflow::stateType());
        const int active = static_cast<int>(workflow["activeSlot"]);
        const float expected = active == 0 ? 2.0f : 8.0f;
        if (std::abs(valueInState(parameters, "depth") - expected) > 1.0e-5f) {
            inconsistent = true;
            break;
        }
    }
    switcher.join();
    check(!inconsistent.load(), "host serialization pairs parameters with the same active slot");
}

int runJournalLockRegression(bool audioDuringJournal) {
    SpectralProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    juce::Slider* slider = nullptr;
    for (auto* child : editor->getChildren())
        if (child->getComponentID() == (audioDuringJournal ? "depth" : "detail"))
            slider = dynamic_cast<juce::Slider*>(child);
    if (slider == nullptr) return 1;
    const auto point = slider->getLocalBounds().toFloat().getCentre();
    std::atomic<bool> first{false}, second{false};
    if (audioDuringJournal) {
        struct FlushProbe final : juce::ValueTree::Listener {
            FlushProbe(std::atomic<bool>& started, std::atomic<bool>& finished)
                : started(started), finished(finished) {}
            void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override {
                if (property != juce::Identifier("value") || tree["id"].toString() != "depth"
                    || started.exchange(true)) return;
                while (!finished.load()) std::this_thread::yield();
            }
            std::atomic<bool>& started;
            std::atomic<bool>& finished;
        } probe(first, second);
        processor.apvts.state.addListener(&probe);
        auto* parameter = processor.apvts.getParameter("depth");
        std::thread audio([&] {
            while (!first.load()) std::this_thread::yield();
            parameter->setValueNotifyingHost(parameter->convertTo0to1(2.0f));
            second = true;
        });
        slider->mouseDown(mouseEvent(*slider, point, point));
        slider->mouseDrag(mouseEvent(*slider, point.translated(0, -35), point, true));
        slider->mouseUp(mouseEvent(*slider, point.translated(0, -35), point, true));
        audio.join();
        processor.apvts.state.removeListener(&probe);
        return first.load() && second.load() ? 0 : 1;
    }

    struct RestoreProbe final : juce::AudioProcessorParameter::Listener {
        RestoreProbe(std::atomic<bool>& first, std::atomic<bool>& second, bool gesture)
            : first(first), second(second), gesture(gesture) {}
        void parameterValueChanged(int, float) override {
            if (gesture) return;
            first = true;
            while (!second.load()) std::this_thread::yield();
        }
        void parameterGestureChanged(int, bool starting) override {
            if (!gesture || !starting) return;
            second = true;
            while (!first.load()) std::this_thread::yield();
        }
        std::atomic<bool>& first;
        std::atomic<bool>& second;
        bool gesture;
    } before(first, second, false), during(first, second, true);
    auto state = processor.workflow.createProcessorState().createCopy();
    auto parameters = state.getChildWithName("PARAMETERS");
    auto depth = parameterInState(parameters, "depth");
    auto detail = parameterInState(parameters, "detail");
    depth.setProperty("value", 2.0f, nullptr);
    detail.setProperty("value", 7.0f, nullptr);
    parameters.moveChild(parameters.indexOf(depth), 0, nullptr);
    parameters.moveChild(parameters.indexOf(detail), 1, nullptr);
    auto* depthParameter = processor.apvts.getParameter("depth");
    auto* detailParameter = processor.apvts.getParameter("detail");
    depthParameter->addListener(&before);
    detailParameter->addListener(&during);
    std::thread restore([&] { processor.workflow.restoreProcessorState(state); });
    while (!first.load()) std::this_thread::yield();
    slider->mouseDown(mouseEvent(*slider, point, point));
    slider->mouseDrag(mouseEvent(*slider, point.translated(0, -35), point, true));
    slider->mouseUp(mouseEvent(*slider, point.translated(0, -35), point, true));
    restore.join();
    depthParameter->removeListener(&before);
    detailParameter->removeListener(&during);
    return second.load() ? 0 : 1;
}

void testJournalDoesNotHoldParameterNotificationLocks() {
    for (const auto* option : {"--journal-restore-lock", "--journal-audio-lock"}) {
        juce::ChildProcess process;
        const juce::StringArray command{
            juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(), option};
        const bool started = process.start(command);
        const bool completed = started && process.waitForProcessToFinish(5000);
        if (!completed) process.kill();
        check(completed && process.getExitCode() == 0,
              juce::String(option).contains("restore")
                  ? "concurrent host restore and actual UI gesture cannot invert parameter/workflow locks"
                  : "audio parameter notification completes while UI journal flush is held");
    }
}
}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juce;
    if (argc == 2 && juce::String(argv[1]).startsWith("--journal-"))
        return runJournalLockRegression(juce::String(argv[1]).contains("audio"));
    testJournalDoesNotHoldParameterNotificationLocks();
    testDetectionModeParameterContract();
    testLegacyBareStateAddsSoftWithoutRenormalisingExistingParameters();
    testLegacyWrappedStateMigratesBothSlotsAndCleanSnapshots();
    testLegacyPresetsAddSoftAndModernHardRoundTrips();
    testMalformedModernModeIsRejectedAtomically();
    testSidechainStateVersionsMigrateAllSnapshotsAtomically();
    testABSlotsRestoreAndCopyWithHostNotification();
    testSlotUndoHistoriesSurviveNavigation();
    testGroupedUiEditsPreserveUnrelatedAutomation();
    testPresetAndFactoryLoadsAreUndoable();
    testPresetUndoRestoresIdentityAndCleanBaseline();
    testActualEditorGesturesUseSlotHistory();
    testGestureCloseRestoreAndDiscreteControls();
    testSelectedBandRestoreDefersAttachmentReplacement();
    testSidechainToolbarGesturesRecallAndMinimumLayout();
    testWorkflowOnlyHostRestoreRefreshesOpenEditor();
    testMalformedPresetIsRejectedAtomically("FixxySpectral");
    testMalformedPresetIsRejectedAtomically("LpngSpectral");
    testBandPresenceDistinguishesBypassDeleteAndReuse();
    testAutomationEnableCreatesRetainedPresence();
    testWorkflowSurvivesStateRoundTripAndEditorReopen();
    testConcurrentHostSerializationIsSlotCoherent();
    return failures == 0 ? 0 : 1;
}
