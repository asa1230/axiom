#include "OwnedObject.h"

#include <cassert>

using namespace MaximCompiler;

OwnedObject::OwnedObject(void *handle, void (*destroy)(void *)) : handle(handle), destroy(destroy) {}

OwnedObject::~OwnedObject() {
    if (handle) destroy(handle);
}

OwnedObject::OwnedObject(OwnedObject &&other) noexcept : handle(other.handle), destroy(other.destroy) {
    other.handle = nullptr;
}

OwnedObject &OwnedObject::operator=(OwnedObject &&other) noexcept {
    if (handle) destroy(handle);
    handle = other.handle;
    other.handle = nullptr;
    return *this;
}

void *OwnedObject::get() const {
    assert(handle && "Using OwnedObject without ownership");
    return handle;
}

void *OwnedObject::release() {
    assert(handle && "Releasing OwnedObject without ownership");
    auto result = handle;
    handle = nullptr;
    return result;
}
