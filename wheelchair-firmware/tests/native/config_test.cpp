// g++ -std=c++17 -Wall -Wextra -Werror -I src tests/native/config_test.cpp src/config/Registry.cpp src/config/Persistence.cpp src/config/Protocol.cpp -o config_test
#include "config/Protocol.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <limits>

using namespace config;

class MemoryStorage : public Storage {
public:
    MemoryStorage() { data.fill(0xff); }

    uint8_t read(size_t address) const override { return data.at(address); }

    void update(size_t address, uint8_t value) override {
        if (data.at(address) == value || writesLeft == 0) return;
        if (writesLeft > 0) --writesLeft;
        data[address] = value;
    }

    std::array<uint8_t, 2 * Persistence::SlotSize> data;
    int writesLeft = -1;
};

template<typename T> bool set(Field &field, T value) {
    return field.update(reinterpret_cast<const uint8_t *>(&value), sizeof(value));
}

static bool setProfile(Field &field, Editor editor, const float *values, size_t count) {
    uint8_t raw[MaxValueSize];
    Writer writer(raw, sizeof(raw));
    writer.put(static_cast<uint8_t>(editor));
    writer.bytes(values, count * sizeof(float));
    return writer.good() && field.update(raw, writer.size());
}

static void validation() {
    Registry registry;
    Group parent(registry, 1, "Parent", true);
    Group child(registry, 2, "Child", false, &parent);
    assert(child.advanced && child.within(&parent));
    ConfigField<float> fraction(registry, 10, {child, "Fraction", ""}, .5f, 0, 1);
    ConfigField<double> precise(registry, 11, {child, "Double", ""}, 1, 0, 2);
    ConfigField<uint64_t> wide(registry, 12, {child, "Unsigned 64-bit", ""}, 0, 0, UINT64_MAX);
    ConfigField<int16_t> small(registry, 13, {child, "Signed 16-bit", ""}, 0, -100, 100);
    ConfigField<bool> enabled(registry, 14, {child, "Enabled", ""}, false);
    enum class Mode : uint8_t { Lean = 3, Remote = 8 };
    const Choice modes[] = {{3, "Lean"}, {8, "Remote"}};
    EnumField<Mode, 2> mode(registry, 15, {parent, "Mode", ""}, Mode::Remote, modes);
    const float knee[] = {.5f, .25f};
    const Parameter limits[] = {{"Input", .1f, .9f}, {"Output", .02f, .9f}};
    ProfileField<2> curve(registry, 16, {child, "Curve", ""}, Editor::SignedPower, knee, limits);
    const float knots[] = {0, 0, .5f, .3f, 1, 1};
    const Parameter axes[] = {{"X0", 0, 0}, {"Y0", 0, 1}, {"X1", 0, 1},
                              {"Y1", 0, 1}, {"X2", 1, 1}, {"Y2", 0, 1}};
    ProfileField<6> profile(registry, 17, {child, "Acceleration", ""}, Editor::PiecewiseLinear, knots, axes);

    assert(registry.groupCount() == 2 && registry.fieldCount() == 8);
    assert(!set(fraction, std::numeric_limits<float>::quiet_NaN()) && !set(fraction, 1.01f));
    assert(set(fraction, .8f) && fraction.get() == .8f);
    assert(!set(precise, std::numeric_limits<double>::infinity()) && set(precise, 1.123456789));
    assert(set(wide, UINT64_MAX) && wide.get() == UINT64_MAX && wide.type == Type::U64);
    assert(!set(small, int16_t{-101}) && set(small, int16_t{-100}));
    assert(!set(enabled, uint8_t{2}) && set(enabled, uint8_t{1}) && enabled.get());
    assert(!set(mode, uint8_t{0}) && set(mode, uint8_t{3}) && mode.get() == Mode::Lean);

    const float badKnee[] = {.3f, 1.0f};
    const float goodKnee[] = {.4f, .3f};
    assert(!setProfile(curve, Editor::SignedPower, badKnee, 2));
    const float amplifiedKnee[] = {.3f, .4f};
    assert(setProfile(curve, Editor::SignedPower, amplifiedKnee, 2));
    assert(!setProfile(curve, Editor::PiecewiseLinear, goodKnee, 2));
    assert(setProfile(curve, Editor::SignedPower, goodKnee, 2));
    const float badKnots[] = {0, 0, 1, .3f, 1, 1};
    assert(!setProfile(profile, Editor::PiecewiseLinear, badKnots, 6));
    assert(setProfile(profile, Editor::PiecewiseLinear, knots, 6));

    registry.defaults(&child);
    assert(fraction.get() == .5f && !enabled.get() && mode.get() == Mode::Lean);
    assert(curve.get()[0] == .5f && curve.get()[1] == .25f);
}

static void persistenceAndMigration() {
    MemoryStorage storage;
    Registry old;
    Group original(old, 1, "Original");
    ConfigField<uint16_t> renamed(old, 1, {original, "Original name", ""}, 1, 0, 99);
    ConfigField<uint16_t> retyped(old, 2, {original, "Was unsigned", ""}, 1, 0, 99);
    ConfigField<uint16_t> removed(old, 3, {original, "Removed", ""}, 1, 0, 99);
    ConfigField<uint16_t> narrowed(old, 4, {original, "Had wide range", ""}, 1, 0, 99);
    Persistence originalStore(old, storage);
    originalStore.load();
    assert(!originalStore.dirty());
    assert(set(renamed, uint16_t{28}) && set(retyped, uint16_t{54}));
    assert(set(removed, uint16_t{19}) && set(narrowed, uint16_t{95}));
    originalStore.refreshDirty();
    assert(originalStore.dirty() && originalStore.save() && !originalStore.dirty());

    Registry current;
    Group replacement(current, 2, "Replacement");
    ConfigField<uint8_t> added(current, 5, {replacement, "Added", ""}, 7, 0, 9);
    ConfigField<int16_t> changed(current, 2, {replacement, "Now signed", ""}, -2, -100, 100);
    ConfigField<uint16_t> reordered(current, 1, {replacement, "Renamed", ""}, 6, 0, 99);
    ConfigField<uint16_t> range(current, 4, {replacement, "Narrow range", ""}, 10, 0, 50);
    Persistence currentStore(current, storage);
    currentStore.load();
    assert(reordered.get() == 28 && changed.get() == -2 && added.get() == 7 && range.get() == 10);

    uint8_t invalid[] = {1, 0, static_cast<uint8_t>(Type::U16), 2, 17};
    assert(!validTlvs(invalid, sizeof(invalid)));
    applyTlvs(current, invalid, sizeof(invalid));
    assert(reordered.get() == 28);

    for (int budget = 0; budget < 48; ++budget) {
        MemoryStorage interrupted = storage;
        Persistence store(current, interrupted);
        store.load();
        assert(set(reordered, uint16_t{42}));
        store.refreshDirty();
        interrupted.writesLeft = budget;
        const bool success = store.save();
        store.load();
        assert(reordered.get() == (success ? 42 : 28));
    }

    assert(set(reordered, uint16_t{45}));
    currentStore.refreshDirty();
    assert(currentStore.save());
    assert(set(reordered, uint16_t{50}));
    currentStore.refreshDirty();
    storage.writesLeft = 0;
    assert(!currentStore.save()); // A stale but valid destination is not a successful write.
    currentStore.load();
    assert(reordered.get() == 45);
    storage.data[Persistence::SlotSize + Persistence::HeaderSize + 4] ^= 1;
    currentStore.load();
    assert(reordered.get() == 28); // The previous bank survived corruption of the latest one.
}

static bool crossValidationAccepted = true;
static bool validateCrossField() { return crossValidationAccepted; }

static void protocolHandling() {
    const uint8_t crcSample[] = {'1','2','3','4','5','6','7','8','9'};
    assert(crc16(crcSample, sizeof(crcSample)) == 0x29b1);
    MemoryStorage storage;
    Registry registry;
    Group group(registry, 1, "Test");
    ConfigField<uint8_t> field(registry, 23, {group, "Value", "A number", "units"}, 4, 0, 10);
    const uint16_t previewFields[] = {field.id};
    const float previewConstants[] = {2.25f};
    const GroupPreview preview{7, previewFields, 1, previewConstants, 1};
    group.preview = &preview;
    Persistence store(registry, storage);
    store.load();
    Protocol protocol(registry, store, validateCrossField);
    uint8_t response[MaxResponseSize];

    auto exchange = [&](Command command, const uint8_t *body, size_t size,
                        float leftRPM = 0, float rightRPM = 0, uint8_t version = 1) {
        uint8_t request[MaxRequestSize];
        Writer out(request, sizeof(request));
        out.put(RequestTag);
        out.put(version);
        out.put(uint8_t{42});
        out.put(static_cast<uint8_t>(command));
        if (size) out.bytes(body, size);
        out.put(crc16(request, out.size()));
        assert(out.good() && protocol.queue(request, out.size()));
        const size_t length = protocol.reply(response, leftRPM, rightRPM);
        assert(length >= 8 && response[0] == ResponseTag && response[2] == 42 &&
               response[3] == static_cast<uint8_t>(command));
        assert(read<uint16_t>(response + length - 2) == crc16(response, length - 2));
        return static_cast<Status>(response[4]);
    };

    const uint8_t legacy[] = {1, 1, 0, 2, 0};
    assert(!protocol.queue(legacy, sizeof(legacy)) && !protocol.pending());
    const uint8_t corrupt[] = {RequestTag, 1, 42, 1, 0, 0};
    assert(protocol.queue(corrupt, sizeof(corrupt)) && !protocol.pending());
    assert(exchange(Command::Hello, nullptr, 0, 0, 0, 2) == Status::Version);
    assert(exchange(Command::Hello, nullptr, 0) == Status::Ok);
    assert(read<uint16_t>(response + 6) == 1 && read<uint16_t>(response + 8) == 1);
    const uint8_t index[] = {0, 0};
    assert(exchange(Command::Group, index, sizeof(index)) == Status::Ok);
    assert(read<uint16_t>(response + 6) == 1 && read<uint16_t>(response + 8) == 0);
    assert(exchange(Command::Describe, index, sizeof(index)) == Status::Ok);
    assert(read<uint16_t>(response + 6) == 23 && response[10] == static_cast<uint8_t>(Type::U8));
    const uint8_t good[] = {23, 0, 8}, bad[] = {23, 0, 11}, unknown[] = {24, 0, 8};
    assert(exchange(Command::Set, bad, sizeof(bad)) == Status::Invalid);
    assert(exchange(Command::Set, unknown, sizeof(unknown)) == Status::Unknown);
    crossValidationAccepted = false;
    assert(exchange(Command::Set, good, sizeof(good)) == Status::Invalid && field.get() == 4);
    crossValidationAccepted = true;
    assert(exchange(Command::Set, good, sizeof(good)) == Status::Ok && field.get() == 8);
    assert(response[5] == 2 && response[6] == 8 && protocol.revision() == 1);
    assert(exchange(Command::Read, good, 2) == Status::Ok && response[6] == 8);
    assert(exchange(Command::Save, nullptr, 0) == Status::Ok && response[5] == 0);
    const uint8_t groupId[] = {1, 0};
    assert(exchange(Command::Preview, groupId, sizeof(groupId), -20, 20) == Status::Ok);
    assert(response[6] == 7 && response[7] == 1 && read<uint16_t>(response + 8) == field.id);
    assert(response[10] == 1 && read<float>(response + 11) == 2.25f);
    const float movingCommands[][2] = {
        {1.01f, 0}, {-1.01f, 0}, {0, 1.01f}, {0, -1.01f},
        {std::numeric_limits<float>::infinity(), 0},
        {0, std::numeric_limits<float>::quiet_NaN()}
    };
    const auto saved = storage.data;
    const uint32_t revision = protocol.revision();
    for (const auto &rpm : movingCommands) {
        assert(exchange(Command::Set, good, sizeof(good), rpm[0], rpm[1]) == Status::Moving);
        assert(exchange(Command::Save, nullptr, 0, rpm[0], rpm[1]) == Status::Moving);
        assert(exchange(Command::Revert, nullptr, 0, rpm[0], rpm[1]) == Status::Moving);
        assert(exchange(Command::Defaults, groupId, sizeof(groupId), rpm[0], rpm[1]) == Status::Moving);
        assert(field.get() == 8 && protocol.revision() == revision && storage.data == saved);
        assert(!store.dirty());
    }
    assert(exchange(Command::Read, good, 2, -20, 20) == Status::Ok && response[6] == 8);
    assert(exchange(Command::Hello, nullptr, 0, 20, -20) == Status::Ok);
    assert(exchange(Command::Set, good, sizeof(good), -1, 1) == Status::Ok);
    assert(exchange(Command::Defaults, groupId, sizeof(groupId), 1, -1) == Status::Ok && field.get() == 4);
    assert(exchange(Command::Revert, nullptr, 0, -1, 1) == Status::Ok && field.get() == 8);
}

int main() {
    validation();
    persistenceAndMigration();
    protocolHandling();
    puts("Configuration validation, persistence, migration, and protocol tests passed.");
}
