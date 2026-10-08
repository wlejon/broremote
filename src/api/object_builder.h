#pragma once
// Building JavaScript objects and arrays from C++ under bronze's moving GC:
// the target lives in a Persistent, and every value is rooted before the
// call that stores it allocates (embed.h, "THE GC CONTRACT").

#include "embed/embed.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace broremote::api {

namespace ev = bronze::embed;
using Value = bronze::Value;

struct ObjectBuilder {
    ev::Persistent obj;

    ObjectBuilder() : obj(ev::createObject()) {}
    explicit ObjectBuilder(Value existing) : obj(existing) {}

    void set(std::string_view name, Value v) {
        ev::Persistent valP(v);
        obj.set(ev::setProperty(obj.get(), name, valP.get()));
    }
    void set(std::string_view name, double d) { set(name, ev::fromDouble(d)); }
    void set(std::string_view name, bool b) { set(name, ev::fromBool(b)); }
    void set(std::string_view name, const std::string& s) { set(name, ev::fromUtf8(s)); }
    void set(std::string_view name, const char* s) { set(name, ev::fromUtf8(s)); }

    void def(std::string_view name, uint32_t arity, ev::NativeFn fn) {
        Value f = ev::makeFunction(std::move(fn), arity, name);
        ev::Persistent fP(f);
        obj.set(ev::setProperty(obj.get(), name, fP.get()));
    }

    Value get() const { return obj.get(); }
};

struct ArrayBuilder {
    ev::Persistent arr;
    uint32_t length = 0;

    ArrayBuilder() : arr(ev::makeArray(0)) {}

    void push(Value v) {
        ev::Persistent valP(v);
        arr.set(ev::setElement(arr.get(), length++, valP.get()));
    }
    void push(const std::string& s) { push(ev::fromUtf8(s)); }

    Value get() const { return arr.get(); }
};

}  // namespace broremote::api
