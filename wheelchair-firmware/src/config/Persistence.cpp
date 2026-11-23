#include "Persistence.h"

namespace config {
namespace {
constexpr uint8_t Committed = 0xA5;
constexpr uint32_t Magic = 0x47464357; // WCFG
constexpr uint8_t Version = 2;
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

} // namespace

size_t snapshot(const Registry &registry, uint8_t *data, size_t capacity) {
    Writer out(data, capacity);
    for (auto *field = registry.fields(); field; field = field->next()) {
        if (!field->info.persistent) continue;
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
        if (field && field->info.persistent && data[2] == static_cast<uint8_t>(field->type))
            field->update(data + TlvHeaderSize, data[3]);
        data += TlvHeaderSize + data[3];
    }
}

Persistence::Slot Persistence::inspect(uint8_t *payload) const {
    // Header offsets are 0 commit, 1 magic, 5 format version, 6 generation,
    // 10 payload length, and 12 CRC. CRC covers header bytes 1..11 and payload.
    uint8_t header[HeaderSize];
    for (size_t i = 0; i < sizeof(header); ++i) header[i] = storage_.read(i);

    const uint16_t length = read<uint16_t>(header + 10);
    if (header[0] != Committed || read<uint32_t>(header + 1) != Magic ||
        header[5] != Version || length > PayloadCapacity) return {};

    for (size_t i = 0; i < length; ++i) payload[i] = storage_.read(HeaderSize + i);
    const uint32_t crc = ~crc32(payload, length, crc32(header + 1, 11));
    if (crc != read<uint32_t>(header + 12) || !validTlvs(payload, length)) return {};
    return {length, true};
}

void Persistence::load() {
    uint8_t payload[PayloadCapacity];
    const Slot active = inspect(payload);
    registry_.defaults();
    if (active.valid) applyTlvs(registry_, payload, active.length);
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
    if (inspect(payload).valid && !dirty_) return true;

    const uint16_t length = snapshot(registry_, payload, sizeof(payload));
    if (length == 0 && registry_.fieldCount() != 0) return false;
    const uint32_t signature = crc32(payload, length);
    uint8_t header[HeaderSize];
    Writer out(header, sizeof(header));
    out.put(Committed);
    out.put(Magic);
    out.put(Version);
    out.put(uint32_t{0});
    out.put(length);
    out.put(~crc32(payload, length, crc32(header + 1, 11)));

    storage_.update(0, 0);
    for (size_t i = 1; i < sizeof(header); ++i) storage_.update(i, header[i]);
    for (size_t i = 0; i < length; ++i) storage_.update(HeaderSize + i, payload[i]);
    storage_.update(0, Committed);

    const Slot committed = inspect(payload);
    if (!committed.valid || committed.length != length ||
        crc32(payload, length) != signature) return false;
    savedSignature_ = signature;
    dirty_ = false;
    return true;
}

size_t Persistence::readProfile(uint8_t slot, uint8_t *name, uint8_t *payload) const {
    if (slot >= ProfileCount) return 0;
    const size_t address = SlotSize + slot * ProfileSlotSize;
    uint8_t header[ProfileHeaderSize];
    for (size_t i = 0; i < sizeof(header); ++i) header[i] = storage_.read(address + i);
    const size_t length = read<uint16_t>(header + 2);
    if (header[0] != Committed || header[1] != 'P' || !header[4] ||
        !length || length > ProfileCapacity) return 0;
    for (size_t i = 0; i < length; ++i)
        payload[i] = storage_.read(address + sizeof(header) + i);
    if (crc32(payload, length, crc32(header + 1, 27)) !=
        read<uint32_t>(header + 28) || !validTlvs(payload, length)) return 0;
    memcpy(name, header + 4, ProfileNameSize);
    return length;
}

bool Persistence::writeProfile(uint8_t slot, const uint8_t *name, const uint8_t *payload, size_t length) {
    if (slot >= ProfileCount || !name[0] || !length || length > ProfileCapacity ||
        !validTlvs(payload, length)) return false;
    const size_t address = SlotSize + slot * ProfileSlotSize;
    uint8_t header[ProfileHeaderSize] = {};
    Writer out(header, sizeof(header));
    out.put(Committed);
    out.put(uint8_t{'P'}); // Profile format marker, version 1.
    out.put(static_cast<uint16_t>(length));
    out.bytes(name, ProfileNameSize);
    out.put(crc32(payload, length, crc32(header + 1, 27)));
    storage_.update(address, 0);
    for (size_t i = 1; i < sizeof(header); ++i) storage_.update(address + i, header[i]);
    for (size_t i = 0; i < length; ++i) storage_.update(address + sizeof(header) + i, payload[i]);
    storage_.update(address, Committed);
    uint8_t savedName[ProfileNameSize], savedPayload[ProfileCapacity];
    return readProfile(slot, savedName, savedPayload) == length &&
           memcmp(savedName, name, ProfileNameSize) == 0 &&
           memcmp(savedPayload, payload, length) == 0;
}

bool Persistence::swapProfiles(uint8_t first, uint8_t second) {
    if (first >= ProfileCount || second >= ProfileCount || first == second) return false;
    uint8_t firstName[ProfileNameSize], secondName[ProfileNameSize];
    uint8_t firstPayload[ProfileCapacity], secondPayload[ProfileCapacity];
    const size_t firstLength = readProfile(first, firstName, firstPayload);
    const size_t secondLength = readProfile(second, secondName, secondPayload);
    if (!firstLength || !writeProfile(second, firstName, firstPayload, firstLength)) return false;
    if (secondLength) return writeProfile(first, secondName, secondPayload, secondLength);
    const size_t address = SlotSize + first * ProfileSlotSize;
    storage_.update(address, 0);
    return storage_.read(address) == 0;
}

} // namespace config
