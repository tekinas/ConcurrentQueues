#ifndef RB_COMMON
#define RB_COMMON

#include "move_forward.hpp"
#include "scope.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>

namespace rb {
using allocator_type = std::pmr::polymorphic_allocator<>;

#ifdef __cpp_lib_hardware_interference_size
static constexpr size_t hardware_destructive_interference_size = std::hardware_destructive_interference_size;
static constexpr size_t hardware_constructive_interference_size = std::hardware_constructive_interference_size;
#else
static constexpr size_t hardware_destructive_interference_size = 64;
static constexpr size_t hardware_constructive_interference_size = 64;
#endif

template<typename T>
struct alignas(rb::hardware_destructive_interference_size) CacheAligned {
    T value;
};

template<size_t ns = 1, size_t max_checks = 8, typename Predicate>
    requires std::is_invocable_r_v<bool, Predicate>
void wait(Predicate &&stop_waiting) {
    constexpr auto sleep_dur = std::chrono::nanoseconds{ns};
    for (size_t count = 1; not stop_waiting(); ++count)
        if (count == max_checks) {
            count = 0;
            std::this_thread::sleep_for(sleep_dur);
        }
}
}// namespace rb

namespace rb::detail {
template<typename U>
concept Unsigned = std::is_unsigned_v<U>;

template<typename U, size_t tag_bits>
constexpr U tag_mask = (1uz << tag_bits) - 1uz;

template<size_t tb, Unsigned U>
size_t tag(U index) {
    return index & tag_mask<U, tb>;
}

template<size_t tb, Unsigned U>
size_t value(U index) {
    return index >> tb;
}

template<size_t tb, Unsigned U>
size_t value(std::atomic<U> const &index) {
    return value<tb>(index.load(std::memory_order::relaxed));
}

template<size_t tb, Unsigned U>
U tagged_size(size_t value, size_t tag) {
    return (static_cast<U>(value) << tb) | (static_cast<U>(tag) & tag_mask<U, tb>);
}

template<size_t tb, Unsigned U>
U incr_tagged(U ts, size_t value) {
    return tagged_size<tb, U>(value, tag<tb>(ts) + 1);
}

template<size_t tb, Unsigned U>
U same_tagged(U ts, size_t value) {
    return tagged_size<tb, U>(value, tag<tb>(ts));
}

template<size_t tb, Unsigned U>
bool empty(U output_pos, U input_pos) {
    return (tag<tb>(input_pos) < tag<tb>(output_pos)) | (value<tb>(output_pos) == value<tb>(input_pos));
}

template<size_t tb, Unsigned U>
bool empty(std::atomic<U> const &output_pos, std::atomic<U> const &input_pos) {
    return empty<tb>(output_pos.load(std::memory_order::relaxed), input_pos.load(std::memory_order::relaxed));
}

inline size_t count(size_t output_pos, size_t input_pos, size_t buffer_size) {
    auto const diff = input_pos - output_pos;
    return output_pos > input_pos ? diff + buffer_size : diff;
}

template<size_t tb, Unsigned U>
inline size_t count(std::atomic<U> const &output_pos, std::atomic<U> const &input_pos, size_t buffer_size) {
    auto const op = output_pos.load(std::memory_order::relaxed);
    auto const ip = input_pos.load(std::memory_order::relaxed);
    return (tag<tb>(ip) >= tag<tb>(op)) ? count(value<tb>(op), value<tb>(ip), buffer_size) : 0;
}

inline size_t count_avl(size_t output_pos, size_t input_pos, size_t buffer_size) {
    return input_pos < output_pos ? (output_pos - input_pos - 1) : (buffer_size - input_pos - 1 + (output_pos != 0));
}

inline size_t next_pos(size_t output_pos, size_t input_pos, size_t buffer_size, size_t n) {
    auto const next_pos = output_pos + n;
    if (output_pos <= input_pos) return std::min(input_pos, next_pos);
    return next_pos < buffer_size ? next_pos : std::min(input_pos, next_pos - buffer_size);
}

inline constexpr auto max_pos = std::numeric_limits<size_t>::max();

template<typename T>
struct RingBuffer {
    std::span<T> buffer;
    size_t input_pos;
    size_t output_pos;
};

struct ReserveResult {
    size_t output_pos;
    size_t next_output_pos;
};

template<bool check_once, size_t tb, Unsigned U>
inline std::optional<ReserveResult> reserve_one(std::atomic<U> &output_pos, std::atomic<U> const &input_pos,
                                                size_t array_size)
    requires check_once
{
    auto op = output_pos.load(std::memory_order::relaxed);
    auto const ip = input_pos.load(std::memory_order::acquire);
    if (empty<tb>(op, ip)) return {};
    size_t next_pos = value<tb>(op) + 1;
    if (next_pos == array_size) next_pos = 0;
    if (output_pos.compare_exchange_strong(op, same_tagged<tb>(ip, next_pos), std::memory_order::acq_rel,
                                           std::memory_order::relaxed))
        return ReserveResult{value<tb>(op), next_pos};
    return {};
}

template<bool check_once, size_t tb, Unsigned U>
inline std::optional<ReserveResult> reserve_one(std::atomic<U> &output_pos, std::atomic<U> const &input_pos,
                                                size_t array_size)
    requires(not check_once)
{
    for (auto op = output_pos.load(std::memory_order::relaxed);;) {
        auto const ip = input_pos.load(std::memory_order::acquire);
        if (empty<tb>(op, ip)) return {};
        size_t next_pos = value<tb>(op) + 1;
        if (next_pos == array_size) next_pos = 0;
        if (output_pos.compare_exchange_weak(op, same_tagged<tb>(ip, next_pos), std::memory_order::acq_rel,
                                             std::memory_order::relaxed))
            return ReserveResult{value<tb>(op), next_pos};
    }
}

template<bool check_once, size_t tb, Unsigned U>
inline std::optional<ReserveResult> reserve_all(std::atomic<U> &output_pos, std::atomic<U> const &input_pos)
    requires check_once
{
    auto op = output_pos.load(std::memory_order::relaxed);
    auto const ip = input_pos.load(std::memory_order::acquire);
    if (empty<tb>(op, ip)) return {};
    if (output_pos.compare_exchange_strong(op, ip, std::memory_order::acq_rel, std::memory_order::relaxed))
        return ReserveResult{value<tb>(op), value<tb>(ip)};
    return {};
}

template<bool check_once, size_t tb, Unsigned U>
inline std::optional<ReserveResult> reserve_all(std::atomic<U> &output_pos, std::atomic<U> const &input_pos)
    requires(not check_once)
{
    for (auto op = output_pos.load(std::memory_order::relaxed);;) {
        auto const ip = input_pos.load(std::memory_order::acquire);
        if (empty<tb>(op, ip)) return {};
        if (output_pos.compare_exchange_weak(op, ip, std::memory_order::acq_rel, std::memory_order::relaxed))
            return ReserveResult{value<tb>(op), value<tb>(ip)};
    }
}

template<bool check_once, size_t tb, Unsigned U>
inline std::optional<ReserveResult> reserve_n(std::atomic<U> &output_pos, std::atomic<U> const &input_pos,
                                              size_t array_size, size_t n)
    requires check_once
{
    auto op = output_pos.load(std::memory_order::relaxed);
    auto const ip = input_pos.load(std::memory_order::acquire);
    if (empty<tb>(op, ip)) return {};
    auto const next_pos = detail::next_pos(value<tb>(op), value<tb>(ip), array_size, n);
    if (output_pos.compare_exchange_strong(op, same_tagged<tb>(ip, next_pos), std::memory_order::acq_rel,
                                           std::memory_order::relaxed))
        return ReserveResult{value<tb>(op), next_pos};
    return {};
}

template<bool check_once, size_t tb, Unsigned U>
inline std::optional<ReserveResult> reserve_n(std::atomic<U> &output_pos, std::atomic<U> const &input_pos,
                                              size_t array_size, size_t n)
    requires(not check_once)
{
    for (auto op = output_pos.load(std::memory_order::relaxed);;) {
        auto const ip = input_pos.load(std::memory_order::acquire);
        if (empty<tb>(op, ip)) return {};
        auto const next_pos = detail::next_pos(value<tb>(op), value<tb>(ip), array_size, n);
        if (output_pos.compare_exchange_strong(op, same_tagged<tb>(ip, next_pos), std::memory_order::acq_rel,
                                               std::memory_order::relaxed))
            return ReserveResult{value<tb>(op), next_pos};
    }
}

template<size_t tb, Unsigned U>
inline size_t sync(size_t prev_pos, std::span<rb::CacheAligned<std::atomic<std::size_t>> const> position_array,
                   std::atomic<U> const &current_pos) {
    auto const cp = value<tb>(current_pos.load(std::memory_order::acquire));
    if (cp == prev_pos) return prev_pos;
    auto gpos = cp > prev_pos ? cp : max_pos, lpos = cp;
    for (auto &pos : position_array)
        if (auto const output_pos = pos.value.load(std::memory_order::acquire); output_pos == prev_pos) [[unlikely]]
            return prev_pos;
        else if (output_pos > prev_pos) gpos = std::min(gpos, output_pos);
        else lpos = std::min(lpos, output_pos);
    return cp > prev_pos ? gpos : (gpos != max_pos ? gpos : lpos);
}

inline void init_readers(std::span<rb::CacheAligned<std::atomic<size_t>>> position_array) {
    std::ranges::uninitialized_fill(position_array, detail::max_pos);
}

template<size_t tb, Unsigned U>
void set_reader(rb::CacheAligned<std::atomic<size_t>> &pos, std::atomic<U> const &output_pos) {
    pos.value.store(value<tb>(output_pos), std::memory_order::relaxed);
}

inline void release_reader(rb::CacheAligned<std::atomic<size_t>> &pos, size_t next_pos) {
    pos.value.store(next_pos, std::memory_order::release);
}

inline void release_reader(rb::CacheAligned<std::atomic<size_t>> &pos) {
    pos.value.store(detail::max_pos, std::memory_order::release);
}

template<size_t tb, Unsigned U>
inline void publish(std::atomic<U> &input_pos, U current_pos, size_t next_pos, std::atomic<U> &output_pos) {
    auto const pos = incr_tagged<tb>(current_pos, next_pos);
    input_pos.store(pos, std::memory_order::release);
    if (detail::tag<tb>(pos) == 0) output_pos.fetch_and(~tag_mask<U, tb>, std::memory_order::acq_rel);
}

inline std::span<std::byte> get_storage(RingBuffer<std::byte> const &rb, size_t bytes, size_t alignment) {
    auto getAlignedStorage = [=](std::span<std::byte> buffer) -> std::span<std::byte> {
        auto const avl_bytes = buffer.size();
        if (avl_bytes < bytes) return {};
        auto const ptr = std::bit_cast<uintptr_t>(buffer.data());
        auto const aligned_ptr = (ptr - 1uz + alignment) & -alignment;
        auto const diff = aligned_ptr - ptr;
        if (diff > avl_bytes - bytes) return {};
        return {std::bit_cast<std::byte *>(aligned_ptr), avl_bytes - diff};
    };
    if (rb.input_pos >= rb.output_pos) {
        if (auto const buffer = getAlignedStorage(rb.buffer.subspan(rb.input_pos)); not buffer.empty()) return buffer;
        if (rb.output_pos) return getAlignedStorage(rb.buffer.first(rb.output_pos - 1));
        return {};
    }
    return getAlignedStorage(rb.buffer.subspan(rb.input_pos, rb.output_pos - rb.input_pos - 1));
}

template<typename Obj>
inline size_t apply(std::invocable<Obj &> auto &&functor, RingBuffer<Obj> const &rb) {
    if (rb.input_pos == rb.output_pos) return 0;
    if (auto const buffer = rb.buffer; rb.output_pos > rb.input_pos) {
        auto const rng1 = buffer.subspan(rb.output_pos), rng2 = buffer.first(rb.input_pos);
        std::ranges::for_each(rng1, functor);
        std::ranges::for_each(rng2, functor);
        return rng1.size() + rng2.size();
    } else {
        auto const rng = buffer.subspan(rb.output_pos, rb.input_pos - rb.output_pos);
        std::ranges::for_each(rng, functor);
        return rng.size();
    }
}

template<typename Obj>
inline size_t invoke_and_destroy(auto &func, RingBuffer<Obj> const &rb) {
    return detail::apply(
            [&](Obj &obj) {
                std::invoke(func, obj);
                std::destroy_at(&obj);
            },
            rb);
}

template<typename Obj>
inline void destroy_non_consumed(RingBuffer<Obj> const &rb) {
    if (rb.output_pos == rb.input_pos) return;
    if (rb.output_pos > rb.input_pos) {
        std::ranges::destroy(rb.buffer.subspan(rb.output_pos));
        std::ranges::destroy(rb.buffer.first(rb.input_pos));
    } else std::ranges::destroy(rb.buffer.subspan(rb.output_pos, rb.input_pos - rb.output_pos));
}
}// namespace rb::detail

#endif
