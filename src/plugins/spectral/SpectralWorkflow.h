#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

/**
    Message-thread workflow state for LpngSpectral.

    This object is owned by SpectralProcessor, not its editor, so A/B slots and
    sidebar state survive closing and reopening the window. Snapshots contain
    APVTS parameter children only; host/session metadata is serialized in the
    separate WORKFLOW tree returned by createState().
*/
class SpectralWorkflow : private juce::AudioProcessorValueTreeState::Listener {
public:
    enum class Slot { a = 0, b = 1 };

    SpectralWorkflow(juce::AudioProcessorValueTreeState& stateToUse,
                     juce::UndoManager& undoManagerToUse)
        : apvts(stateToUse), undoManager(undoManagerToUse) {
        factoryBaseline = parameterSnapshot();
        slots.fill(factoryBaseline);
        cleanSnapshots.fill(factoryBaseline);
        presenceMasks.fill(0u);
        cleanPresenceMasks = presenceMasks;
        for (int band = 0; band < kNumBands; ++band)
            apvts.addParameterListener(wbandID(band, "On"), this);
        for (auto* parameter : apvts.processor.getParameters()) {
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter)) {
                journalParameters.push_back(ranged);
            }
        }
        editBeforeValues.resize(journalParameters.size());
        editedParameters.resize(journalParameters.size());
    }

    ~SpectralWorkflow() {
        for (int band = 0; band < kNumBands; ++band)
            apvts.removeParameterListener(wbandID(band, "On"), this);
        const juce::ScopedLock lock(metadataLock);
        undoManager.clearUndoHistory();
    }

    static juce::Identifier stateType() { return {"WORKFLOW"}; }
    static juce::Identifier processorStateType() { return {"LPNGSPECTRAL_STATE"}; }

    juce::ValueTree createProcessorState() {
        const juce::ScopedLock lock(metadataLock);
        juce::ValueTree processorState(processorStateType());
        processorState.setProperty("stateVersion", 3, nullptr);
        processorState.addChild(apvts.copyState(), -1, nullptr);
        processorState.addChild(createState(), -1, nullptr);
        return processorState;
    }

    bool restoreProcessorState(const juce::ValueTree& incoming) {
        const juce::ScopedLock lock(metadataLock);
        int version = 1;
        if (!readVersion(incoming, "stateVersion", version)) return false;
        auto restored = incoming.createCopy();
        if (restored.hasType(apvts.state.getType())) {
            if (!migrateParameters(restored, version)) return false;
            apvts.replaceState(restored);
            resetFromCurrentState();
            return true;
        }

        if (!restored.hasType(processorStateType())) return false;
        const auto parameters = restored.getChildWithName(apvts.state.getType());
        if (!parameters.isValid()) return false;
        if (!migrateParameters(restored, version)) return false;

        apvts.replaceState(parameters.createCopy());
        restoreState(restored.getChildWithName(stateType()));
        return true;
    }

    Slot activeSlot() const noexcept {
        const juce::ScopedLock lock(metadataLock);
        return selectedSlot;
    }

    std::uint64_t revision() const noexcept {
        const juce::ScopedLock lock(metadataLock);
        return metadataRevision;
    }

    void selectSlot(Slot nextSlot) {
        const juce::ScopedLock lock(metadataLock);
        if (nextSlot == selectedSlot) return;
        finishPendingUiEdit();

        slots[indexOf(selectedSlot)] = parameterSnapshot();
        presenceMasks[indexOf(selectedSlot)] = currentPresenceMask();
        selectedSlot = nextSlot;

        // Navigation does not enter either slot's edit journal. The APVTS
        // manager is only attachment plumbing; its shared history is discarded.
        applySnapshot(slots[indexOf(selectedSlot)], "Switch A/B", false,
                      presenceMasks[indexOf(selectedSlot)]);
        ++metadataRevision;
    }

    void copyActiveToOther() {
        const juce::ScopedLock lock(metadataLock);
        finishPendingUiEdit();
        const auto current = parameterSnapshot();
        const auto active = indexOf(selectedSlot);
        const auto other = active == 0u ? 1u : 0u;
        presenceMasks[active] = currentPresenceMask();
        slots[active] = current;
        slots[other] = current.createCopy();
        cleanSnapshots[other] = cleanSnapshots[active].createCopy();
        presetNames[other] = presetNames[active];
        presenceMasks[other] = presenceMasks[active];
        cleanPresenceMasks[other] = cleanPresenceMasks[active];
        histories[other].clear();
        ++metadataRevision;
    }

    bool canUndo() const {
        const juce::ScopedLock lock(metadataLock);
        return histories[indexOf(selectedSlot)].cursor > 0;
    }
    bool canRedo() const {
        const juce::ScopedLock lock(metadataLock);
        const auto& history = histories[indexOf(selectedSlot)];
        return history.cursor < history.count;
    }

    bool undo() {
        const juce::ScopedLock lock(metadataLock);
        if (!isMessageThread()) return false;
        finishPendingUiEdit();
        auto& history = histories[indexOf(selectedSlot)];
        if (history.cursor == 0) return false;
        replay(history.at(--history.cursor), false);
        return true;
    }

    bool redo() {
        const juce::ScopedLock lock(metadataLock);
        if (!isMessageThread()) return false;
        finishPendingUiEdit();
        auto& history = histories[indexOf(selectedSlot)];
        if (history.cursor == history.count) return false;
        replay(history.at(history.cursor++), true);
        return true;
    }

    // Message-thread UI groups may nest (for example frequency + gain in a
    // node drag). Call these outside parameter notifications: JUCE holds its
    // parameter listener lock while notifying, including gesture callbacks.
    void beginUiEdit(const juce::String& name = {}) {
        if (!isMessageThread()) return;
        const juce::ScopedLock lock(metadataLock);
        beginEdit(name);
    }

    void endUiEdit() {
        if (!isMessageThread()) return;
        const juce::ScopedLock lock(metadataLock);
        if (suppressHistory || editDepth == 0) return;
        endEdit();
    }

    void markUiParameter(const juce::String& id) {
        if (!isMessageThread()) return;
        const juce::ScopedLock lock(metadataLock);
        if (suppressHistory || editDepth == 0) return;
        for (std::size_t i = 0; i < journalParameters.size(); ++i) {
            if (journalParameters[i]->paramID != id) continue;
            if (!editedParameters[i]) editBeforeValues[i] = journalParameters[i]->getValue();
            editedParameters[i] = true;
            if (id.length() == 8 && id.startsWith("wband") && id.endsWith("On"))
                editedPresenceBits |= bitFor(id[5] - '0');
            return;
        }
    }

    void finishUiEdit() {
        if (!isMessageThread()) return;
        const juce::ScopedLock lock(metadataLock);
        finishPendingUiEdit();
    }

    juce::Result savePreset(const juce::File& file) {
        const juce::ScopedLock lock(metadataLock);
        auto snapshot = parameterSnapshot();
        snapshot.setProperty("presetFormat", "LpngSpectral", nullptr);
        snapshot.setProperty("presetVersion", 3, nullptr);
        snapshot.setProperty("presetName", file.getFileNameWithoutExtension(), nullptr);
        snapshot.setProperty("bandPresence", static_cast<int>(currentPresenceMask()), nullptr);

        auto xml = snapshot.createXml();
        if (xml == nullptr || !file.replaceWithText(xml->toString()))
            return juce::Result::fail("Could not write preset file");

        const auto active = indexOf(selectedSlot);
        slots[active] = parameterSnapshot();
        cleanSnapshots[active] = slots[active].createCopy();
        cleanPresenceMasks[active] = currentPresenceMask();
        presetNames[active] = file.getFileNameWithoutExtension();
        ++metadataRevision;
        return juce::Result::ok();
    }

    juce::Result loadPreset(const juce::File& file) {
        auto xml = juce::XmlDocument::parse(file);
        if (xml == nullptr)
            return juce::Result::fail("The preset file is not valid XML");

        auto incoming = juce::ValueTree::fromXml(*xml);
        const juce::ScopedLock lock(metadataLock);
        int version = 1;
        if (!readVersion(incoming, "presetVersion", version)
            || !migrateParameters(incoming, version))
            return juce::Result::fail("The preset version or parameter set is invalid");
        if (const auto validation = validatePreset(incoming); validation.failed())
            return validation;

        float parsedPresence = 0.0f;
        const auto presence = incoming.hasProperty("bandPresence")
                                  && parseFiniteNumber(incoming["bandPresence"], parsedPresence)
                                  ? static_cast<std::uint32_t>(parsedPresence)
                                  : inferPresence(incoming);
        applySnapshot(incoming, "Load preset", true, presence, file.getFileNameWithoutExtension());
        const auto active = indexOf(selectedSlot);
        slots[active] = parameterSnapshot();
        return juce::Result::ok();
    }

    void loadFactoryBaseline() {
        const juce::ScopedLock lock(metadataLock);
        applySnapshot(factoryBaseline, "Load factory baseline", true, 0u, "Factory baseline");
        const auto active = indexOf(selectedSlot);
        slots[active] = parameterSnapshot();
    }

    juce::String currentPresetName() const {
        const juce::ScopedLock lock(metadataLock);
        return presetNames[indexOf(selectedSlot)];
    }

    bool isCurrentPresetDirty() {
        const juce::ScopedLock lock(metadataLock);
        const auto active = indexOf(selectedSlot);
        return currentPresenceMask() != cleanPresenceMasks[active]
               || !sameParameterValues(parameterSnapshot(), cleanSnapshots[active]);
    }

    bool isSidebarExpanded() const noexcept {
        const juce::ScopedLock lock(metadataLock);
        return sidebarExpanded;
    }
    void setSidebarExpanded(bool expanded) noexcept {
        const juce::ScopedLock lock(metadataLock);
        if (sidebarExpanded == expanded) return;
        sidebarExpanded = expanded;
        ++metadataRevision;
    }

    bool isBandPresent(int band) const noexcept {
        const juce::ScopedLock lock(metadataLock);
        return isBandPresentUnlocked(band);
    }

    int firstAbsentBand() const noexcept {
        const juce::ScopedLock lock(metadataLock);
        for (int band = 0; band < kNumBands; ++band)
            if (!isBandPresentUnlocked(band)) return band;
        return -1;
    }

    std::uint32_t bandPresenceMask() const noexcept {
        const juce::ScopedLock lock(metadataLock);
        return currentPresenceMask();
    }

    void createBand(int band, float frequencyHz, float gainDb) {
        const juce::ScopedLock lock(metadataLock);
        if (!isValidBand(band) || isBandPresentUnlocked(band)) return;
        beginUiEdit("Add band");
        setCurrentPresenceMask(currentPresenceMask() | bitFor(band));
        setParameter(wbandID(band, "Freq"), frequencyHz);
        setParameter(wbandID(band, "Gain"), gainDb);
        setParameter(wbandID(band, "On"), 1.0f);
        endUiEdit();
    }

    void deleteBand(int band) {
        const juce::ScopedLock lock(metadataLock);
        if (!isValidBand(band) || !isBandPresentUnlocked(band)) return;
        beginUiEdit("Delete band");
        setCurrentPresenceMask(currentPresenceMask() & ~bitFor(band));
        for (const auto* suffix : {"On", "Type", "Freq", "Gain", "Q"}) {
            if (auto* parameter = apvts.getParameter(wbandID(band, suffix)))
                setParameter(wbandID(band, suffix),
                             parameter->convertFrom0to1(parameter->getDefaultValue()));
        }
        endUiEdit();
    }

    juce::ValueTree createState() {
        const juce::ScopedLock lock(metadataLock);
        slots[indexOf(selectedSlot)] = parameterSnapshot();
        presenceMasks[indexOf(selectedSlot)] = currentPresenceMask();

        juce::ValueTree workflowState(stateType());
        workflowState.setProperty("activeSlot", static_cast<int>(selectedSlot), nullptr);
        workflowState.setProperty("sidebarExpanded", sidebarExpanded, nullptr);
        for (std::size_t slot = 0; slot < slots.size(); ++slot) {
            juce::ValueTree slotState(slot == 0u ? "SLOT_A" : "SLOT_B");
            slotState.setProperty("presetName", presetNames[slot], nullptr);
            slotState.setProperty("bandPresence", static_cast<int>(presenceMasks[slot]), nullptr);
            slotState.setProperty("cleanBandPresence", static_cast<int>(cleanPresenceMasks[slot]), nullptr);
            juce::ValueTree currentState("CURRENT");
            currentState.addChild(slots[slot].createCopy(), -1, nullptr);
            slotState.addChild(currentState, -1, nullptr);
            juce::ValueTree cleanState("CLEAN");
            cleanState.addChild(cleanSnapshots[slot].createCopy(), -1, nullptr);
            slotState.addChild(cleanState, -1, nullptr);
            workflowState.addChild(slotState, -1, nullptr);
        }
        return workflowState;
    }

    void restoreState(const juce::ValueTree& workflowState) {
        const juce::ScopedLock lock(metadataLock);
        if (!workflowState.hasType(stateType())) {
            resetFromCurrentState();
            return;
        }

        for (std::size_t slot = 0; slot < slots.size(); ++slot) {
            const auto child = workflowState.getChildWithName(slot == 0u ? "SLOT_A" : "SLOT_B");
            auto parameterState = child.getChildWithName("CURRENT").getChildWithName(apvts.state.getType());
            if (!parameterState.isValid())
                parameterState = child.getChildWithName(apvts.state.getType());  // pre-CURRENT workflow state
            if (parameterState.isValid()) {
                slots[slot] = parameterState.createCopy();
                const auto cleanState = child.getChildWithName("CLEAN").getChildWithName(apvts.state.getType());
                cleanSnapshots[slot] = cleanState.isValid() ? cleanState.createCopy()
                                                            : slots[slot].createCopy();
                presenceMasks[slot] = child.hasProperty("bandPresence")
                                          ? static_cast<std::uint32_t>(static_cast<int>(child["bandPresence"]))
                                          : inferPresence(parameterState);
                cleanPresenceMasks[slot] = child.hasProperty("cleanBandPresence")
                                               ? static_cast<std::uint32_t>(
                                                     static_cast<int>(child["cleanBandPresence"]))
                                               : presenceMasks[slot];
            }
            if (child.isValid())
                presetNames[slot] = child.getProperty("presetName", "Factory baseline").toString();
        }

        selectedSlot = static_cast<int>(workflowState.getProperty("activeSlot", 0)) == 1
                           ? Slot::b
                           : Slot::a;
        sidebarExpanded = static_cast<bool>(workflowState.getProperty("sidebarExpanded", true));

        // The processor restores the active PARAMETERS sibling before calling
        // this function. Trust that host state as authoritative and mirror it
        // into the active slot without generating another parameter gesture.
        slots[indexOf(selectedSlot)] = parameterSnapshot();
        clearHistories();
        undoManager.clearUndoHistory();
        ++metadataRevision;
    }

    void resetFromCurrentState() {
        const juce::ScopedLock lock(metadataLock);
        const auto current = parameterSnapshot();
        selectedSlot = Slot::a;
        slots.fill(current);
        cleanSnapshots.fill(current);
        presenceMasks.fill(inferPresence(current));
        cleanPresenceMasks = presenceMasks;
        presetNames.fill("Current");
        sidebarExpanded = true;
        clearHistories();
        undoManager.clearUndoHistory();
        ++metadataRevision;
    }

private:
    static constexpr int kNumBands = 8;

    struct PresetIdentity {
        juce::ValueTree cleanSnapshot;
        juce::String name;
        std::uint32_t cleanPresence;
    };

    struct ParameterEdit {
        juce::RangedAudioParameter* parameter;
        float before, after;  // normalized host values, never whole-state replay
    };

    struct UiEdit {
        std::vector<ParameterEdit> parameters;
        std::uint32_t beforePresence = 0, afterPresence = 0, presenceBits = 0;
        std::optional<PresetIdentity> beforeIdentity, afterIdentity;
    };

    struct History {
        static constexpr std::size_t capacity = 64;
        std::array<UiEdit, capacity> entries;
        std::size_t start = 0, count = 0, cursor = 0;

        UiEdit& at(std::size_t index) { return entries[(start + index) % capacity]; }
        // Logical reset only: host restore may run off the message thread.
        // Old storage is retired on the next message-thread edit/destruction.
        void clear() noexcept { start = count = cursor = 0; }
        void push(UiEdit edit) {
            count = cursor;
            if (count == capacity) {
                start = (start + 1) % capacity;
                --count;
                --cursor;
            }
            at(cursor) = std::move(edit);
            count = ++cursor;
        }
    };

    static bool isMessageThread() noexcept {
        const auto* manager = juce::MessageManager::getInstanceWithoutCreating();
        return manager != nullptr && manager->isThisTheMessageThread();
    }

    PresetIdentity presetIdentity() const {
        const auto slot = indexOf(selectedSlot);
        return {cleanSnapshots[slot], presetNames[slot], cleanPresenceMasks[slot]};
    }

    void beginEdit(const juce::String&) {
        if (suppressHistory || !isMessageThread()) return;
        if (editDepth++ != 0) return;
        std::fill(editedParameters.begin(), editedParameters.end(), false);
        editBeforePresence = currentPresenceMask();
        editedPresenceBits = 0;
        editBeforeIdentity = presetIdentity();
        editedIdentity = false;
    }

    void endEdit() {
        if (suppressHistory || editDepth == 0 || --editDepth != 0) return;
        UiEdit edit;
        for (std::size_t i = 0; i < journalParameters.size(); ++i) {
            const float after = journalParameters[i]->getValue();
            if (editedParameters[i] && std::abs(after - editBeforeValues[i]) > 1.0e-7f)
                edit.parameters.push_back({journalParameters[i], editBeforeValues[i], after});
        }
        edit.beforePresence = editBeforePresence;
        edit.afterPresence = currentPresenceMask();
        edit.presenceBits = (edit.beforePresence ^ edit.afterPresence) & editedPresenceBits;
        if (editedIdentity) {
            edit.beforeIdentity = editBeforeIdentity;
            edit.afterIdentity = presetIdentity();
        }
        if (!edit.parameters.empty() || edit.presenceBits != 0 || editedIdentity)
            histories[indexOf(selectedSlot)].push(std::move(edit));
        slots[indexOf(selectedSlot)] = parameterSnapshot();
        undoManager.clearUndoHistory();
        ++metadataRevision;
    }

    void finishPendingUiEdit() {
        if (editDepth == 0 || !isMessageThread()) return;
        editDepth = 1;
        endEdit();
    }

    void clearHistories() noexcept {
        for (auto& history : histories) history.clear();
        editDepth = 0;
    }

    void replay(const UiEdit& edit, bool forward) {
        const juce::ScopedValueSetter<bool> guard(suppressHistory, true);
        for (const auto& change : edit.parameters)
            setParameter(*change.parameter, forward ? change.after : change.before);
        if (edit.presenceBits != 0) {
            const auto presence = forward ? edit.afterPresence : edit.beforePresence;
            setCurrentPresenceMask((currentPresenceMask() & ~edit.presenceBits)
                                   | (presence & edit.presenceBits));
        }
        const auto& identity = forward ? edit.afterIdentity : edit.beforeIdentity;
        if (identity.has_value()) setPresetIdentity(indexOf(selectedSlot), *identity);
        slots[indexOf(selectedSlot)] = parameterSnapshot();
        undoManager.clearUndoHistory();
        ++metadataRevision;
    }

    static std::size_t indexOf(Slot slot) noexcept {
        return slot == Slot::a ? 0u : 1u;
    }

    static bool isValidBand(int band) noexcept {
        return band >= 0 && band < kNumBands;
    }

    static std::uint32_t bitFor(int band) noexcept {
        return std::uint32_t{1} << static_cast<unsigned int>(band);
    }

    std::uint32_t currentPresenceMask() const noexcept {
        auto& retained = presenceMasks[indexOf(selectedSlot)];
        retained |= observedEnabledMask.exchange(0u, std::memory_order_relaxed);
        for (int band = 0; band < kNumBands; ++band) {
            const auto* enabled = apvts.getRawParameterValue(wbandID(band, "On"));
            if (enabled != nullptr && enabled->load() > 0.5f) retained |= bitFor(band);
        }
        return retained;
    }

    void setCurrentPresenceMask(std::uint32_t mask) noexcept {
        if (editDepth > 0 && !suppressHistory)
            editedPresenceBits |= presenceMasks[indexOf(selectedSlot)] ^ (mask & 0xffu);
        presenceMasks[indexOf(selectedSlot)] = mask & 0xffu;
        ++metadataRevision;
    }

    void setPresetIdentity(std::size_t slot, const PresetIdentity& identity) {
        if (editDepth > 0 && !suppressHistory) editedIdentity = true;
        cleanSnapshots[slot] = identity.cleanSnapshot;
        presetNames[slot] = identity.name;
        cleanPresenceMasks[slot] = identity.cleanPresence;
        ++metadataRevision;
    }

    static juce::String wbandID(int band, const char* suffix) {
        return "wband" + juce::String(band) + suffix;
    }

    void parameterChanged(const juce::String& paramID, float newValue) override {
        if (newValue <= 0.5f || paramID.length() != 8
            || paramID[0] != 'w' || paramID[1] != 'b' || paramID[2] != 'a'
            || paramID[3] != 'n' || paramID[4] != 'd'
            || paramID[6] != 'O' || paramID[7] != 'n')
            return;
        const int band = paramID[5] - '0';
        if (isValidBand(band))
            observedEnabledMask.fetch_or(bitFor(band), std::memory_order_relaxed);
    }

    juce::ValueTree parameterSnapshot() {
        auto snapshot = apvts.copyState();
        snapshot.removeAllProperties(nullptr);
        return snapshot;
    }

    bool isBandPresentUnlocked(int band) const noexcept {
        return isValidBand(band) && (currentPresenceMask() & bitFor(band)) != 0u;
    }

    static bool readVersion(const juce::ValueTree& tree, const char* property, int& version) {
        float parsed = 1.0f;
        if (tree.hasProperty(property) && !parseFiniteNumber(tree[property], parsed)) return false;
        if (parsed != 1.0f && parsed != 2.0f && parsed != 3.0f) return false;
        version = static_cast<int>(parsed);
        return true;
    }

    // Only edit the incoming copy. Migrate active, A/B and clean PARAMETERS
    // snapshots together, then validate all of them before touching live state.
    bool migrateParameters(juce::ValueTree tree, int version) {
        if (tree.hasType(apvts.state.getType())) {
            for (const auto* id : {"detectionMode", "externalSidechain", "sidechainListen"}) {
                const bool mayBeMissing = juce::String(id) == "detectionMode" ? version == 1 : version < 3;
                if (mayBeMissing && !tree.getChildWithProperty("id", id).isValid()) {
                    auto value = factoryBaseline.getChildWithProperty("id", id).createCopy();
                    value.setProperty("value", 0.0f, nullptr);
                    tree.addChild(value, -1, nullptr);
                }
            }
            return validatePreset(tree).wasOk();
        }
        for (auto child : tree)
            if (!migrateParameters(child, version)) return false;
        return true;
    }

    juce::Result validatePreset(const juce::ValueTree& incoming) {
        if (!incoming.isValid() || incoming.getType() != apvts.state.getType())
            return juce::Result::fail("This is not a fixxy Spectral preset");

        if (incoming.hasProperty("presetFormat")
            && incoming["presetFormat"].toString() != "LpngSpectral")
            return juce::Result::fail("The preset belongs to another product");

        if (incoming.hasProperty("presetVersion")) {
            int version = 1;
            if (!readVersion(incoming, "presetVersion", version))
                return juce::Result::fail("The preset version is not supported");
        }

        if (incoming.hasProperty("bandPresence")) {
            const auto presence = incoming["bandPresence"];
            float parsedPresence = 0.0f;
            if (!parseFiniteNumber(presence, parsedPresence)
                || parsedPresence != std::floor(parsedPresence)
                || parsedPresence < 0.0f || parsedPresence > 255.0f)
                return juce::Result::fail("The preset band metadata is invalid");
        }

        const auto expected = parameterSnapshot();
        if (incoming.getNumChildren() != expected.getNumChildren())
            return juce::Result::fail("The preset parameter set is incomplete");

        for (const auto& expectedChild : expected) {
            const auto id = expectedChild.getProperty("id").toString();
            int matches = 0;
            juce::ValueTree incomingChild;
            for (const auto& candidate : incoming) {
                if (candidate.getProperty("id").toString() == id) {
                    ++matches;
                    incomingChild = candidate;
                }
            }
            if (matches != 1 || !incomingChild.hasType(expectedChild.getType())
                || !incomingChild.hasProperty("value"))
                return juce::Result::fail("The preset parameter set is invalid");

            float value = 0.0f;
            if (!parseFiniteNumber(incomingChild["value"], value))
                return juce::Result::fail("A preset parameter is not numeric and finite");
            auto* parameter = apvts.getParameter(id);
            if (parameter == nullptr)
                return juce::Result::fail("A preset parameter is unknown");

            const auto rangeStart = parameter->convertFrom0to1(0.0f);
            const auto rangeEnd = parameter->convertFrom0to1(1.0f);
            if (value < rangeStart - 1.0e-6f || value > rangeEnd + 1.0e-6f)
                return juce::Result::fail("A preset parameter is outside its supported range");
            if ((dynamic_cast<juce::AudioParameterBool*>(parameter) != nullptr
                 || dynamic_cast<juce::AudioParameterChoice*>(parameter) != nullptr)
                && value != std::round(value))
                return juce::Result::fail("A discrete preset parameter is fractional");
        }
        return juce::Result::ok();
    }

    static bool parseFiniteNumber(const juce::var& source, float& destination) {
        double parsed = 0.0;
        if (source.isDouble() || source.isInt() || source.isInt64()) {
            parsed = static_cast<double>(source);
        } else if (source.isString()) {
            const auto text = source.toString().trim();
            const auto utf8 = text.toRawUTF8();
            char* end = nullptr;
            errno = 0;
            parsed = std::strtod(utf8, &end);
            if (end == utf8 || end == nullptr || *end != '\0' || errno == ERANGE)
                return false;
        } else {
            return false;
        }

        if (!std::isfinite(parsed)
            || parsed < -static_cast<double>(std::numeric_limits<float>::max())
            || parsed > static_cast<double>(std::numeric_limits<float>::max()))
            return false;
        destination = static_cast<float>(parsed);
        return std::isfinite(destination);
    }

    void applySnapshot(const juce::ValueTree& snapshot,
                       const juce::String& transactionName,
                       bool undoable,
                       std::optional<std::uint32_t> presence = std::nullopt,
                       std::optional<juce::String> presetName = std::nullopt) {
        if (undoable) beginUiEdit(transactionName);
        const juce::ScopedValueSetter<bool> guard(suppressHistory, !undoable);

        if (presence.has_value()) {
            setCurrentPresenceMask(*presence);
        }

        for (const auto& child : snapshot) {
            const auto id = child.getProperty("id").toString();
            auto* parameter = apvts.getParameter(id);
            if (parameter == nullptr || !child.hasProperty("value")) continue;

            float denormalised = 0.0f;
            if (!parseFiniteNumber(child.getProperty("value"), denormalised)) continue;
            const auto normalised = parameter->convertTo0to1(denormalised);
            if (std::abs(parameter->getValue() - normalised) <= 1.0e-7f) continue;

            setParameter(*parameter, normalised);
        }

        apvts.copyState();
        if (presetName.has_value()) {
            const auto slot = indexOf(selectedSlot);
            PresetIdentity after{parameterSnapshot(), *presetName,
                                 presence.has_value() ? *presence : currentPresenceMask()};
            setPresetIdentity(slot, after);
        }
        if (undoable) endUiEdit();
        else undoManager.clearUndoHistory();
    }

    void setParameter(const juce::String& id, float denormalised) {
        if (auto* parameter = apvts.getParameter(id))
            setParameter(*parameter, parameter->convertTo0to1(denormalised));
    }

    void setParameter(juce::RangedAudioParameter& parameter, float normalised) {
        if (std::abs(parameter.getValue() - normalised) <= 1.0e-7f) return;
        markUiParameter(parameter.paramID);
        parameter.beginChangeGesture();
        parameter.setValueNotifyingHost(normalised);
        parameter.endChangeGesture();
    }

    std::uint32_t inferPresence(const juce::ValueTree& snapshot) const {
        std::uint32_t result = 0u;
        for (int band = 0; band < kNumBands; ++band) {
            bool present = false;
            for (const auto* suffix : {"On", "Type", "Freq", "Gain", "Q"}) {
                const auto id = wbandID(band, suffix);
                const auto child = findParameter(snapshot, id);
                const auto* parameter = apvts.getParameter(id);
                if (!child.isValid() || parameter == nullptr) continue;
                const auto value = static_cast<float>(child.getProperty("value"));
                const auto normalisedValue = parameter->convertTo0to1(value);
                if (std::abs(normalisedValue - parameter->getDefaultValue()) > 1.0e-5f) {
                    present = true;
                    break;
                }
            }
            if (present) result |= bitFor(band);
        }
        return result;
    }

    static bool sameParameterValues(const juce::ValueTree& lhs,
                                    const juce::ValueTree& rhs) {
        if (lhs.getNumChildren() != rhs.getNumChildren()) return false;
        for (const auto& leftChild : lhs) {
            const auto id = leftChild.getProperty("id").toString();
            const auto rightChild = findParameter(rhs, id);
            if (!rightChild.isValid()) return false;
            const auto leftValue = static_cast<float>(leftChild.getProperty("value"));
            const auto rightValue = static_cast<float>(rightChild.getProperty("value"));
            if (std::abs(leftValue - rightValue) > 1.0e-6f) return false;
        }
        return true;
    }

    static juce::ValueTree findParameter(const juce::ValueTree& tree,
                                         const juce::String& id) {
        for (const auto& child : tree)
            if (child.getProperty("id").toString() == id) return child;
        return {};
    }

    juce::AudioProcessorValueTreeState& apvts;
    juce::UndoManager& undoManager;
    mutable juce::CriticalSection metadataLock;
    std::array<History, 2> histories;
    std::vector<juce::RangedAudioParameter*> journalParameters;
    std::vector<float> editBeforeValues;
    std::vector<bool> editedParameters;
    PresetIdentity editBeforeIdentity;
    std::uint32_t editBeforePresence = 0, editedPresenceBits = 0;
    int editDepth = 0;
    bool suppressHistory = false, editedIdentity = false;
    juce::ValueTree factoryBaseline;
    std::array<juce::ValueTree, 2> slots;
    std::array<juce::ValueTree, 2> cleanSnapshots;
    std::array<juce::String, 2> presetNames{{"Factory baseline", "Factory baseline"}};
    mutable std::array<std::uint32_t, 2> presenceMasks{};
    std::array<std::uint32_t, 2> cleanPresenceMasks{};
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    mutable std::atomic<std::uint32_t> observedEnabledMask{0u};
    Slot selectedSlot = Slot::a;
    bool sidebarExpanded = true;
    std::uint64_t metadataRevision = 0;
};
