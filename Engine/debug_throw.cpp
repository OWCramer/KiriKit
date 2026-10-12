/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Debug builds of the harness only (-DSHIORIKRKR_DEBUG_THROW=ON): prints a backtrace for
// every C++ exception thrown inside the engine image, then throws it as usual. The engine
// turns many exceptions into one-word TJS errors ("vector"), which says nothing about where.
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <execinfo.h>
#include <typeinfo>

extern "C" void __cxa_throw(void *thrown, std::type_info *type, void (*destructor)(void *)) {
    using Throw = void (*)(void *, std::type_info *, void (*)(void *));
    static Throw real = reinterpret_cast<Throw>(dlsym(RTLD_NEXT, "__cxa_throw"));
    if (std::getenv("SHIORIKRKR_TRACE_THROW")) {
        void *frames[32];
        const int count = backtrace(frames, 32);
        std::fprintf(stderr, "== throw %s\n", type ? type->name() : "?");
        backtrace_symbols_fd(frames, count, 2);
    }
    real(thrown, type, destructor);
    __builtin_unreachable();
}
