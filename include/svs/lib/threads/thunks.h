/**
 *    Copyright (C) 2023-present, Intel Corporation
 *
 *    You can redistribute and/or modify this software under the terms of the
 *    GNU Affero General Public License version 3.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    version 3 along with this software. If not, see
 *    <https://www.gnu.org/licenses/agpl-3.0.en.html>.
 */

#pragma once

// stdlib
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

// local
#include "svs/lib/misc.h"
#include "svs/lib/threads/types.h"

namespace svs {
namespace threads {

using FunctionType = void(*)(void*, size_t);

/// @brief A function pointer-like object that can point to capturing lambdas.
struct FunctionRef {
  public:
    FunctionType fn = nullptr;
    void* arg = nullptr;

  public:
    FunctionRef() = default;

    // N.B.: Need to constrain the argument to not be a `FunctionRef` (otherwise, this
    // seems to have higher precedence than the copy constructor).
    template<typename F>
        requires (!std::is_same_v<std::remove_const_t<F>, FunctionRef>)
    explicit FunctionRef(F& f)
        : fn{+[](void* arg, size_t tid) { static_cast<F*>(arg)->operator()(tid); }}
        , arg{static_cast<void*>(&f)} {}

    void operator()(size_t tid) const { fn(arg, tid); }
    constexpr friend bool operator==(FunctionRef, FunctionRef) = default;
};

struct ThreadFunctionRef {
  public:
    FunctionRef fn {};
    size_t thread_id = 0;

  public:
    ThreadFunctionRef() = default;
    ThreadFunctionRef(FunctionRef fn_, size_t thread_id_)
        : fn{fn_}, thread_id{thread_id_} {}

    void operator()() const { fn(thread_id); }
};

/////
///// Thunks
/////

namespace thunks {

template <typename F, typename... Args> struct Thunk {};

// No change to the underlying lambda.
template <std::invocable<size_t> F>
struct Thunk<F>
{
    static auto wrap(ThreadCount /*unused*/, F& f) -> F& { return f; }
};

// Static partition
template <typename F, typename I> struct Thunk<F, StaticPartition<I>> {
    static auto wrap(ThreadCount nthreads, F& f, StaticPartition<I> space) {
        // Captures:
        // - `f` by reference: Lives outside function scope.
        // - `space` by value: Cheap to copy.
        // - `nthreads` by value: Cheap to copy.
        return [&f, space, nthreads](uint64_t tid) {
            auto nthr = static_cast<size_t>(nthreads);
            auto r = balance(space.size(), nthr, tid);

            // No work for this thread.
            if (r.empty()) {
                return;
            }

            auto this_range =
                IteratorPair{space.begin() + r.start(), space.begin() + r.stop()};
            f(this_range, tid);
        };
    }
};

// Dynamic partition
template <typename F, typename I> struct Thunk<F, DynamicPartition<I>> {
    static auto
    wrap(ThreadCount SVS_UNUSED(nthreads), F& f, DynamicPartition<I> space) {
        auto count = std::make_shared<std::atomic<uint64_t>>(0);
        return [&f, space, count](uint64_t tid) {
            size_t grainsize = space.grainsize;
            size_t iterator_size = space.size();
            for (;;) {
                uint64_t i = count->fetch_add(1, std::memory_order_relaxed);
                auto start = grainsize * i;
                if (start >= iterator_size) {
                    return;
                }

                auto stop = std::min(grainsize * (i + 1), iterator_size);
                auto this_range =
                    IteratorPair{std::begin(space) + start, std::begin(space) + stop};
                f(this_range, tid);
            }
        };
    }
};

// Thunk entry point.
template <typename F, typename... Args>
auto wrap(ThreadCount nthreads, F& f, Args&&... args) {
    return Thunk<F, std::decay_t<Args>...>::wrap(nthreads, f, std::forward<Args>(args)...);
}

} // namespace thunks
} // namespace threads
} // namespace svs
