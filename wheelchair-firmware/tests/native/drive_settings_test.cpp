// g++ -std=c++17 -Wall -Wextra -Werror -I tests/native/config_stubs -I src tests/native/drive_settings_test.cpp src/config/DriveSettings.cpp src/config/Registry.cpp src/config/Persistence.cpp src/config/Protocol.cpp -o .pio/drive_settings_test.exe
#include "config/DriveSettings.h"
#include <EEPROM.h>
#include <cmath>
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
            &settings::rangeForward, &settings::rangeBack, &settings::rangeLeft, &settings::rangeRight,
            &settings::leanForewardBackResponse, &settings::leanSideToSideResponse, &settings::assistResponse,
            &settings::maxTurnRate, &settings::maxLateralAccel,
            &settings::assistMaxBoostAmps, &settings::assistSlowdownRPM, &settings::assistSlowdownBoostFraction,
            &settings::controllerTimeout, &settings::buttonDebounce, &settings::controlPeriod,
            &settings::zeroFront, &settings::zeroBack, &settings::zeroLeft, &settings::zeroRight, &settings::seatThreshold,
            &settings::leftVesc, &settings::rightVesc, &settings::wheelCircumference, &settings::trackWidth,
            &settings::vescMinSpeed,
            &settings::normalPid.lowTau, &settings::normalPid.highTau, &settings::normalPid.ki,
            &settings::normalPid.kd, &settings::normalPid.filter, &settings::normalPid.maxJerk,
            &settings::transitionPid.lowTau, &settings::transitionPid.highTau, &settings::transitionPid.ki,
            &settings::transitionPid.kd, &settings::transitionPid.filter, &settings::transitionPid.maxJerk
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
        speedOut.put(uint8_t{1}); speedOut.put(.25f); speedOut.put(.15f);
        CHECK(request(Command::Set, speed.id, speedRaw, sizeof(speedRaw)) == Status::Ok);
        CHECK(speed.get()[0] == .25f && speed.get()[1] == .15f);
        CHECK(turn.get()[0] == originalTurnInput && turn.get()[1] == originalTurnOutput);
        uint8_t turnRaw[9]; Writer turnOut(turnRaw, sizeof(turnRaw));
        turnOut.put(uint8_t{1}); turnOut.put(.4f); turnOut.put(.2f);
        CHECK(request(Command::Set, turn.id, turnRaw, sizeof(turnRaw)) == Status::Ok);
        CHECK(speed.get()[0] == .25f && speed.get()[1] == .15f);
        CHECK(turn.get()[0] == .4f && turn.get()[1] == .2f);
        CHECK(settings::persistence.save());
        speed.reset(); turn.reset();
        settings::persistence.load();
        CHECK(speed.get()[0] == .25f && speed.get()[1] == .15f);
        CHECK(turn.get()[0] == .4f && turn.get()[1] == .2f);
        CHECK(!settings::persistence.dirty());
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
    test("retired field and group IDs stay unused", [] {
        for (uint16_t id : {10, 11, 12, 13, 20, 21, 22, 23, 36, 50, 51}) CHECK(!settings::registry().find(id));
        CHECK(!settings::registry().findGroup(3) && !settings::registry().findGroup(4));
    });
    test("calibration fields are exactly the chair and rider measurements", [] {
        const std::set<const Field *> expected = {
            &settings::rangeForward, &settings::rangeBack, &settings::rangeLeft, &settings::rangeRight,
            &settings::zeroFront, &settings::zeroBack, &settings::zeroLeft, &settings::zeroRight,
            &settings::seatThreshold, &settings::trackWidth,
            &settings::leftVesc, &settings::rightVesc, &settings::wheelCircumference, &settings::vescMinSpeed};
        for (auto *f = settings::registry().fields(); f; f = f->next())
            CHECK(f->info.calibration == (expected.count(f) == 1));
    });
    test("typed set() enforces the same bounds as a dashboard write", [] {
        CHECK(!settings::rangeForward.set(settings::LeanRangeMin - 1));
        CHECK(!settings::rangeForward.set(settings::LeanRangeMax + 1));
        CHECK(settings::rangeForward.set(settings::LeanRangeMin) && settings::rangeForward.get() == settings::LeanRangeMin);
        CHECK(!settings::maxTurnRate.set(NAN) && !settings::maxTurnRate.set(1000.0f));
        settings::rangeForward.reset();
    });
    test("a calibration save keeps other unsaved edits unsaved", [] {
        settings::registry().defaults();
        settings::persistence.refreshDirty();
        CHECK(settings::persistence.save());
        // An unsaved dashboard edit, then a calibration.
        const float edited = .2f;
        CHECK(request(Command::Set, settings::deadband.id, reinterpret_cast<const uint8_t *>(&edited), sizeof(edited)) == Status::Ok);
        CHECK(settings::persistence.dirty());
        CHECK(settings::rangeBack.set(321) && settings::zeroBack.set(437));
        CHECK(settings::persistence.saveCalibration());
        // RAM keeps everything; the edit is still reported as unsaved.
        CHECK(settings::deadband.get() == edited && settings::rangeBack.get() == 321);
        CHECK(settings::persistence.dirty());
        // After a power cycle: the calibration is there, the unsaved edit is not.
        settings::persistence.load();
        CHECK(settings::rangeBack.get() == 321 && settings::zeroBack.get() == 437);
        CHECK(settings::deadband.get() == .05f);
        CHECK(!settings::persistence.dirty());
        // With nothing else pending, the bank and RAM agree afterwards.
        CHECK(settings::rangeLeft.set(222));
        CHECK(settings::persistence.saveCalibration() && !settings::persistence.dirty());
    });
    test("a calibration save works on a blank EEPROM and keeps saved values of other fields", [] {
        for (auto &byte : EEPROM.bytes) byte = 0xff;
        settings::persistence.load();
        CHECK(settings::rangeLeft.get() == 500);
        CHECK(settings::rangeRight.set(444));
        CHECK(settings::persistence.saveCalibration());
        settings::persistence.load();
        CHECK(settings::rangeRight.get() == 444 && !settings::persistence.dirty());
        // A saved non-calibration value survives a later calibration save.
        const float edited = .3f;
        CHECK(request(Command::Set, settings::deadband.id, reinterpret_cast<const uint8_t *>(&edited), sizeof(edited)) == Status::Ok);
        CHECK(settings::persistence.save());
        CHECK(settings::rangeRight.set(555) && settings::persistence.saveCalibration());
        settings::persistence.load();
        CHECK(settings::deadband.get() == edited && settings::rangeRight.get() == 555);
    });
    test("a failed EEPROM write is reported and leaves RAM intact", [] {
        CHECK(settings::rangeRight.set(600));
        EEPROM.failWrites = true;
        const bool saved = settings::persistence.saveCalibration();
        EEPROM.failWrites = false;
        CHECK(!saved && settings::rangeRight.get() == 600);
        CHECK(settings::persistence.saveCalibration());
    });
    test("loading a profile leaves calibration fields alone", [] {
        settings::registry().defaults();
        // The profile was saved with one rider's ranges and a different feel.
        CHECK(settings::rangeForward.set(900) && settings::zeroBack.set(500) && settings::seatThreshold.set(300));
        const float feel = .25f, rate = 90.0f;
        CHECK(settings::deadband.set(feel) && settings::maxTurnRate.set(rate));
        uint8_t payload[Persistence::ProfileCapacity], name[Persistence::ProfileNameSize] = "feel";
        const size_t length = snapshot(settings::registry(), payload, sizeof(payload));
        CHECK(length && settings::persistence.writeProfile(2, name, payload, length));
        // Now the chair has been tared and calibrated for someone else.
        settings::registry().defaults();
        CHECK(settings::rangeForward.set(450) && settings::zeroBack.set(437));
        CHECK(settings::protocol.selectProfile(2));
        CHECK(settings::deadband.get() == feel && settings::maxTurnRate.get() == rate); // the feel loads
        CHECK(settings::rangeForward.get() == 450 && settings::zeroBack.get() == 437);  // the calibration stays
        CHECK(settings::seatThreshold.get() == 200 && settings::trackWidth.get() == 0.60f);
        settings::registry().defaults();
    });
    test("a profile cannot swap the VESC IDs or change the wheel size", [] {
        settings::registry().defaults();
        // A profile saved while the IDs were the other way round.
        const uint8_t temporary = 99, left = 26, right = 2;
        const float circumference = 1.85f;
        CHECK(settings::leftVesc.set(temporary) && settings::rightVesc.set(right) && settings::leftVesc.set(left));
        CHECK(settings::wheelCircumference.set(circumference));
        uint8_t payload[Persistence::ProfileCapacity], name[Persistence::ProfileNameSize] = "swapped";
        const size_t length = snapshot(settings::registry(), payload, sizeof(payload));
        CHECK(length && settings::persistence.writeProfile(3, name, payload, length));
        settings::registry().defaults();
        CHECK(settings::protocol.selectProfile(3));
        CHECK(settings::leftVesc.get() == 2 && settings::rightVesc.get() == 26); // steering not mirrored
        CHECK(settings::wheelCircumference.get() == common::WHEEL_CIRCUMFERENCE_M);
    });
    test("lean response curves stay between a straight line and a strong curve", [] {
        settings::registry().defaults();
        const auto set = [](config::ProfileField<2> &field, float input, float output) {
            uint8_t raw[9]; Writer out(raw, sizeof(raw));
            out.put(uint8_t{1}); out.put(input); out.put(output);
            return request(Command::Set, field.id, raw, sizeof(raw));
        };
        for (auto *field : {&settings::leanForewardBackResponse, &settings::leanSideToSideResponse}) {
            const float input = field->get()[0], output = field->get()[1];
            // Knee above the straight line: steepest at neutral. Far above it, any lean is full speed.
            CHECK(set(*field, .5f, .999f) == Status::Invalid);
            CHECK(set(*field, .5f, .51f) == Status::Invalid);
            // So flat that nothing happens until the very end of the lean.
            CHECK(set(*field, .9f, .001f) == Status::Invalid);
            CHECK(field->get()[0] == input && field->get()[1] == output); // rolled back
            CHECK(set(*field, .5f, .5f) == Status::Ok);    // a straight line
            CHECK(set(*field, .5f, .0625f) == Status::Ok); // exponent 4
        }
        // The assist curve is a different thing and keeps its own freedom.
        CHECK(set(settings::assistResponse, .44f, .61f) == Status::Ok);
        // A saved curve from before this rule is put back to the default on load.
        const float steep[] = {.5f, .9f};
        uint8_t raw[9]; Writer out(raw, sizeof(raw));
        out.put(uint8_t{1}); out.bytes(steep, sizeof(steep));
        CHECK(settings::leanSideToSideResponse.update(raw, sizeof(raw)));
        settings::persistence.refreshDirty();
        CHECK(settings::persistence.save());
        settings::persistence.load();
        CHECK(settings::leanSideToSideResponse.get()[0] == .35f && settings::leanSideToSideResponse.get()[1] == .20f);
        settings::registry().defaults();
        settings::persistence.refreshDirty();
        CHECK(settings::persistence.save());
    });
    test("every EEPROM byte written reports activity for the watchdog", [] {
        static int calls;
        calls = 0;
        settings::storageActivity = [] { ++calls; };
        CHECK(settings::rangeForward.set(700) && settings::persistence.saveCalibration());
        settings::storageActivity = nullptr;
        CHECK(calls > 100);
        settings::registry().defaults();
        settings::persistence.refreshDirty();
    });
    test("bounds keep settings inside what the firmware can use", [] {
        const float tooWide = 0.6f;
        CHECK(request(Command::Set, settings::deadband.id, reinterpret_cast<const uint8_t *>(&tooWide), sizeof(tooWide)) == Status::Invalid);
        const unsigned long slow = 201, debounce = 101;
        CHECK(request(Command::Set, settings::controllerTimeout.id, reinterpret_cast<const uint8_t *>(&slow), sizeof(slow)) == Status::Invalid);
        CHECK(request(Command::Set, settings::buttonDebounce.id, reinterpret_cast<const uint8_t *>(&debounce), sizeof(debounce)) == Status::Invalid);
        const float tipping = 6.5f;
        CHECK(request(Command::Set, settings::maxLateralAccel.id, reinterpret_cast<const uint8_t *>(&tipping), sizeof(tipping)) == Status::Invalid);
        CHECK(settings::maxLateralAccel.get() == 2.5f);
    });
    std::cout << "TOTAL " << passed + failed << " / PASS " << passed << " / FAIL " << failed << '\n';
    return failed ? 1 : 0;
}
