#pragma once

#include "Persistence.h"

namespace config {

// Packet tags distinguish configuration from the fixed remote-drive identifiers.
constexpr uint8_t RequestTag = 0xC1;
constexpr uint8_t ResponseTag = 0xC2;
constexpr uint8_t ProtocolVersion = 1;
constexpr size_t MaxRequestSize = 48;
constexpr size_t MaxResponseSize = 400;

// Preserve numeric values because config-protocol.js uses the same identifiers.
enum class Command : uint8_t { Hello = 1, Group, Describe, Read, Set, Save, Revert, Defaults, Preview, ProfileList, ProfileRead, ProfileBegin, ProfileChunk, ProfileCommit, ProfileMove };
enum class Status : uint8_t { Ok, Version, Malformed, Unknown, Invalid, NotDisarmed, Storage, TooLarge };

// CRC-16/CCITT-FALSE. Covers the decoded packet except its final two CRC bytes.
uint16_t crc16(const uint8_t *data, size_t size);

/**
 * One-request configuration server, independent of the serial transport.
 *
 * PacketSerial passes decoded bytes to queue(). The main loop later calls
 * reply(), applies any derived runtime settings when revision() changes, and
 * sends the response using PacketSerial. See docs/CONFIGURATION.md for layouts.
 */
class Protocol {
public:
    // Optional side-effect-free validator for relationships across fields.
    // A failed validation rolls back a Set or Restore Defaults operation.
    Protocol(Registry &registry, Persistence &persistence, bool (*validate)())
        : registry_(registry), persistence_(persistence), validate_(validate) {}

    /**
     * Returns true for every packet carrying RequestTag, including damaged or
     * dropped configuration requests, so it cannot reach the drive handler.
     * Copies one complete CRC-valid request. Later requests are dropped while
     * that slot is occupied. The browser therefore waits for each reply.
     */
    bool queue(const uint8_t *packet, size_t size);
    void tick(uint32_t now, bool disarmed) {
        if (!disarmed || now - uploadActivity_ >= 10000) uploadLength_ = 0;
        now_ = now;
    }
    bool selectProfile(uint8_t slot);
    bool pending() const { return pendingSize_ != 0; }

    /**
     * Executes the queued request and returns the decoded reply length, or zero
     * when idle. The caller supplies whether the current mode is DISARM.
     * Set, Save, Revert, and Defaults return NotDisarmed outside DISARM.
     * Reads and discovery are available in every mode.
     */
    size_t reply(uint8_t (&response)[MaxResponseSize], bool disarmed);

    // Changes after successful Set, Revert, or Defaults, allowing the caller to
    // refresh motor IDs, response exponents, and controller tuning once.
    uint32_t revision() const { return revision_; }

private:
    Status execute(Command command, const uint8_t *body, size_t size,
                   bool disarmed, Writer &out);
    Status update(Field &field, const uint8_t *data, size_t size, Writer &out);
    Status defaults(const Group *group);
    void changed();
    Status profiles(Command command, const uint8_t *body, size_t size, Writer &out);

    uint8_t upload_[Persistence::ProfileCapacity];
    uint8_t uploadName_[Persistence::ProfileNameSize];
    uint16_t uploadLength_ = 0, uploadReceived_ = 0;
    uint8_t uploadSlot_ = 0;
    uint32_t now_ = 0, uploadActivity_ = 0;

    Registry &registry_;
    Persistence &persistence_;
    bool (*validate_)();
    uint8_t pending_[MaxRequestSize];
    size_t pendingSize_ = 0;
    uint32_t revision_ = 0;
};

} // namespace config
