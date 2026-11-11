#include "Persistence.h"

namespace config {
namespace {
constexpr uint8_t Committed = 0xA5;
constexpr uint32_t Magic = 0x47464357; // WCFG
constexpr uint8_t Version = 1;
constexpr size_t TlvHeaderSize = 4;

// Running CRC state is left uncomplemented so header and payload can be hashed
// separately. Stored CRCs are complemented after both parts have been processed.
uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc = UINT32_MAX) {
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

bool newer(uint32_t first, uint32_t second) {
    // Compare adjacent generations correctly when the uint32_t counter wraps.
    const uint32_t distance = first - second;
    return distance != 0 && distance < 0x80000000u;
}
} // namespace

size_t snapshot(const Registry &registry, uint8_t *data, size_t capacity) {
    Writer out(data, capacity);
    for (auto *field = registry.fields(); field; field = field->next()) {
        out.put(field->id);
        out.put(static_cast<uint8_t>(field->type));
        out.put(field->width());
        field->value(out);
    }
    return out.good() ? out.size() : 0;
}

bool validTlvs(const uint8_t *data, size_t size) {
    while (size != 0) {
        if (size < TlvHeaderSize || data[3] == 0 || data[3] > size - TlvHeaderSize) return false;
        const size_t recordSize = TlvHeaderSize + data[3];
        data += recordSize;
        size -= recordSize;
    }
    return true;
}

void applyTlvs(Registry &registry, const uint8_t *data, size_t size) {
    if (!validTlvs(data, size)) return;
    const uint8_t *end = data + size;
    while (data != end) {
        Field *field = registry.find(read<uint16_t>(data));
        // update() additionally checks width, range, enum choices, and profile
        // constraints. Rejected records leave the caller's existing value intact.
        if (field && data[2] == static_cast<uint8_t>(field->type))
            field->update(data + TlvHeaderSize, data[3]);
        data += TlvHeaderSize + data[3];
    }
}

Persistence::Slot Persistence::inspect(int slot, uint8_t *payload) const {
    // Header offsets are 0 commit, 1 magic, 5 format version, 6 generation,
    // 10 payload length, and 12 CRC. CRC covers header bytes 1..11 and payload.
    const size_t start = slot * SlotSize;
    uint8_t header[HeaderSize];
    for (size_t i = 0; i < sizeof(header); ++i) header[i] = storage_.read(start + i);

    const uint16_t length = read<uint16_t>(header + 10);
    if (header[0] != Committed || read<uint32_t>(header + 1) != Magic ||
        header[5] != Version || length > PayloadCapacity) return {};

    for (size_t i = 0; i < length; ++i) payload[i] = storage_.read(start + HeaderSize + i);
    const uint32_t crc = ~crc32(payload, length, crc32(header + 1, 11));
    if (crc != read<uint32_t>(header + 12) || !validTlvs(payload, length)) return {};
    return {read<uint32_t>(header + 6), length, true};
}

void Persistence::load() {
    uint8_t payload[PayloadCapacity];
    const Slot first = inspect(0, payload);
    const Slot second = inspect(1, payload);
    registry_.defaults();
    if (second.valid && (!first.valid || newer(second.generation, first.generation))) {
        applyTlvs(registry_, payload, second.length);
    } else if (first.valid) {
        // Inspecting the second bank reused the payload buffer. Read the selected
        // first bank again before applying its records.
        inspect(0, payload);
        applyTlvs(registry_, payload, first.length);
    }
    if (repair_) repair_();

    // Compare later edits against the loaded values after migration and repair.
    const size_t length = snapshot(registry_, payload, sizeof(payload));
    savedSignature_ = crc32(payload, length);
    dirty_ = false;
}

void Persistence::refreshDirty() {
    uint8_t payload[PayloadCapacity];
    const size_t length = snapshot(registry_, payload, sizeof(payload));
    dirty_ = (length == 0 && registry_.fieldCount() != 0) || crc32(payload, length) != savedSignature_;
}

bool Persistence::save() {
    uint8_t payload[PayloadCapacity];
    const Slot first = inspect(0, payload);
    const Slot second = inspect(1, payload);
    const bool latestIsSecond = second.valid && (!first.valid || newer(second.generation, first.generation));
    const Slot latest = latestIsSecond ? second : first;
    if (latest.valid && !dirty_) return true; // Avoid unnecessary emulated EEPROM writes.

    const uint16_t length = snapshot(registry_, payload, sizeof(payload));
    if (length == 0 && registry_.fieldCount() != 0) return false;
    const uint32_t signature = crc32(payload, length);
    const uint32_t generation = latest.valid ? latest.generation + 1 : 0;
    const size_t start = latest.valid && !latestIsSecond ? SlotSize : 0;

    uint8_t header[HeaderSize];
    Writer out(header, sizeof(header));
    out.put(Committed);
    out.put(Magic);
    out.put(Version);
    out.put(generation);
    out.put(length);
    out.put(~crc32(payload, length, crc32(header + 1, 11)));

    // Write the other bank, preserving the newest committed bank throughout.
    // Committing last makes an interrupted destination fail inspection at boot.
    storage_.update(start, 0);
    for (size_t i = 1; i < sizeof(header); ++i) storage_.update(start + i, header[i]);
    for (size_t i = 0; i < length; ++i) storage_.update(start + HeaderSize + i, payload[i]);
    storage_.update(start, Committed);

    // Check the intended generation and contents as well as the bank's CRC.
    // An old valid destination must not be mistaken for a successful save.
    const Slot committed = inspect(start / SlotSize, payload);
    if (!committed.valid || committed.generation != generation || committed.length != length ||
        crc32(payload, length) != signature) return false;
    savedSignature_ = signature;
    dirty_ = false;
    return true;
}

} // namespace config
