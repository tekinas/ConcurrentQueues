#ifndef BUFFERQUEUE_SCSP
#define BUFFERQUEUE_SCSP

#include "detail/rb_common.h"

namespace rb {
template<size_t buffer_align, bool wait_interface>
    requires(std::has_single_bit(buffer_align))
class BufferQueueSCSP {
public:
    using Buffer = std::span<std::byte>;

    explicit BufferQueueSCSP(size_t buffer_size, size_t max_buffers, allocator_type allocator = {})
        : m_Writer{.byte_rb{
                  .buffer{static_cast<std::byte *>(allocator.allocate_bytes(buffer_size, buffer_align)), buffer_size},
                  .input_pos{},
                  .output_pos{}}},
          m_SpliceArray{allocator.allocate_object<Buffer>(max_buffers + 1), max_buffers + 1}, m_Allocator{allocator} {}

    ~BufferQueueSCSP() {
        m_Allocator.deallocate_object(m_SpliceArray.data(), m_SpliceArray.size());
        m_Allocator.deallocate_bytes(m_Writer.byte_rb.buffer.data(), m_Writer.byte_rb.buffer.size(), buffer_align);
    }

    allocator_type get_allocator() const { return m_Allocator; }

    size_t buffer_size() const { return m_Writer.byte_rb.buffer.size(); }

    size_t max_buffers() const { return m_SpliceArray.size() - 1; }

    bool empty() const {
        return m_Writer.input_pos.load(std::memory_order::relaxed) ==
               m_Reader.output_pos.load(std::memory_order::relaxed);
    }

    size_t count() const {
        return detail::count(m_Reader.output_pos.load(std::memory_order::relaxed),
                             m_Writer.input_pos.load(std::memory_order::relaxed), m_SpliceArray.size());
    }

    void wait() const
        requires wait_interface
    {
        auto const output_pos = m_Reader.output_pos.load(std::memory_order::relaxed);
        m_Writer.input_pos.wait(output_pos, std::memory_order::relaxed);
    }

    bool consume(std::invocable<Buffer> auto &&functor) {
        auto const output_pos = m_Reader.output_pos.load(std::memory_order::relaxed);
        if (output_pos == m_Reader.input_pos) {
            m_Reader.input_pos = m_Writer.input_pos.load(std::memory_order::acquire);
            if (output_pos == m_Reader.input_pos) return false;
        }
        std::invoke(fwd(functor), auto{m_SpliceArray[output_pos]});
        auto const next_pos = output_pos + 1;
        m_Reader.output_pos.store(next_pos != m_SpliceArray.size() ? next_pos : 0, std::memory_order::release);
        return true;
    }

    size_t consume_all(std::invocable<Buffer> auto &&functor) {
        detail::RingBuffer const rb{.buffer = m_SpliceArray,
                                    .input_pos = m_Writer.input_pos.load(std::memory_order::acquire),
                                    .output_pos = m_Reader.output_pos.load(std::memory_order::relaxed)};
        ScopeGaurd _ = [&] {
            m_Reader.output_pos.store(rb.input_pos, std::memory_order::release);
            m_Reader.input_pos = rb.input_pos;
        };
        return detail::apply(fwd(functor), rb);
    }

    size_t consume_n(std::invocable<Buffer> auto &&functor, size_t n) {
        auto const output_pos = m_Reader.output_pos.load(std::memory_order::relaxed);
        auto const input_pos = m_Writer.input_pos.load(std::memory_order::acquire);
        auto const next_pos = detail::next_pos(output_pos, input_pos, m_SpliceArray.size(), n);
        ScopeGaurd _ = [&] {
            m_Reader.output_pos.store(next_pos, std::memory_order::release);
            m_Reader.input_pos = input_pos;
        };
        return detail::apply(
                fwd(functor),
                detail::RingBuffer{.buffer = m_SpliceArray, .input_pos = next_pos, .output_pos = output_pos});
    }

    Buffer allocate(size_t size_bytes, size_t alignment) {
        auto const input_pos = m_Writer.input_pos.load(std::memory_order::relaxed);
        auto const next_pos = (input_pos + 1) != m_SpliceArray.size() ? (input_pos + 1) : 0;
        auto buffer = detail::get_storage(m_Writer.byte_rb, size_bytes, alignment);
        if (next_pos == m_Writer.output_pos or buffer.empty()) {
            sync();
            buffer = detail::get_storage(m_Writer.byte_rb, size_bytes, alignment);
            if (next_pos == m_Writer.output_pos or buffer.empty()) return {};
        }
        return buffer;
    }

    size_t release(Buffer buffer_rel) {
        auto const input_pos = m_Writer.input_pos.load(std::memory_order::relaxed);
        auto const next_pos = (input_pos + 1) != m_SpliceArray.size() ? (input_pos + 1) : 0;
        m_SpliceArray[input_pos] = buffer_rel;
        m_Writer.input_pos.store(next_pos, std::memory_order::release);
        if constexpr (wait_interface) m_Writer.input_pos.notify_one();
        m_Writer.byte_rb.input_pos =
                static_cast<size_t>(buffer_rel.data() - m_Writer.byte_rb.buffer.data()) + buffer_rel.size();
        return buffer_rel.size();
    }

    template<typename Functor>
        requires std::is_invocable_r_v<Buffer, Functor, Buffer>
    std::optional<size_t> allocate_and_release(size_t size_bytes, size_t alignment, Functor &&functor) {
        auto const input_pos = m_Writer.input_pos.load(std::memory_order::relaxed);
        auto const next_pos = (input_pos + 1) != m_SpliceArray.size() ? (input_pos + 1) : 0;
        auto buffer = detail::get_storage(m_Writer.byte_rb, size_bytes, alignment);
        if (next_pos == m_Writer.output_pos or buffer.empty()) {
            sync();
            buffer = detail::get_storage(m_Writer.byte_rb, size_bytes, alignment);
            if (next_pos == m_Writer.output_pos or buffer.empty()) return {};
        }
        auto const buffer_rel = std::invoke(fwd(functor), auto{buffer});
        m_SpliceArray[input_pos] = buffer_rel;
        m_Writer.input_pos.store(next_pos, std::memory_order::release);
        if constexpr (wait_interface) m_Writer.input_pos.notify_one();
        m_Writer.byte_rb.input_pos =
                static_cast<size_t>(buffer_rel.data() - m_Writer.byte_rb.buffer.data()) + buffer_rel.size();
        return buffer_rel.size();
    }

private:
    void sync() {
        m_Writer.output_pos = m_Reader.output_pos.load(std::memory_order::acquire);
        m_Writer.byte_rb.output_pos = m_Writer.output_pos != m_Writer.input_pos.load(std::memory_order::relaxed)
                                              ? static_cast<size_t>(m_SpliceArray[m_Writer.output_pos].data() -
                                                                    m_Writer.byte_rb.buffer.data())
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
    std::span<Buffer> const m_SpliceArray;
    allocator_type m_Allocator;
};
}// namespace rb

#endif
