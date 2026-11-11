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
void applyTlvs(Registry &registry, const uint8_t *data, size_t size);

// Byte storage adapter. Firmware uses EEPROM, and host tests supply a memory
// implementation that can simulate interrupted writes.
class Storage {
public:
    virtual ~Storage() = default;
    virtual uint8_t read(size_t address) const = 0;
    virtual void update(size_t address, uint8_t value) = 0;
};

/**
 * Explicit persistence using alternating EEPROM banks with CRC-32 integrity.
 * The caller supplies storage covering at least two SlotSize banks.
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

    void load(); // Loads the newest valid bank and establishes the dirty baseline.
    bool save(); // Synchronous explicit save. Returns false if writing or verification fails.
    void refreshDirty(); // Call after changing RAM values, before deciding whether to save.
    bool dirty() const { return dirty_; }

private:
    struct Slot {
        uint32_t generation;
        uint16_t length;
        bool valid;
    };

    Slot inspect(int slot, uint8_t *payload) const;

    Registry &registry_;
    Storage &storage_;
    void (*repair_)();
    uint32_t savedSignature_ = 0;
    bool dirty_ = false;
};

} // namespace config
