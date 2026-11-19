#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace config {

/**
 * Shared schema and typed value storage for configuration fields.
 *
 * Declare groups and fields with static lifetime, then read each field through
 * get() from its owning subsystem. Constructors register the declarations so
 * protocol discovery can enumerate them without a separate list of fields.
 *
 * See docs/CONFIGURATION.md for declaration examples and wire format details.
 */

// Wire and EEPROM scalars are little-endian, using IEEE 754 for floats.
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "Configuration requires little-endian scalars");
static_assert(sizeof(float) == 4 && sizeof(double) == 8, "Unexpected floating-point format");

/**
 * Encodes into a caller-owned buffer without allocating memory.
 * Overflow makes good() false until this writer is discarded. Callers must
 * check good() before using the encoded output.
 */
class Writer {
public:
    Writer(uint8_t *data, size_t capacity) : data_(data), capacity_(capacity) {}

    void bytes(const void *value, size_t count) {
        if (count > capacity_ - size_) {
            good_ = false;
            return;
        }
        memcpy(data_ + size_, value, count);
        size_ += count;
    }

    template<typename T> void put(T value) { bytes(&value, sizeof(value)); }

    // Strings carry a one-byte UTF-8 byte count, excluding the terminating NUL.
    void text(const char *value) {
        const size_t length = strlen(value);
        if (length > UINT8_MAX) {
            good_ = false;
            return;
        }
        put(static_cast<uint8_t>(length));
        bytes(value, length);
    }

    bool good() const { return good_; }
    size_t size() const { return size_; }

private:
    uint8_t *data_;
    size_t capacity_;
    size_t size_ = 0;
    bool good_ = true;
};

// The caller must check that sizeof(T) bytes are available before calling.
// memcpy permits values at unaligned positions inside a packet or TLV record.
template<typename T> T read(const uint8_t *data) {
    T value;
    memcpy(&value, data, sizeof(value));
    return value;
}

// These numbers are shared with EEPROM records and config-protocol.js.
// Preserve existing values when adding a new type or editor.
enum class Type : uint8_t {
    F32 = 1, F64, I8, U8, I16, U16, I32, U32, I64, U64, Boolean, Enum, Profile
};

enum class Editor : uint8_t { SignedPower = 1, PiecewiseLinear = 2 };
constexpr size_t MaxValueSize = 33; // editor tag plus up to eight float parameters

class Field;
class Group;

// Optional group-level graph metadata. Field IDs bind editor roles to the
// schema, and constants supply the model's fixed firmware limits.
struct GroupPreview {
    uint8_t type;
    const uint16_t *fieldIds;
    uint8_t fieldCount;
    const float *constants;
    uint8_t constantCount;
};

/**
 * Non-owning lists of registered fields and groups.
 * Objects must outlive the registry's use and stay at their original addresses.
 * IDs must be nonzero and unique within the field list or the group list.
 */
class Registry {
public:
    void add(Field &field);
    void add(Group &group);
    const Field *fields() const { return firstField_; }
    const Group *groups() const { return firstGroup_; }
    Field *find(uint16_t id) const;
    const Group *findGroup(uint16_t id) const;
    // Discovery uses zero-based indices. Persistence and updates use stable IDs.
    const Field *fieldAt(uint16_t index) const;
    const Group *groupAt(uint16_t index) const;
    uint16_t fieldCount() const { return fieldCount_; }
    uint16_t groupCount() const { return groupCount_; }
    // Resets RAM values in this group and its descendants, or every field for null.
    void defaults(const Group *group = nullptr);

private:
    Field *firstField_ = nullptr;
    Field *lastField_ = nullptr;
    Group *firstGroup_ = nullptr;
    Group *lastGroup_ = nullptr;
    uint16_t fieldCount_ = 0;
    uint16_t groupCount_ = 0;
};

/**
 * Organizes the dashboard hierarchy. Construct parents before children.
 * A nested group inherits its root's ordinary/advanced navigation section.
 */
class Group {
public:
    Group(Registry &registry, uint16_t id, const char *name, bool advanced = false,
          const Group *parent = nullptr)
        : id(id), name(name), advanced(parent ? parent->advanced : advanced), parent(parent) {
        registry.add(*this);
    }

    const uint16_t id;
    const char *const name;
    const bool advanced;
    const Group *const parent;
    const GroupPreview *preview = nullptr; // Descriptor and arrays must outlive the group.
    const Group *next() const { return next_; }
    // Includes this group itself when checking membership.
    bool within(const Group *ancestor) const;

private:
    friend class Registry;
    Group *next_ = nullptr;
};

// Metadata is referenced directly. Groups and strings must outlive their fields.
struct FieldInfo {
    const Group &group;
    const char *name;
    const char *description;
    const char *units = "";
    bool restart = false; // UI hint. The consumer controls when the value is used.
};

/**
 * Type-erased interface used by discovery, persistence, and update validation.
 * Subclasses own the current/default values and define their binary encoding.
 * Reading or modifying a field directly affects its RAM value. Protocol and
 * persistence callers handle cross-field validation and dirty-state tracking.
 */
class Field {
public:
    Field(Registry &registry, uint16_t id, Type type, FieldInfo info)
        : id(id), type(type), info(info) { registry.add(*this); }
    virtual ~Field() = default;
    Field(const Field &) = delete;
    Field &operator=(const Field &) = delete;

    const uint16_t id; // Persistent identity. Keep it when renaming or moving a field.
    const Type type;
    const FieldInfo info;
    const Field *next() const { return next_; }
    virtual uint8_t width() const = 0; // Encoded value size, excluding any TLV header.
    virtual void value(Writer &out) const = 0;
    virtual void metadata(Writer &out) const = 0; // Defaults, bounds, choices, or editor parameters.
    virtual bool update(const uint8_t *data, size_t size) = 0; // Failure preserves the current value.
    virtual void reset() = 0;

private:
    friend class Registry;
    Field *next_ = nullptr;
};

// Derive the wire type from signedness and width. For example, unsigned long
// follows its size on the target compiler, which is four bytes on the Teensy.
template<typename T> constexpr Type scalarType() {
    if constexpr (std::is_same_v<T, bool>) return Type::Boolean;
    else if constexpr (std::is_same_v<T, float>) return Type::F32;
    else if constexpr (std::is_same_v<T, double>) return Type::F64;
    else {
        static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "Unsupported config scalar");
        if constexpr (sizeof(T) == 1) return std::is_signed_v<T> ? Type::I8 : Type::U8;
        if constexpr (sizeof(T) == 2) return std::is_signed_v<T> ? Type::I16 : Type::U16;
        if constexpr (sizeof(T) == 4) return std::is_signed_v<T> ? Type::I32 : Type::U32;
        return std::is_signed_v<T> ? Type::I64 : Type::U64;
    }
}

/**
 * A scalar field with inclusive bounds and a firmware-supplied default.
 * Floating-point writes must be finite. Boolean writes accept only 0 or 1.
 *
 * Example declaration after constructing a group:
 * ConfigField<float> gain(registry, 100, {group, "Gain", "Scales input."},
 *                         1.0f, 0.25f, 2.0f);
 */
template<typename T> class ConfigField : public Field {
public:
    ConfigField(Registry &registry, uint16_t id, FieldInfo info, T initial, T minimum, T maximum)
        : Field(registry, id, scalarType<T>(), info), current_(initial), default_(initial),
          min_(minimum), max_(maximum) {}

    ConfigField(Registry &registry, uint16_t id, FieldInfo info, bool initial)
        : ConfigField(registry, id, info, initial, false, true) {
        static_assert(std::is_same_v<T, bool>, "Provide bounds for numerical fields");
    }

    T get() const { return current_; }
    uint8_t width() const override { return sizeof(T); }
    void value(Writer &out) const override { put(out, current_); }
    void metadata(Writer &out) const override {
        put(out, default_);
        if constexpr (!std::is_same_v<T, bool>) {
            put(out, min_);
            put(out, max_);
        }
    }

    bool update(const uint8_t *data, size_t size) override {
        if (size != width()) return false;
        T candidate;
        if constexpr (std::is_same_v<T, bool>) {
            if (*data > 1) return false;
            candidate = *data != 0;
        } else {
            candidate = read<T>(data);
        }
        if constexpr (std::is_floating_point_v<T>) {
            if (!std::isfinite(candidate)) return false;
        }
        if (!(candidate >= min_ && candidate <= max_)) return false;
        current_ = candidate;
        return true;
    }

    void reset() override { current_ = default_; }

private:
    static void put(Writer &out, T value) {
        if constexpr (std::is_same_v<T, bool>) out.put(static_cast<uint8_t>(value));
        else out.put(value);
    }

    T current_;
    const T default_;
    const T min_;
    const T max_;
};

// Labels may change. Preserve each choice's byte value across firmware updates.
struct Choice { uint8_t value; const char *label; };

/**
 * An enum encoded as one byte. Only explicitly listed choices are accepted.
 * The referenced choice array must have static lifetime for firmware fields.
 */
template<typename Enum, size_t N> class EnumField : public Field {
public:
    EnumField(Registry &registry, uint16_t id, FieldInfo info, Enum initial, const Choice (&choices)[N])
        : Field(registry, id, Type::Enum, info), current_(initial), default_(initial), choices_(choices) {
        static_assert(std::is_enum_v<Enum> && N > 0 && N <= UINT8_MAX, "Invalid enum choices");
    }

    Enum get() const { return current_; }
    uint8_t width() const override { return 1; }
    void value(Writer &out) const override { out.put(static_cast<uint8_t>(current_)); }
    void metadata(Writer &out) const override {
        out.put(static_cast<uint8_t>(default_));
        out.put(static_cast<uint8_t>(N));
        for (const auto &choice : choices_) {
            out.put(choice.value);
            out.text(choice.label);
        }
    }
    bool update(const uint8_t *data, size_t size) override {
        if (size != 1) return false;
        for (const auto &choice : choices_) {
            if (choice.value == *data) {
                current_ = static_cast<Enum>(*data);
                return true;
            }
        }
        return false;
    }
    void reset() override { current_ = default_; }

private:
    Enum current_;
    const Enum default_;
    const Choice (&choices_)[N];
};

// One profile coordinate's display label and inclusive float32 bounds.
struct Parameter {
    const char *name;
    float minimum;
    float maximum;
};

/**
 * A reusable graphical profile described by N float32 coordinates.
 * SignedPower stores [knee input, knee output]. PiecewiseLinear stores
 * [x0, y0, x1, y1, ...] with strictly increasing input coordinates.
 *
 * The initial-value and parameter arrays are referenced and must outlive the
 * field. The value includes its editor tag so a changed profile interpretation
 * is detected even when the coordinate count stays the same.
 */
template<size_t N> class ProfileField : public Field {
public:
    ProfileField(Registry &registry, uint16_t id, FieldInfo info, Editor editor,
                 const float (&initial)[N], const Parameter (&parameters)[N])
        : Field(registry, id, Type::Profile, info), editor_(editor), defaults_(initial), parameters_(parameters) {
        static_assert(N >= 2 && N % 2 == 0 && 1 + N * sizeof(float) <= MaxValueSize, "Invalid profile size");
        reset();
    }

    const float (&get() const)[N] { return current_; }
    uint8_t width() const override { return 1 + N * sizeof(float); }
    void value(Writer &out) const override {
        out.put(static_cast<uint8_t>(editor_));
        out.bytes(current_, sizeof(current_));
    }
    void metadata(Writer &out) const override {
        out.put(static_cast<uint8_t>(editor_));
        out.put(static_cast<uint8_t>(N));
        for (size_t i = 0; i < N; ++i) {
            out.text(parameters_[i].name);
            out.put(defaults_[i]);
            out.put(parameters_[i].minimum);
            out.put(parameters_[i].maximum);
        }
    }
    bool update(const uint8_t *data, size_t size) override {
        if (size != width() || *data != static_cast<uint8_t>(editor_)) return false;
        float candidate[N];
        memcpy(candidate, data + 1, sizeof(candidate));
        for (size_t i = 0; i < N; ++i) {
            if (!(candidate[i] >= parameters_[i].minimum && candidate[i] <= parameters_[i].maximum))
                return false;
        }
        // Validate the relationship between coordinates after individual bounds.
        if (editor_ == Editor::SignedPower) {
            if (N != 2 || candidate[0] <= 0 || candidate[0] >= 1 ||
                candidate[1] <= 0 || candidate[1] >= 1) return false;
        } else if (editor_ == Editor::PiecewiseLinear) {
            for (size_t i = 2; i < N; i += 2)
                if (candidate[i] <= candidate[i - 2]) return false;
        } else {
            return false;
        }
        memcpy(current_, candidate, sizeof(current_));
        return true;
    }
    void reset() override { memcpy(current_, defaults_, sizeof(current_)); }

private:
    const Editor editor_;
    const float (&defaults_)[N];
    const Parameter (&parameters_)[N];
    float current_[N];
};

} // namespace config
