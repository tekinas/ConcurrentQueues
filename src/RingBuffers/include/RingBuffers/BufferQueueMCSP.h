#ifndef BUFFERQUEUE_MCSP
#define BUFFERQUEUE_MCSP

#include "detail/rb_common.h"

namespace rb {
template<size_t buffer_align, bool wait_interface>
    requires(std::has_single_bit(buffer_align))
class BufferQueueMCSP {
public:
    using Buffer = std::span<std::byte>;

    class Reader {
    public:
        template<bool check_once, bool release>
        bool consume(std::invocable<Buffer> auto &&functor) {
            auto const rp = detail::reserve_one<check_once, tb>(m_BQ->m_OutputPos, m_BQ->m_Writer.input_pos,
                                                                m_BQ->m_SpliceArray.size());
            if (not rp) return false;
            std::invoke(fwd(functor), auto{m_BQ->m_SpliceArray[rp->output_pos]});
            if constexpr (release) detail::release_reader(m_BQ->m_PositionArray[m_Index], rp->next_output_pos);
            return true;
        }

        template<bool check_once>
        size_t consume_all(std::invocable<Buffer> auto &&functor) {
            auto const rp = detail::reserve_all<check_once, tb>(m_BQ->m_OutputPos, m_BQ->m_Writer.input_pos);
            return rp ? detail::apply(fwd(functor), RingBuffer{.buffer = m_BQ->m_SpliceArray,
                                                               .input_pos = rp->next_output_pos,
                                                               .output_pos = rp->output_pos})
                      : 0;
        }

        template<bool check_once, bool release>
        size_t consume_n(std::invocable<Buffer> auto &&functor, size_t n) {
            auto const rp = detail::reserve_n<check_once, tb>(m_BQ->m_OutputPos, m_BQ->m_Writer.input_pos,
                                                              m_BQ->m_SpliceArray.size(), n);
            if (not rp) return 0;
            auto const nc = detail::apply(fwd(functor), RingBuffer{.buffer = m_BQ->m_SpliceArray,
                                                                   .input_pos = rp->next_output_pos,
                                                                   .output_pos = rp->output_pos});
            if constexpr (release) detail::release_reader(m_BQ->m_PositionArray[m_Index], rp->next_output_pos);
            return nc;
        }

        ~Reader() { detail::release_reader(m_BQ->m_PositionArray[m_Index]); }

        Reader(Reader const &) = delete;

        Reader &operator=(Reader const &) = delete;

    private:
        explicit Reader(BufferQueueMCSP *bq, size_t i) : m_BQ{bq}, m_Index{i} {
            detail::set_reader<tb>(m_BQ->m_PositionArray[m_Index], m_BQ->m_OutputPos);
        }

        friend BufferQueueMCSP;

        BufferQueueMCSP *m_BQ;
        size_t m_Index;
    };

    explicit BufferQueueMCSP(size_t buffer_size, size_t max_buffers, size_t max_readers, allocator_type allocator = {})
        : m_Writer{.byte_rb{
                  .buffer{static_cast<std::byte *>(allocator.allocate_bytes(buffer_size, buffer_align)), buffer_size},
                  .input_pos{},
                  .output_pos{}}},
          m_SpliceArray{allocator.allocate_object<Buffer>(max_buffers + 1), max_buffers + 1},
          m_PositionArray{allocator.allocate_object<rb::CacheAligned<std::atomic<size_t>>>(max_readers), max_readers},
          m_Allocator{allocator} {
        detail::init_readers(m_PositionArray);
    }

    ~BufferQueueMCSP() {
        m_Allocator.deallocate_object(m_SpliceArray.data(), m_SpliceArray.size());
        m_Allocator.deallocate_bytes(m_Writer.byte_rb.buffer.data(), m_Writer.byte_rb.buffer.size(), buffer_align);
        m_Allocator.deallocate_object(m_PositionArray.data(), m_PositionArray.size());
    }

    allocator_type get_allocator() const { return m_Allocator; }

    size_t buffer_size() const { return m_Writer.byte_rb.buffer.size(); }

    size_t capacity() const { return m_SpliceArray.size() - 1; }

    size_t max_readers() const { return m_PositionArray.size(); }

    bool empty() const { return detail::empty<tb>(m_OutputPos, m_Writer.input_pos); }

    size_t count() const { return detail::count<tb>(m_OutputPos, m_Writer.input_pos, m_SpliceArray.size()); }

    void wait() const
        requires wait_interface
    {
        auto const output_pos = m_OutputPos.load(std::memory_order::relaxed);
        m_Writer.input_pos.wait(output_pos, std::memory_order::relaxed);
    }

    auto get_reader(size_t index) { return Reader{this, index}; }

    Buffer allocate(size_t size_bytes, size_t alignment) {
        Index const pos = m_Writer.input_pos.load(std::memory_order::relaxed);
        auto const input_pos = detail::value<tb>(pos);
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
        Index const pos = m_Writer.input_pos.load(std::memory_order::relaxed);
        auto const input_pos = detail::value<tb>(pos);
        auto const next_pos = (input_pos + 1) != m_SpliceArray.size() ? (input_pos + 1) : 0;
        m_SpliceArray[input_pos] = buffer_rel;
        detail::publish<tb>(m_Writer.input_pos, pos, next_pos, m_OutputPos);
        if constexpr (wait_interface) m_Writer.input_pos.notify_one();
        m_Writer.byte_rb.input_pos =
                static_cast<size_t>(buffer_rel.data() - m_Writer.byte_rb.buffer.data()) + buffer_rel.size();
        return buffer_rel.size();
    }

    template<typename Functor>
        requires std::is_invocable_r_v<Buffer, Functor, Buffer>
    std::optional<size_t> allocate_and_release(size_t size_bytes, size_t alignment, Functor &&functor) {
        Index const pos = m_Writer.input_pos.load(std::memory_order::relaxed);
        auto const input_pos = detail::value<tb>(pos);
        auto const next_pos = (input_pos + 1) != m_SpliceArray.size() ? (input_pos + 1) : 0;
        auto buffer = detail::get_storage(m_Writer.byte_rb, size_bytes, alignment);
        if (next_pos == m_Writer.output_pos or buffer.empty()) {
            sync();
            buffer = detail::get_storage(m_Writer.byte_rb, size_bytes, alignment);
            if (next_pos == m_Writer.output_pos or buffer.empty()) return {};
        }
        auto const buffer_rel = std::invoke(fwd(functor), auto{buffer});
        m_SpliceArray[input_pos] = buffer_rel;
        detail::publish<tb>(m_Writer.input_pos, pos, next_pos, m_OutputPos);
        if constexpr (wait_interface) m_Writer.input_pos.notify_one();
        m_Writer.byte_rb.input_pos =
                static_cast<size_t>(buffer_rel.data() - m_Writer.byte_rb.buffer.data()) + buffer_rel.size();
        return buffer_rel.size();
    }

private:
    void sync() {
        m_Writer.output_pos = detail::sync<tb>(m_Writer.output_pos, m_PositionArray, m_OutputPos);
        m_Writer.byte_rb.output_pos = m_Writer.output_pos != detail::value<tb>(m_Writer.input_pos)
                                              ? static_cast<size_t>(m_SpliceArray[m_Writer.output_pos].data() -
                                                                    m_Writer.byte_rb.buffer.data())
                                              : m_Writer.byte_rb.input_pos;
    }

    using RingBuffer = detail::RingBuffer<Buffer>;
    using Index = uint64_t;
    static constexpr size_t tb = 16;
    struct alignas(rb::hardware_destructive_interference_size) {
        std::atomic<Index> input_pos{};
        size_t output_pos{};
        detail::RingBuffer<std::byte> byte_rb;
    } m_Writer;
    alignas(rb::hardware_destructive_interference_size) std::atomic<Index> m_OutputPos{};
    std::span<Buffer> const m_SpliceArray;
    std::span<rb::CacheAligned<std::atomic<size_t>>> const m_PositionArray;
    allocator_type m_Allocator;
};
}// namespace rb

#endif
