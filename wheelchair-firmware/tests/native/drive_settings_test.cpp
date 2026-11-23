// g++ -std=c++17 -Wall -Wextra -Werror -I tests/native/config_stubs -I src tests/native/drive_settings_test.cpp src/config/DriveSettings.cpp src/config/Registry.cpp src/config/Persistence.cpp src/config/Protocol.cpp -o .pio/drive_settings_test.exe
#include "config/DriveSettings.h"
#include <cstring>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace config;
static int passed = 0, failed = 0;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)
static void test(const std::string &name, const std::function<void()> &body) {
    try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
    catch (const std::exception &error) { ++failed; std::cout << "FAIL " << name << " -- " << error.what() << '\n'; }
}
static Status request(Command command, uint16_t id, const uint8_t *value = nullptr, size_t size = 0) {
    uint8_t data[MaxRequestSize], response[MaxResponseSize];
    Writer out(data, sizeof(data));
    out.put(RequestTag); out.put(ProtocolVersion); out.put(uint8_t{1}); out.put(static_cast<uint8_t>(command));
    out.put(id);
    if (size) out.bytes(value, size);
    out.put(crc16(data, out.size()));
    CHECK(settings::protocol.queue(data, out.size()));
    CHECK(settings::protocol.reply(response, true) >= 8);
    return static_cast<Status>(response[4]);
}
int main() {
    settings::persistence.load();
    for (const auto *field = settings::registry().fields(); field; field = field->next()) {
        const std::string name = std::to_string(field->id) + " " + field->info.name;
        test("actual declaration round trip / " + name, [&] {
            uint8_t raw[MaxValueSize]; Writer out(raw, sizeof(raw)); field->value(out);
            CHECK(out.good() && out.size() == field->width());
            CHECK(settings::registry().find(field->id)->update(raw, out.size()));
        });
        test("actual declaration fits discovery packet / " + name, [&] {
            uint16_t index = 0;
            while (settings::registry().fieldAt(index) != field) ++index;
            CHECK(request(Command::Describe, index) == Status::Ok);
            CHECK(field->info.name[0] && field->info.description[0]);
        });
    }
    for (const auto *group = settings::registry().groups(); group; group = group->next()) {
        test("actual group resolves parent / " + std::string(group->name), [&] {
            CHECK(group->id && group->name[0]);
            if (group->parent) CHECK(settings::registry().findGroup(group->parent->id) == group->parent);
        });
    }
    test("actual field IDs are unique and nonzero", [] {
        std::set<uint16_t> ids;
        for (auto *f = settings::registry().fields(); f; f = f->next()) CHECK(f->id && ids.insert(f->id).second);
    });
    test("every exported declaration is discoverable under its own ID", [] {
        const std::vector<const Field *> exported = {
            &settings::defaultMode, &settings::deadband,
            &settings::frontGain, &settings::rearGain, &settings::leftGain, &settings::rightGain,
            &settings::leanForewardBackResponse, &settings::leanSideToSideResponse, &settings::assistResponse, &settings::leanTurnMix,
            &settings::assistMaxBoostAmps, &settings::assistSlowdownRPM, &settings::assistSlowdownBoostFraction,
            &settings::controllerTimeout, &settings::buttonDebounce,
            &settings::controlPeriod, &settings::calibrationSamples, &settings::defaultFloor,
            &settings::leftVesc, &settings::rightVesc, &settings::wheelCircumference,
            &settings::normalPid.lowTau, &settings::normalPid.highTau, &settings::normalPid.ki,
            &settings::normalPid.kd, &settings::normalPid.filter, &settings::normalPid.maxJerk,
            &settings::transitionPid.lowTau, &settings::transitionPid.highTau, &settings::transitionPid.ki,
            &settings::transitionPid.kd, &settings::transitionPid.filter, &settings::transitionPid.maxJerk,
            &settings::sensorLayout, &settings::cornerDeadzone,
            &settings::reachForward, &settings::reachBack, &settings::reachLeft, &settings::reachRight, &settings::passiveMargin,
            &settings::emptyPlateF, &settings::emptyPlateB, &settings::emptyPlateL, &settings::emptyPlateR,
            &settings::cornerGainF, &settings::cornerGainB, &settings::cornerGainL, &settings::cornerGainR,
            &settings::benchTest
        };
        CHECK(exported.size() == settings::registry().fieldCount());
        for (const auto *field : exported) CHECK(settings::registry().find(field->id) == field);
    });
    test("actual live CAN ID conflict is rejected and rolled back", [] {
        const uint8_t previous = settings::leftVesc.get(), collision = settings::rightVesc.get();
        CHECK(request(Command::Set, settings::leftVesc.id, &collision, 1) == Status::Invalid);
        CHECK(settings::leftVesc.get() == previous);
    });
    test("actual PID preview bindings reference registered fields", [] {
        size_t count = 0;
        for (const auto *g = settings::registry().groups(); g; g = g->next()) {
            if (!g->preview) continue;
            ++count;
            CHECK(g->preview->type == 1 && g->preview->fieldCount == 8 && g->preview->constantCount == 2);
            for (uint8_t i = 0; i < g->preview->fieldCount; ++i) CHECK(settings::registry().find(g->preview->fieldIds[i]));
        }
        CHECK(count == 2);
    });
    test("actual runtime edits mark persistence dirty", [] {
        const float value = .10f;
        CHECK(request(Command::Set, settings::deadband.id, reinterpret_cast<const uint8_t *>(&value), sizeof(value)) == Status::Ok);
        CHECK(settings::persistence.dirty());
        settings::persistence.load();
    });
    test("both response curves accept independent edits and persist", [] {
        auto &speed = settings::leanForewardBackResponse;
        auto &turn = settings::leanSideToSideResponse;
        CHECK(speed.id != turn.id);
        const float originalTurnInput = turn.get()[0], originalTurnOutput = turn.get()[1];
        uint8_t speedRaw[9]; Writer speedOut(speedRaw, sizeof(speedRaw));
        speedOut.put(uint8_t{1}); speedOut.put(.25f); speedOut.put(.75f);
        CHECK(request(Command::Set, speed.id, speedRaw, sizeof(speedRaw)) == Status::Ok);
        CHECK(speed.get()[0] == .25f && speed.get()[1] == .75f);
        CHECK(turn.get()[0] == originalTurnInput && turn.get()[1] == originalTurnOutput);
        uint8_t turnRaw[9]; Writer turnOut(turnRaw, sizeof(turnRaw));
        turnOut.put(uint8_t{1}); turnOut.put(.4f); turnOut.put(.2f);
        CHECK(request(Command::Set, turn.id, turnRaw, sizeof(turnRaw)) == Status::Ok);
        CHECK(speed.get()[0] == .25f && speed.get()[1] == .75f);
        CHECK(turn.get()[0] == .4f && turn.get()[1] == .2f);
        CHECK(settings::persistence.save());
        speed.reset(); turn.reset();
        settings::persistence.load();
        CHECK(speed.get()[0] == .25f && speed.get()[1] == .75f);
        CHECK(turn.get()[0] == .4f && turn.get()[1] == .2f);
        CHECK(!settings::persistence.dirty());
    });
    test("every setting fits one saved profile", [] {
        // Saving a profile snapshots every field into ProfileCapacity bytes. The
        // host's 8-byte unsigned long makes this a few bytes stricter than the Teensy.
        uint8_t data[Persistence::PayloadCapacity];
        const size_t used = snapshot(settings::registry(), data, sizeof(data));
        CHECK(used != 0 && used <= Persistence::ProfileCapacity);
    });
    test("sensor layout accepts only PLUS and CORNER and defaults to CORNER", [] {
        CHECK(settings::sensorLayout.get() == lean::SensorLayout::Corner);
        for (uint8_t value : {uint8_t{0}, uint8_t{1}}) {
            CHECK(request(Command::Set, settings::sensorLayout.id, &value, 1) == Status::Ok);
            CHECK(static_cast<uint8_t>(settings::sensorLayout.get()) == value);
        }
        const uint8_t bad = 2;
        CHECK(request(Command::Set, settings::sensorLayout.id, &bad, 1) != Status::Ok);
        CHECK(settings::sensorLayout.get() == lean::SensorLayout::Corner);
    });
    test("corner reach can never sit inside the deadzone", [] {
        // axisLean needs reach > deadzone in every direction; the bounds guarantee it.
        const float tooShort = lean::MinReach - 0.01f, tooWide = lean::MaxDeadzone + 0.01f;
        CHECK(lean::MinReach > lean::MaxDeadzone);
        CHECK(request(Command::Set, settings::reachBack.id, reinterpret_cast<const uint8_t *>(&tooShort), 4) != Status::Ok);
        CHECK(request(Command::Set, settings::cornerDeadzone.id, reinterpret_cast<const uint8_t *>(&tooWide), 4) != Status::Ok);
        CHECK(settings::reachBack.get() == lean::DefaultReachBack);
        CHECK(settings::cornerDeadzone.get() == lean::DefaultDeadzone);
    });
    test("a profile saved before the corner layout selects corner defaults, not the last profile's", [] {
        // Profile from older firmware: only the startup mode, no layout or corner fields.
        const uint8_t older[] = {1, 0, uint8_t(Type::Enum), 1, uint8_t(common::DISARM)};
        uint8_t name[Persistence::ProfileNameSize] = "Before corners";
        CHECK(settings::persistence.writeProfile(6, name, older, sizeof(older)));
        const uint8_t plus = 0;
        const float reach = 3.0f;
        CHECK(settings::sensorLayout.update(&plus, 1));
        CHECK(settings::reachForward.update(reinterpret_cast<const uint8_t *>(&reach), 4));
        CHECK(settings::protocol.selectProfile(6));
        CHECK(settings::sensorLayout.get() == lean::SensorLayout::Corner);
        CHECK(settings::reachForward.get() == lean::DefaultReachForward);
        settings::registry().defaults();
    });
    test("bench test trigger is RAM only: never saved, never dirty, OFF after load", [] {
        CHECK(!settings::benchTest.info.persistent);
        CHECK(settings::benchTest.get() == bench::Request::Off);
        settings::persistence.load();
        uint8_t before[Persistence::PayloadCapacity], after[Persistence::PayloadCapacity];
        const size_t length = snapshot(settings::registry(), before, sizeof(before));
        for (size_t offset = 0; offset < length; offset += 4 + before[offset + 3])
            CHECK(read<uint16_t>(before + offset) != settings::benchTest.id);
        const uint8_t run = 1;
        CHECK(request(Command::Set, settings::benchTest.id, &run, 1) == Status::Ok);
        CHECK(settings::benchTest.get() == bench::Request::Run);
        CHECK(!settings::persistence.dirty()); // nothing for the dashboard to save
        CHECK(snapshot(settings::registry(), after, sizeof(after)) == length && !memcmp(before, after, length));
        CHECK(settings::persistence.save());
        settings::persistence.load(); // = power-on
        CHECK(settings::benchTest.get() == bench::Request::Off);
        const uint8_t bad = 2;
        CHECK(request(Command::Set, settings::benchTest.id, &bad, 1) == Status::Invalid);
    });
    test("bench test trigger stays out of saved profiles and profile selection clears it", [] {
        uint8_t payload[Persistence::ProfileCapacity], name[Persistence::ProfileNameSize] = "Bench";
        const uint8_t run = 1;
        CHECK(request(Command::Set, settings::benchTest.id, &run, 1) == Status::Ok);
        const size_t length = snapshot(settings::registry(), payload, sizeof(payload));
        CHECK(settings::persistence.writeProfile(5, name, payload, length));
        uint8_t readName[Persistence::ProfileNameSize], read[Persistence::ProfileCapacity];
        const size_t stored = settings::persistence.readProfile(5, readName, read);
        CHECK(stored == length);
        for (size_t offset = 0; offset < stored; offset += 4 + read[offset + 3])
            CHECK(config::read<uint16_t>(read + offset) != settings::benchTest.id);
        CHECK(settings::protocol.selectProfile(5));
        CHECK(settings::benchTest.get() == bench::Request::Off);
    });
    test("a dashboard profile upload (every field, bench trigger included) fits on the Teensy", [] {
        // The dashboard uploads every discovered field; the firmware drops the
        // bench record. The host's unsigned long is 8 bytes, the Teensy's 4.
        uint8_t data[Persistence::PayloadCapacity];
        size_t teensy = snapshot(settings::registry(), data, sizeof(data)) + 4 + settings::benchTest.width();
        for (auto *f = settings::registry().fields(); f; f = f->next())
            if (f->type == scalarType<unsigned long>() && sizeof(unsigned long) == 8) teensy -= 4;
        CHECK(teensy <= Persistence::ProfileCapacity);
    });
    test("actual settings fit EEPROM and survive reload", [] {
        CHECK(settings::persistence.save());
        settings::persistence.load();
        CHECK(!settings::persistence.dirty());
    });
    test("loading conflicting saved CAN IDs repairs only the conflicting pair", [] {
        const uint8_t collision = settings::rightVesc.get();
        const float deadband = .2f;
        CHECK(settings::leftVesc.update(&collision, sizeof(collision)));
        CHECK(settings::deadband.update(reinterpret_cast<const uint8_t *>(&deadband), sizeof(deadband)));
        settings::persistence.refreshDirty();
        CHECK(settings::persistence.save());
        settings::persistence.load();
        CHECK(settings::leftVesc.get() != settings::rightVesc.get());
        CHECK(settings::deadband.get() == deadband);
    });
    std::cout << "TOTAL " << passed + failed << " / PASS " << passed << " / FAIL " << failed << '\n';
    return failed ? 1 : 0;
}
