#ifndef FUNCTIONQUEUE
#define FUNCTIONQUEUE

#include "detail/fq_common.h"

namespace rb {
template<typename FSig, FQOpt opt, size_t buffer_align = alignof(std::max_align_t)>
    requires(std::is_function_v<FSig> and std::has_single_bit(buffer_align))
class FunctionQueue {
public:
    explicit FunctionQueue(size_t buffer_size, size_t max_functions, allocator_type allocator = {})
        : m_FunctionRB{.buffer{allocator.allocate_object<detail::FData<FSig, opt>>(max_functions + 1),
                               max_functions + 1},
                       .input_pos{},
                       .output_pos{}},
          m_ByteRB{.buffer = {static_cast<std::byte *>(allocator.allocate_bytes(buffer_size, buffer_align)),
                              buffer_size},
                   .input_pos{},
                   .output_pos{}},
          m_Allocator{allocator} {}

    ~FunctionQueue() {
        if constexpr (opt != FQOpt::InvokeOnce) detail::destroy_non_consumed(m_FunctionRB);
        m_Allocator.deallocate_bytes(m_ByteRB.buffer.data(), m_ByteRB.buffer.size(), buffer_align);
        m_Allocator.deallocate_object(m_FunctionRB.buffer.data(), m_FunctionRB.buffer.size());
    }

    allocator_type get_allocator() const { return m_Allocator; }

    size_t buffer_size() const { return m_ByteRB.buffer.size(); }

    size_t max_functions() const { return m_FunctionRB.buffer.size() - 1; }

    bool empty() const { return m_FunctionRB.input_pos == m_FunctionRB.output_pos; }

    size_t count() const {
        return detail::count(m_FunctionRB.output_pos, m_FunctionRB.input_pos, m_FunctionRB.buffer.size());
    }

    bool consume(detail::Consumer<FSig, opt> auto &&functor) {
        if (empty()) return false;
        detail::invoke(fwd(functor), m_FunctionRB.buffer[m_FunctionRB.output_pos]);
        auto const next_pos = m_FunctionRB.output_pos + 1;
        set_output_pos(next_pos != m_FunctionRB.buffer.size() ? next_pos : 0);
        return true;
    }

    size_t consume_all(detail::Consumer<FSig, opt> auto &&functor) {
        ScopeGaurd _ = [&] { set_output_pos(m_FunctionRB.input_pos); };
        return detail::invoke(functor, m_FunctionRB);
    }

    size_t consume_n(detail::Consumer<FSig, opt> auto &&functor, size_t n) {
        auto const next_pos =
                detail::next_pos(m_FunctionRB.output_pos, m_FunctionRB.input_pos, m_FunctionRB.buffer.size(), n);
        ScopeGaurd _ = [&] { set_output_pos(next_pos); };
        return detail::invoke(functor, detail::RingBuffer{.buffer = m_FunctionRB.buffer,
                                                          .input_pos = next_pos,
                                                          .output_pos = m_FunctionRB.output_pos});
    }

    template<typename T>
    bool push(T &&callable) {
        return emplace<std::remove_cvref_t<T>>(fwd(callable));
    }

    template<typename Callable, typename... CArgs>
        requires detail::valid_callable<Callable, FSig, CArgs...>
    bool emplace(CArgs &&...args) {
        size_t next_pos = m_FunctionRB.input_pos + 1;
        auto const ptr = detail::get_storage<Callable>(m_ByteRB);
        if (next_pos == m_FunctionRB.buffer.size()) next_pos = 0;
        if (next_pos == m_FunctionRB.output_pos or not ptr) return false;
        auto const res = detail::emplace<Callable, FSig, opt>(ptr, fwd(args)...);
        m_ByteRB.input_pos = static_cast<size_t>(res.next_pos - m_ByteRB.buffer.data());
        m_FunctionRB.buffer[m_FunctionRB.input_pos] = res.fd;
        m_FunctionRB.input_pos = next_pos;
        return true;
    }

private:
    void set_output_pos(size_t next_pos) {
        m_FunctionRB.output_pos = next_pos;
        m_ByteRB.output_pos =
                m_FunctionRB.output_pos != m_FunctionRB.input_pos
                        ? static_cast<size_t>(m_FunctionRB.buffer[m_FunctionRB.output_pos].obj - m_ByteRB.buffer.data())
                        : m_ByteRB.input_pos;
    }

    detail::RingBuffer<detail::FData<FSig, opt>> m_FunctionRB;
    detail::RingBuffer<std::byte> m_ByteRB;
    allocator_type m_Allocator;
};
}// namespace rb

#endif
