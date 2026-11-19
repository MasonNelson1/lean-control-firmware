#include "Registry.h"

namespace config {

void Registry::add(Field &field) {
    // Keep the first registration for an ID. Duplicate or zero IDs are ignored.
    if (field.id == 0 || find(field.id)) return;
    if (lastField_) lastField_->next_ = &field;
    else firstField_ = &field;
    lastField_ = &field;
    ++fieldCount_;
}

void Registry::add(Group &group) {
    if (group.id == 0 || findGroup(group.id)) return;
    if (lastGroup_) lastGroup_->next_ = &group;
    else firstGroup_ = &group;
    lastGroup_ = &group;
    ++groupCount_;
}

Field *Registry::find(uint16_t id) const {
    for (auto *field = firstField_; field; field = field->next_)
        if (field->id == id) return field;
    return nullptr;
}

const Group *Registry::findGroup(uint16_t id) const {
    for (auto *group = firstGroup_; group; group = group->next_)
        if (group->id == id) return group;
    return nullptr;
}

const Field *Registry::fieldAt(uint16_t index) const {
    const Field *field = firstField_;
    while (field && index--) field = field->next();
    return field;
}

const Group *Registry::groupAt(uint16_t index) const {
    const Group *group = firstGroup_;
    while (group && index--) group = group->next();
    return group;
}

bool Group::within(const Group *ancestor) const {
    // Parent traversal also lets restoring a root group include nested fields.
    for (auto *group = this; group; group = group->parent)
        if (group == ancestor) return true;
    return false;
}

void Registry::defaults(const Group *group) {
    for (auto *field = firstField_; field; field = field->next_)
        if (!group || field->info.group.within(group)) field->reset();
}

} // namespace config
