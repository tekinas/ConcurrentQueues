#ifndef OBJECTQUEUE_SCSP
#define OBJECTQUEUE_SCSP

#include "detail/rb_common.h"

namespace rb {
template<typename Obj, bool wait_interface>
    requires(std::is_object_v<Obj> and std::is_destructible_v<Obj>)
class ObjectQueueSCSP {
public:
    explicit ObjectQueueSCSP(size_t buffer_size, allocator_type allocator = {})
        : m_Buffer{allocator.allocate_object<Obj>(buffer_size + 1), buffer_size + 1}, m_Allocator{allocator} {}

    ~ObjectQueueSCSP() {
        detail::destroy_non_consumed(RingBuffer{.buffer = m_Buffer,
                                                .input_pos = m_Writer.input_pos.load(std::memory_order::relaxed),
                                                .output_pos = m_Reader.output_pos.load(std::memory_order::relaxed)});
        m_Allocator.deallocate_object(m_Buffer.data(), m_Buffer.size());
    }

    allocator_type get_allocator() const { return m_Allocator; }

    size_t capacity() const { return m_Buffer.size() - 1; }

    bool empty() const {
        return m_Writer.input_pos.load(std::memory_order::relaxed) ==
               m_Reader.output_pos.load(std::memory_order::relaxed);
    }

    size_t count() const {
        return detail::count(m_Reader.output_pos.load(std::memory_order::relaxed),
                             m_Writer.input_pos.load(std::memory_order::relaxed), m_Buffer.size());
    }

    void wait() const
        requires wait_interface
    {
        auto const output_pos = m_Reader.output_pos.load(std::memory_order::relaxed);
        m_Writer.input_pos.wait(output_pos, std::memory_order::relaxed);
    }

    bool consume(std::invocable<Obj &> auto &&functor) {
        auto const output_pos = m_Reader.output_pos.load(std::memory_order::relaxed);
        if (output_pos == m_Reader.input_pos) {
            m_Reader.input_pos = m_Writer.input_pos.load(std::memory_order::acquire);
            if (output_pos == m_Reader.input_pos) return false;
        }
        auto &obj = m_Buffer[output_pos];
        std::invoke(fwd(functor), obj);
        std::destroy_at(&obj);
        auto const next_pos = output_pos + 1;
        m_Reader.output_pos.store(next_pos != m_Buffer.size() ? next_pos : 0, std::memory_order::release);
        return true;
    }

    size_t consume_all(std::invocable<Obj &> auto &&functor) {
        RingBuffer const rb{.buffer = m_Buffer,
                            .input_pos = m_Writer.input_pos.load(std::memory_order::acquire),
                            .output_pos = m_Reader.output_pos.load(std::memory_order::relaxed)};
        ScopeGaurd _ = [&] {
            m_Reader.output_pos.store(rb.input_pos, std::memory_order::release);
            m_Reader.input_pos = rb.input_pos;
        };
        return detail::invoke_and_destroy(functor, rb);
    }

    size_t consume_n(std::invocable<Obj &> auto &&functor, size_t n) {
        auto const output_pos = m_Reader.output_pos.load(std::memory_order::relaxed);
        auto const input_pos = m_Writer.input_pos.load(std::memory_order::acquire);
        auto const next_pos = detail::next_pos(output_pos, input_pos, m_Buffer.size(), n);
        ScopeGaurd _ = [&] {
            m_Reader.output_pos.store(next_pos, std::memory_order::release);
            m_Reader.input_pos = input_pos;
        };
        return detail::invoke_and_destroy(
                functor, RingBuffer{.buffer = m_Buffer, .input_pos = next_pos, .output_pos = output_pos});
    }

    bool push(Obj const &obj) { return emplace(obj); }

    bool push(Obj &&obj) { return emplace(mov(obj)); }

    template<typename... Args>
        requires std::is_constructible_v<Obj, Args...>
    bool emplace(Args &&...args) {
        auto const input_pos = m_Writer.input_pos.load(std::memory_order::relaxed);
        auto const next_pos = (input_pos + 1) != m_Buffer.size() ? (input_pos + 1) : 0;
        if (next_pos == m_Writer.output_pos) {
            m_Writer.output_pos = m_Reader.output_pos.load(std::memory_order::acquire);
            if (next_pos == m_Writer.output_pos) return false;
        }
        std::construct_at(&m_Buffer[input_pos], fwd(args)...);
        m_Writer.input_pos.store(next_pos, std::memory_order::release);
        if constexpr (wait_interface) m_Writer.input_pos.notify_one();
        return true;
    }

    template<typename Functor>
        requires std::is_invocable_r_v<size_t, Functor, std::span<Obj>>
    size_t emplace_n(Functor &&functor) {
        auto const input_pos = m_Writer.input_pos.load(std::memory_order::relaxed);
        auto n_avl = detail::count_avl(m_Writer.output_pos, input_pos, m_Buffer.size());
        if (not n_avl) {
            m_Writer.output_pos = m_Reader.output_pos.load(std::memory_order::acquire);
            n_avl = detail::count_avl(m_Writer.output_pos, input_pos, m_Buffer.size());
            if (not n_avl) return 0;
        }
        auto const obj_emplaced = std::invoke(fwd(functor), m_Buffer.subspan(input_pos, n_avl));
        auto const next_pos = input_pos + obj_emplaced;
        m_Writer.input_pos.store(next_pos != m_Buffer.size() ? next_pos : 0, std::memory_order::release);
        if constexpr (wait_interface)
            obj_emplaced == 1 ? m_Writer.input_pos.notify_one() : m_Writer.input_pos.notify_all();
        return obj_emplaced;
    }

private:
    using RingBuffer = detail::RingBuffer<Obj>;
    struct alignas(rb::hardware_destructive_interference_size) {
        std::atomic<size_t> input_pos{};
        size_t output_pos{};
    } m_Writer;
    struct alignas(rb::hardware_destructive_interference_size) {
        std::atomic<size_t> output_pos{};
        size_t input_pos{};
    } m_Reader;
    std::span<Obj> const m_Buffer;
    allocator_type m_Allocator;
};
}// namespace rb

#endif
