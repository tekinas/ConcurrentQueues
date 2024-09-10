#ifndef FUNCTIONQUEUE_SCSP
#define FUNCTIONQUEUE_SCSP

#include "detail/fq_common.h"

namespace rb {
template<typename FSig, FQOpt opt, bool wait_interface, size_t buffer_align = alignof(std::max_align_t)>
    requires(std::is_function_v<FSig> and std::has_single_bit(buffer_align))
class FunctionQueueSCSP {
public:
    explicit FunctionQueueSCSP(size_t buffer_size, size_t max_functions, allocator_type allocator = {})
        : m_Writer{.byte_rb{
                  .buffer{static_cast<std::byte *>(allocator.allocate_bytes(buffer_size, buffer_align)), buffer_size},
                  .input_pos{},
                  .output_pos{}}},
          m_FunctionArray{allocator.allocate_object<detail::FData<FSig, opt>>(max_functions + 1), max_functions + 1},
          m_Allocator{allocator} {}

    ~FunctionQueueSCSP() {
        if constexpr (opt != FQOpt::InvokeOnce)
            detail::destroy_non_consumed(
                    detail::RingBuffer{.buffer = m_FunctionArray,
                                       .input_pos = m_Writer.input_pos.load(std::memory_order::relaxed),
                                       .output_pos = m_Reader.output_pos.load(std::memory_order::relaxed)});
        m_Allocator.deallocate_object(m_FunctionArray.data(), m_FunctionArray.size());
        m_Allocator.deallocate_bytes(m_Writer.byte_rb.buffer.data(), m_Writer.byte_rb.buffer.size(), buffer_align);
    }

    allocator_type get_allocator() const { return m_Allocator; }

    size_t buffer_size() const { return m_Writer.byte_rb.buffer.size(); }

    size_t max_functions() const { return m_FunctionArray.size() - 1; }

    bool empty() const {
        return m_Writer.input_pos.load(std::memory_order::relaxed) ==
               m_Reader.output_pos.load(std::memory_order::relaxed);
    }

    size_t count() const {
        return detail::count(m_Reader.output_pos.load(std::memory_order::relaxed),
                             m_Writer.input_pos.load(std::memory_order::relaxed), m_FunctionArray.size());
    }

    void wait() const
        requires wait_interface
    {
        auto const output_pos = m_Reader.output_pos.load(std::memory_order::relaxed);
        m_Writer.input_pos.wait(output_pos, std::memory_order::relaxed);
    }

    bool consume(detail::Consumer<FSig, opt> auto &&functor) {
        auto const output_pos = m_Reader.output_pos.load(std::memory_order::relaxed);
        if (output_pos == m_Reader.input_pos) {
            m_Reader.input_pos = m_Writer.input_pos.load(std::memory_order::acquire);
            if (output_pos == m_Reader.input_pos) return false;
        }
        detail::invoke(fwd(functor), m_FunctionArray[output_pos]);
        auto const next_pos = output_pos + 1;
        m_Reader.output_pos.store(next_pos != m_FunctionArray.size() ? next_pos : 0, std::memory_order::release);
        return true;
    }

    size_t consume_all(detail::Consumer<FSig, opt> auto &&functor) {
        detail::RingBuffer const rb{.buffer = m_FunctionArray,
                                    .input_pos = m_Writer.input_pos.load(std::memory_order::acquire),
                                    .output_pos = m_Reader.output_pos.load(std::memory_order::relaxed)};
        ScopeGaurd _ = [&] {
            m_Reader.output_pos.store(rb.input_pos, std::memory_order::release);
            m_Reader.input_pos = rb.input_pos;
        };
        return detail::invoke(functor, rb);
    }

    size_t consume_n(detail::Consumer<FSig, opt> auto &&functor, size_t n) {
        auto const output_pos = m_Reader.output_pos.load(std::memory_order::relaxed);
        auto const input_pos = m_Writer.input_pos.load(std::memory_order::acquire);
        auto const next_pos = detail::next_pos(output_pos, input_pos, m_FunctionArray.size(), n);
        ScopeGaurd _ = [&] {
            m_Reader.output_pos.store(next_pos, std::memory_order::release);
            m_Reader.input_pos = input_pos;
        };
        return detail::invoke(
                functor,
                detail::RingBuffer{.buffer = m_FunctionArray, .input_pos = next_pos, .output_pos = output_pos});
    }

    template<typename T>
    bool push(T &&callable) {
        return emplace<std::remove_cvref_t<T>>(fwd(callable));
    }

    template<typename Callable, typename... CArgs>
        requires detail::valid_callable<Callable, FSig, CArgs...>
    bool emplace(CArgs &&...args) {
        auto const input_pos = m_Writer.input_pos.load(std::memory_order::relaxed);
        auto const next_pos = (input_pos + 1) != m_FunctionArray.size() ? (input_pos + 1) : 0;
        auto ptr = detail::get_storage<Callable>(m_Writer.byte_rb);
        if (next_pos == m_Writer.output_pos or not ptr) {
            sync();
            ptr = detail::get_storage<Callable>(m_Writer.byte_rb);
            if (next_pos == m_Writer.output_pos or not ptr) return false;
        }
        auto const res = detail::emplace<Callable, FSig, opt>(ptr, fwd(args)...);
        m_FunctionArray[input_pos] = res.fd;
        m_Writer.input_pos.store(next_pos, std::memory_order::release);
        if constexpr (wait_interface) m_Writer.input_pos.notify_one();
        m_Writer.byte_rb.input_pos = static_cast<size_t>(res.next_pos - m_Writer.byte_rb.buffer.data());
        return true;
    }

private:
    void sync() {
        m_Writer.output_pos = m_Reader.output_pos.load(std::memory_order::acquire);
        m_Writer.byte_rb.output_pos =
                m_Writer.output_pos != m_Writer.input_pos.load(std::memory_order::relaxed)
                        ? static_cast<size_t>(m_FunctionArray[m_Writer.output_pos].obj - m_Writer.byte_rb.buffer.data())
                        : m_Writer.byte_rb.input_pos;
    }

    struct alignas(rb::hardware_destructive_interference_size) {
        std::atomic<size_t> input_pos{};
        size_t output_pos{};
        detail::RingBuffer<std::byte> byte_rb;
    } m_Writer;
    struct alignas(rb::hardware_destructive_interference_size) {
        std::atomic<size_t> output_pos{};
        size_t input_pos{};
    } m_Reader;
    std::span<detail::FData<FSig, opt>> const m_FunctionArray;
    allocator_type m_Allocator;
};
}// namespace rb

#endif
