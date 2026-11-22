#pragma once

#include "Registry.h"

namespace config {

// Records are [field ID:16, type:8, length:8, value:length].
// A zero result indicates insufficient capacity, or an empty registry.
size_t snapshot(const Registry &registry, uint8_t *data, size_t capacity);
// Checks record boundaries before any field values are consumed.
bool validTlvs(const uint8_t *data, size_t size);
// Applies compatible records to the current RAM values. Unknown IDs and values
// rejected by field validation are skipped. load() supplies defaults beforehand.
// With keepCalibration, records for calibration fields are skipped as well.
void applyTlvs(Registry &registry, const uint8_t *data, size_t size, bool keepCalibration = false);

// Byte storage adapter. Firmware uses EEPROM, and host tests supply a memory
// implementation that can simulate interrupted writes.
class Storage {
public:
    virtual ~Storage() = default;
    virtual uint8_t read(size_t address) const = 0;
    virtual void update(size_t address, uint8_t value) = 0;
};

/**
 * Explicit persistence using one active EEPROM bank and optional named profiles.
 *
 * Field identity comes from the TLV ID and type. Renaming or reordering fields
 * preserves their values. Missing or incompatible records use current defaults.
 */
class Persistence {
public:
    static constexpr size_t SlotSize = 1024;
    static constexpr size_t HeaderSize = 16;
    static constexpr size_t PayloadCapacity = SlotSize - HeaderSize;

    // repair runs after loading defaults and compatible records, allowing the
    // application to repair relationships such as two conflicting CAN IDs.
    Persistence(Registry &registry, Storage &storage, void (*repair)() = nullptr)
        : registry_(registry), storage_(storage), repair_(repair) {}

    static constexpr size_t ProfileCount = 7;
    static constexpr size_t ProfileSlotSize = 447;
    static constexpr size_t ProfileNameSize = 24;
    static constexpr size_t ProfileHeaderSize = 32;
    static constexpr size_t ProfileCapacity = ProfileSlotSize - ProfileHeaderSize;
    static constexpr size_t StorageSize = SlotSize + ProfileCount * ProfileSlotSize;
    size_t readProfile(uint8_t slot, uint8_t *name, uint8_t *payload) const;
    bool writeProfile(uint8_t slot, const uint8_t *name, const uint8_t *payload, size_t length);

    bool swapProfiles(uint8_t first, uint8_t second);

    void load(); // Loads the newest valid bank and establishes the dirty baseline.
    bool save(); // Synchronous explicit save. Returns false if writing or verification fails.
    // Saves the current RAM value of every calibration field and nothing else:
    // other fields keep what was last saved, so unsaved dashboard edits stay
    // unsaved. Used after a calibration so its result survives a power cycle.
    bool saveCalibration();
    void refreshDirty(); // Call after changing RAM values, before deciding whether to save.
    bool dirty() const { return dirty_; }
    // False when the last load() found no valid saved bank (a new board, or a
    // save cut short by power loss) and the values are the firmware defaults.
    bool loadedSaved() const { return loadedSaved_; }

private:
    struct Slot {
        uint16_t length;
        bool valid;
    };

    Slot inspect(uint8_t *payload) const;
    bool write(uint8_t *payload, uint16_t length);

    Registry &registry_;
    Storage &storage_;
    void (*repair_)();
    uint32_t savedSignature_ = 0;
    bool dirty_ = false;
    bool loadedSaved_ = false;
};

} // namespace config
