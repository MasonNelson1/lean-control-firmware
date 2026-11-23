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

size_t Protocol::reply(uint8_t (&response)[MaxResponseSize], bool disarmed) {
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
                         disarmed, out);
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
                         bool disarmed, Writer &out) {
    // Check the current mode at execution time, before touching RAM or EEPROM.
    const bool modifiesConfiguration = command == Command::Set || command == Command::Save ||
                                       command == Command::Revert || command == Command::Defaults ||
                                       command == Command::ProfileBegin || command == Command::ProfileChunk ||
                                       command == Command::ProfileCommit || command == Command::ProfileMove;
    if (!disarmed) {
        uploadLength_ = 0;
        if (modifiesConfiguration) return Status::NotDisarmed;
    }

    switch (command) {
    case Command::ProfileMove:
        if (size != 2) return Status::Malformed;
        if (body[0] >= Persistence::ProfileCount || body[1] >= Persistence::ProfileCount ||
            (body[0] + 1 != body[1] && body[1] + 1 != body[0])) return Status::Invalid;
        uploadLength_ = 0;
        return persistence_.swapProfiles(body[0], body[1]) ? Status::Ok : Status::Storage;
    case Command::ProfileList:
    case Command::ProfileRead:
    case Command::ProfileBegin:
    case Command::ProfileChunk:
    case Command::ProfileCommit:
        return profiles(command, body, size, out);
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

bool Protocol::selectProfile(uint8_t slot) {
    uint8_t name[Persistence::ProfileNameSize], payload[Persistence::ProfileCapacity];
    const size_t length = persistence_.readProfile(slot, name, payload);
    if (!length) return false;
    uint8_t old[Persistence::PayloadCapacity];
    const size_t oldLength = snapshot(registry_, old, sizeof(old));
    if (!oldLength) return false;
    // Like Persistence::load(): a profile saved by older firmware lacks newer
    // fields, and those must take their defaults rather than whatever the
    // previously active profile held. Incompatible records also stay at default.
    registry_.defaults();
    applyTlvs(registry_, payload, length);
    if (validate_ && !validate_()) {
        applyTlvs(registry_, old, oldLength);
        return false;
    }
    changed();
    return true;
}

Status Protocol::profiles(Command command, const uint8_t *body, size_t size, Writer &out) {
    if (command == Command::ProfileList || command == Command::ProfileRead) {
        uint8_t name[Persistence::ProfileNameSize], payload[Persistence::ProfileCapacity];
        if (command == Command::ProfileList) {
            if (size) return Status::Malformed;
            out.put(static_cast<uint8_t>(Persistence::ProfileCount));
            for (uint8_t slot = 0; slot < Persistence::ProfileCount; ++slot) {
                const bool present = persistence_.readProfile(slot, name, payload) != 0;
                out.put(static_cast<uint8_t>(present));
                if (present) out.bytes(name, sizeof(name));
            }
            return Status::Ok;
        }
        if (size != 3) return Status::Malformed;
        const size_t length = persistence_.readProfile(body[0], name, payload);
        if (!length) return Status::Unknown;
        const size_t offset = read<uint16_t>(body + 1);
        if (offset >= length) return Status::Invalid;
        const size_t remaining = length - offset;
        out.put(static_cast<uint16_t>(length));
        out.bytes(payload + offset, remaining < 384 ? remaining : 384);
        return Status::Ok;
    }

    uploadActivity_ = now_;
    if (command == Command::ProfileBegin) {
        uploadLength_ = 0;
        if (size != 3 + sizeof(uploadName_)) return Status::Malformed;
        const auto length = read<uint16_t>(body + 1);
        if (body[0] >= Persistence::ProfileCount || !length || length > sizeof(upload_) || !body[3])
            return Status::Invalid;
        uploadSlot_ = body[0];
        uploadLength_ = length;
        uploadReceived_ = 0;
        memcpy(uploadName_, body + 3, sizeof(uploadName_));
        return Status::Ok;
    }
    if (!uploadLength_) return Status::Invalid;
    if (command == Command::ProfileChunk) {
        if (size <= 2 || read<uint16_t>(body) != uploadReceived_ ||
            size - 2 > static_cast<size_t>(uploadLength_ - uploadReceived_)) {
            uploadLength_ = 0;
            return Status::Malformed;
        }
        memcpy(upload_ + uploadReceived_, body + 2, size - 2);
        uploadReceived_ += size - 2;
        return Status::Ok;
    }

    const size_t length = uploadLength_;
    uploadLength_ = 0;
    if (size || uploadReceived_ != length || !validTlvs(upload_, length)) return Status::Malformed;
    uint8_t old[Persistence::PayloadCapacity];
    const size_t oldLength = snapshot(registry_, old, sizeof(old));
    if (!oldLength) return Status::Storage;

    // Validate the candidate synchronously, then restore the active registry
    // before touching EEPROM or returning to the control loop. The dashboard
    // uploads every field it discovered; RAM-only fields (the bench-test
    // trigger) are dropped here, so a profile can never carry one.
    size_t fields = 0, kept = 0;
    uint16_t seen[Persistence::ProfileCapacity / 5];
    bool valid = true;
    for (size_t offset = 0; offset < length && valid;) {
        const auto id = read<uint16_t>(upload_ + offset);
        auto *field = registry_.find(id);
        const size_t recordSize = 4 + upload_[offset + 3];
        for (size_t i = 0; i < fields; ++i)
            if (seen[i] == id) valid = false;
        seen[fields++] = id;
        valid = valid && field && static_cast<uint8_t>(field->type) == upload_[offset + 2];
        if (valid && !field->info.persistent) {
            offset += recordSize; // skipped, not validated: update() would change RAM
            continue;
        }
        valid = valid && field->update(upload_ + offset + 4, recordSize - 4);
        memmove(upload_ + kept, upload_ + offset, recordSize); // compact in place (kept <= offset)
        kept += recordSize;
        offset += recordSize;
    }
    size_t persistentFields = 0;
    for (auto *field = registry_.fields(); field; field = field->next())
        persistentFields += field->info.persistent;
    size_t uploadedPersistent = 0;
    for (size_t i = 0; i < fields; ++i) {
        const auto *field = registry_.find(seen[i]);
        uploadedPersistent += field && field->info.persistent;
    }
    valid = valid && kept && uploadedPersistent == persistentFields && (!validate_ || validate_());
    applyTlvs(registry_, old, oldLength);
    if (!valid) return Status::Invalid;
    return persistence_.writeProfile(uploadSlot_, uploadName_, upload_, kept) ? Status::Ok : Status::Storage;
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
