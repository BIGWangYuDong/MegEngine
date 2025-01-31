#include "megbrain/imperative/value.h"

#include "megbrain/imperative/basic_operators.h"
#include "megbrain/imperative/dispatch.h"
#include "megbrain/imperative/utils/map.h"

namespace mgb {
namespace imperative {

namespace {
static thread_local size_t nr_watched_values = 0;
static thread_local uint64_t nr_values = 0;
static thread_local bool recording_values = false;
static thread_local std::vector<ValueWeakRef> recorded_values;
static WeakValueMap<uint64_t, ValueWeakRef> registered_values;
}  // namespace

ValueRef::storage_t& ValueRef::storage() const {
    if (!m_storage) {
        return m_storage;
    }
    if (auto& storage = m_storage->m_successor.m_storage) {
        while (storage->m_successor.m_storage) {
            storage = storage->m_successor.m_storage;
        }
        return storage;
    } else {
        return m_storage;
    }
}

TypedValueRef<DeviceValue> ValueRef::dev_tensor() const {
    return imperative::apply(GetAttr(GetAttr::Data), *this)[0].as_ref<DeviceValue>();
}

TypedValueRef<HostValue> ValueRef::numpy() const {
    return imperative::apply(GetAttr(GetAttr::Value), *this)[0].as_ref<HostValue>();
}

TypedValueRef<CompNodeValue> ValueRef::device() const {
    return imperative::apply(GetAttr(GetAttr::Device), *this)[0]
            .as_ref<CompNodeValue>();
}

TypedValueRef<ShapeValue> ValueRef::shape() const {
    return imperative::apply(GetAttr(GetAttr::Shape), *this)[0].as_ref<ShapeValue>();
}

TypedValueRef<DTypeValue> ValueRef::dtype() const {
    return imperative::apply(GetAttr(GetAttr::DType), *this)[0].as_ref<DTypeValue>();
}

TypedValueRef<StringValue> ValueRef::name() const {
    return imperative::apply(GetName(), *this)[0].as_ref<StringValue>();
}

bool ValueRef::is_scalar() const {
    return imperative::apply(IsScalar(), *this)[0].cast<BoolValue>();
}

void ValueRef::watch() const {
    mgb_assert(m_storage);
    storage()->m_watching++;
    nr_watched_values++;
    storage()->on_watch();
    // TODO:
    // imperative::apply(Watch(), this);
}

void ValueRef::unwatch() const {
    mgb_assert(m_storage);
    storage()->m_watching--;
    nr_watched_values--;
    storage()->on_unwatch();
}

ValueRef ValueRef::unwrap() const {
    ValueRef value = *this;
    auto& context = Transformation::get_context();
    for (size_t i = 0; i < context.next_transformation; ++i) {
        value = context.transformations[i]->unwrap(value);
    }
    mgb_assert(value);
    return value;
}

std::string ValueRef::to_string() const {
    if (!m_storage) {
        return "<empty value>";
    }
    return ssprintf(
            "(%zu:%zu) %s", id(), storage()->m_id, storage()->to_string().c_str());
}

std::string ValueRef::raw_type() const {
    if (!m_storage) {
        return "null";
    }
    auto& types = Value::registered_types();
    mgb_assert(types.size() > m_storage->m_typecode);
    return types[m_storage->m_typecode].name();
}

uint64_t ValueRef::id() const {
    return m_storage ? m_storage->m_id : std::numeric_limits<uint64_t>::max();
}

bool ValueRef::watching() const {
    auto storage = this->storage();
    return storage && storage->m_watching;
}

ValueRef ValueRef::make(ValueRef::storage_t storage) {
    if (recording_values) {
        recorded_values.push_back({storage});
    }
    return {storage};
}

bool ValueRef::any_watching() {
    return nr_watched_values != 0;
}

ValueRef ValueWeakRef::lock() {
    auto strong_storage = m_storage.lock();
    if ((!strong_storage) || strong_storage->m_successor) {
        return {};
    }
    return {strong_storage};
}

Value::Value(size_t typecode) : m_typecode{typecode} {
    m_id = nr_values++;
}

Value::~Value() {
    if (m_watching) {
        debug::notify_event("dtor");
    }
}

size_t Value::register_type(std::type_index type) {
    auto& types = const_cast<std::vector<std::type_index>&>(registered_types());
    types.push_back(type);
    return types.size() - 1;
}

const std::vector<std::type_index>& Value::registered_types() {
    static std::vector<std::type_index> sm_registered_types;
    return sm_registered_types;
}

void Value::register_value(ValueRef value) {
    registered_values[value.id()] = ValueWeakRef(value);
}

ValueRef Value::get_value_by_id(uint64_t id) {
    auto& weak_value = registered_values[id];
    if (auto value = weak_value.lock()) {
        return value;
    }
    return {};
}

void Value::begin_record_values() {
    mgb_assert(!recording_values);
    recording_values = true;
    recorded_values.clear();
}

std::vector<ValueRef> Value::end_record_values() {
    recording_values = false;
    std::vector<ValueRef> recorded_strong_values;
    for (auto&& weak_value : recorded_values) {
        if (auto value = weak_value.lock()) {
            recorded_strong_values.push_back(value);
        }
    }
    return recorded_strong_values;
}

void Value::try_rethrow() {
    if (m_typecode == ErrorValue::TYPE_CODE) {
        auto message = static_cast<ErrorValue*>(this)->message();
        mgb_throw(MegBrainError, "invalid value: %s", message.c_str());
    }
}

}  // namespace imperative
}  // namespace mgb
