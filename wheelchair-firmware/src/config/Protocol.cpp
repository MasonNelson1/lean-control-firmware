#include "Protocol.h"

namespace config {

uint16_t crc16(const uint8_t *data, size_t size) {
    uint16_t crc = UINT16_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
    }
    return crc;
}

bool Protocol::queue(const uint8_t *packet, size_t size) {
    if (!packet || size == 0 || packet[0] != RequestTag) return false;
    // The shortest request is four header bytes followed by a two-byte CRC.
    if (!pending() && size >= 6 && size <= sizeof(pending_) &&
        crc16(packet, size - 2) == read<uint16_t>(packet + size - 2)) {
        memcpy(pending_, packet, size);
        pendingSize_ = size;
    }
    return true;
}

size_t Protocol::reply(uint8_t (&response)[MaxResponseSize], float leftCommandRPM, float rightCommandRPM) {
    if (!pending()) return 0;
    response[0] = ResponseTag;
    response[1] = ProtocolVersion;
    response[2] = pending_[2];
    response[3] = pending_[3];
    // Reserve six header bytes and two checksum bytes around the response body.
    Writer out(response + 6, sizeof(response) - 8);
    Status result = Status::Version;
    if (pending_[1] == ProtocolVersion) {
        result = execute(static_cast<Command>(pending_[3]), pending_ + 4, pendingSize_ - 6,
                         leftCommandRPM, rightCommandRPM, out);
    }
    if (!out.good()) result = Status::TooLarge;
    pendingSize_ = 0;
    response[4] = static_cast<uint8_t>(result);
    response[5] = persistence_.dirty() ? 2 : 0; // Flag bit 1 reports unsaved RAM values.
    // Error replies carry only status and flags. Discard any partial body.
    const size_t length = 6 + (result == Status::Ok ? out.size() : 0);
    const uint16_t crc = crc16(response, length);
    memcpy(response + length, &crc, sizeof(crc));
    return length + sizeof(crc);
}

Status Protocol::execute(Command command, const uint8_t *body, size_t size,
                         float leftCommandRPM, float rightCommandRPM, Writer &out) {
    // Check at execution time, before touching RAM or EEPROM. Ordered <= checks
    // accept both signs at exactly 1 RPM and reject nonfinite command values.
    const bool modifiesConfiguration = command == Command::Set || command == Command::Save ||
                                       command == Command::Revert || command == Command::Defaults;
    if (modifiesConfiguration && !(std::fabs(leftCommandRPM) <= 1.0f && std::fabs(rightCommandRPM) <= 1.0f))
        return Status::Moving;

    switch (command) {
    case Command::Hello:
        if (size != 0) return Status::Malformed;
        out.put(registry_.groupCount());
        out.put(registry_.fieldCount());
        return Status::Ok;
    case Command::Group: {
        if (size != 2) return Status::Malformed;
        const auto *group = registry_.groupAt(read<uint16_t>(body));
        if (!group) return Status::Unknown;
        out.put(group->id);
        out.put(group->parent ? group->parent->id : uint16_t{0});
        out.put(static_cast<uint8_t>(group->advanced));
        out.text(group->name);
        return Status::Ok;
    }
    case Command::Describe: {
        if (size != 2) return Status::Malformed;
        const auto *field = registry_.fieldAt(read<uint16_t>(body));
        if (!field) return Status::Unknown;
        out.put(field->id);
        out.put(field->info.group.id);
        out.put(static_cast<uint8_t>(field->type));
        out.put(static_cast<uint8_t>(field->info.restart));
        out.text(field->info.name);
        out.text(field->info.description);
        out.text(field->info.units);
        // Generic field header comes first, followed by type-specific defaults,
        // constraints, and the current value. The browser renders from this data.
        field->metadata(out);
        field->value(out);
        return Status::Ok;
    }
    case Command::Read:
    case Command::Set: {
        if (size < 2 || (command == Command::Read && size != 2)) return Status::Malformed;
        auto *field = registry_.find(read<uint16_t>(body));
        if (!field) return Status::Unknown;
        if (command == Command::Set) {
            if (size - 2 != field->width()) return Status::Malformed;
            return update(*field, body + 2, size - 2, out);
        }
        field->value(out);
        return Status::Ok;
    }
    case Command::Save:
    case Command::Revert:
        if (size != 0) return Status::Malformed;
        if (command == Command::Save) return persistence_.save() ? Status::Ok : Status::Storage;
        persistence_.load();
        ++revision_;
        return Status::Ok;
    case Command::Defaults: {
        if (size != 2) return Status::Malformed;
        const uint16_t id = read<uint16_t>(body);
        const auto *group = id ? registry_.findGroup(id) : nullptr;
        if (id && !group) return Status::Unknown;
        return defaults(group);
    }
    case Command::Preview: {
        if (size != 2) return Status::Malformed;
        const auto *group = registry_.findGroup(read<uint16_t>(body));
        if (!group || !group->preview) return Status::Unknown;
        const auto &preview = *group->preview;
        out.put(preview.type);
        out.put(preview.fieldCount);
        for (uint8_t i = 0; i < preview.fieldCount; ++i) out.put(preview.fieldIds[i]);
        out.put(preview.constantCount);
        for (uint8_t i = 0; i < preview.constantCount; ++i) out.put(preview.constants[i]);
        return Status::Ok;
    }
    }
    return Status::Unknown;
}

Status Protocol::update(Field &field, const uint8_t *data, size_t size, Writer &out) {
    // Keep the old typed encoding until both local and cross-field checks pass.
    uint8_t old[MaxValueSize];
    Writer previous(old, sizeof(old));
    field.value(previous);
    if (!previous.good() || !field.update(data, size)) return Status::Invalid;
    if (validate_ && !validate_()) {
        field.update(old, previous.size());
        return Status::Invalid;
    }
    changed();
    field.value(out);
    return Status::Ok;
}

Status Protocol::defaults(const Group *group) {
    // Restoring nested groups can change several related fields at once.
    // Snapshot the registry so cross-field validation can roll back the batch.
    uint8_t old[Persistence::PayloadCapacity];
    const size_t length = snapshot(registry_, old, sizeof(old));
    if (length == 0 && registry_.fieldCount() != 0) return Status::Storage;
    registry_.defaults(group);
    if (validate_ && !validate_()) {
        applyTlvs(registry_, old, length);
        return Status::Invalid;
    }
    changed();
    return Status::Ok;
}

void Protocol::changed() {
    // Runtime changes and EEPROM saves are separate operations.
    ++revision_;
    persistence_.refreshDirty();
}

} // namespace config
