#ifndef FUNCTIONQUEUE_MCSP
#define FUNCTIONQUEUE_MCSP

#include "detail/fq_common.h"

namespace rb {
template<typename FSig, FQOpt opt, bool wait_interface, size_t buffer_align = alignof(std::max_align_t)>
    requires(std::is_function_v<FSig> and std::has_single_bit(buffer_align))
class FunctionQueueMCSP {
public:
    class Reader {
    public:
        template<bool check_once, bool release>
        bool consume(detail::Consumer<FSig, opt> auto &&functor) {
            auto const rp = detail::reserve_one<check_once, tb>(m_FQ->m_OutputPos, m_FQ->m_Writer.input_pos,
                                                                m_FQ->m_FunctionArray.size());
            if (not rp) return false;
            detail::invoke(fwd(functor), m_FQ->m_FunctionArray[rp->output_pos]);
            if constexpr (release) detail::release_reader(m_FQ->m_PositionArray[m_Index], rp->next_output_pos);
            return true;
        }

        template<bool check_once>
        size_t consume_all(detail::Consumer<FSig, opt> auto &&functor) {
            auto const rp = detail::reserve_all<check_once, tb>(m_FQ->m_OutputPos, m_FQ->m_Writer.input_pos);
            return rp ? detail::invoke(functor, RingBuffer{.buffer = m_FQ->m_FunctionArray,
                                                           .input_pos = rp->next_output_pos,
                                                           .output_pos = rp->output_pos})
                      : 0;
        }

        template<bool check_once, bool release>
        size_t consume_n(detail::Consumer<FSig, opt> auto &&functor, size_t n) {
            auto const rp = detail::reserve_n<check_once, tb>(m_FQ->m_OutputPos, m_FQ->m_Writer.input_pos,
                                                              m_FQ->m_FunctionArray.size(), n);
            if (not rp) return 0;
            auto const nc = detail::invoke(functor, RingBuffer{.buffer = m_FQ->m_FunctionArray,
                                                               .input_pos = rp->next_output_pos,
                                                               .output_pos = rp->output_pos});
            if constexpr (release) detail::release_reader(m_FQ->m_PositionArray[m_Index], rp->next_output_pos);
            return nc;
        }

        ~Reader() { detail::release_reader(m_FQ->m_PositionArray[m_Index]); }

        Reader(Reader const &) = delete;

        Reader &operator=(Reader const &) = delete;

    private:
        explicit Reader(FunctionQueueMCSP *fq, size_t i) : m_FQ{fq}, m_Index{i} {
            detail::set_reader<tb>(m_FQ->m_PositionArray[m_Index], m_FQ->m_OutputPos);
        }

        friend FunctionQueueMCSP;

        FunctionQueueMCSP *m_FQ;
        size_t m_Index;
    };

    explicit FunctionQueueMCSP(size_t buffer_size, size_t max_functions, size_t max_readers,
                               allocator_type allocator = {})
        : m_Writer{.byte_rb{
                  .buffer{static_cast<std::byte *>(allocator.allocate_bytes(buffer_size, buffer_align)), buffer_size},
                  .input_pos{},
                  .output_pos{}}},
          m_FunctionArray{allocator.allocate_object<detail::FData<FSig, opt>>(max_functions + 1), max_functions + 1},
          m_PositionArray{allocator.allocate_object<rb::CacheAligned<std::atomic<size_t>>>(max_readers), max_readers},
          m_Allocator{allocator} {
        detail::init_readers(m_PositionArray);
    }

    ~FunctionQueueMCSP() {
        if constexpr (opt != FQOpt::InvokeOnce)
            detail::destroy_non_consumed(RingBuffer{.buffer = m_FunctionArray,
                                                    .input_pos = detail::value<tb>(m_Writer.input_pos),
                                                    .output_pos = detail::value<tb>(m_OutputPos)});
        m_Allocator.deallocate_object(m_FunctionArray.data(), m_FunctionArray.size());
        m_Allocator.deallocate_bytes(m_Writer.byte_rb.buffer.data(), m_Writer.byte_rb.buffer.size(), buffer_align);
        m_Allocator.deallocate_object(m_PositionArray.data(), m_PositionArray.size());
    }

    allocator_type get_allocator() const { return m_Allocator; }

    size_t buffer_size() const { return m_Writer.byte_rb.buffer.size(); }

    size_t max_functions() const { return m_FunctionArray.size() - 1; }

    size_t max_readers() const { return m_PositionArray.size(); }

    bool empty() const { return detail::empty<tb>(m_OutputPos, m_Writer.input_pos); }

    size_t count() const { return detail::count<tb>(m_OutputPos, m_Writer.input_pos, m_FunctionArray.size()); }

    void wait() const
        requires wait_interface
    {
        auto const output_pos = m_OutputPos.load(std::memory_order::relaxed);
        m_Writer.input_pos.wait(output_pos, std::memory_order::relaxed);
    }

    auto get_reader(size_t index) { return Reader{this, index}; }

    template<typename T>
    bool push(T &&callable) {
        return emplace<std::remove_cvref_t<T>>(fwd(callable));
    }


    template<typename Callable, typename... CArgs>
        requires detail::valid_callable<Callable, FSig, CArgs...>
    bool emplace(CArgs &&...args) {
        Index const pos = m_Writer.input_pos.load(std::memory_order::relaxed);
        auto const input_pos = detail::value<tb>(pos);
        auto const next_pos = (input_pos + 1) != m_FunctionArray.size() ? (input_pos + 1) : 0;
        auto ptr = detail::get_storage<Callable>(m_Writer.byte_rb);
        if (next_pos == m_Writer.output_pos or not ptr) {
            sync();
            ptr = detail::get_storage<Callable>(m_Writer.byte_rb);
            if (next_pos == m_Writer.output_pos or not ptr) return false;
        }
        auto const res = detail::emplace<Callable, FSig, opt>(ptr, fwd(args)...);
        m_FunctionArray[input_pos] = res.fd;
        detail::publish<tb>(m_Writer.input_pos, pos, next_pos, m_OutputPos);
        if constexpr (wait_interface) m_Writer.input_pos.notify_one();
        m_Writer.byte_rb.input_pos = static_cast<size_t>(res.next_pos - m_Writer.byte_rb.buffer.data());
        return true;
    }

private:
    void sync() {
        m_Writer.output_pos = detail::sync<tb>(m_Writer.output_pos, m_PositionArray, m_OutputPos);
        m_Writer.byte_rb.output_pos =
                m_Writer.output_pos != detail::value<tb>(m_Writer.input_pos)
                        ? static_cast<size_t>(m_FunctionArray[m_Writer.output_pos].obj - m_Writer.byte_rb.buffer.data())
                        : m_Writer.byte_rb.input_pos;
    }

    using RingBuffer = detail::RingBuffer<detail::FData<FSig, opt>>;
    using Index = uint64_t;
    static constexpr size_t tb = 16;
    struct alignas(rb::hardware_destructive_interference_size) {
        std::atomic<Index> input_pos{};
        size_t output_pos{};
        detail::RingBuffer<std::byte> byte_rb;
    } m_Writer;
    alignas(rb::hardware_destructive_interference_size) std::atomic<Index> m_OutputPos{};
    std::span<detail::FData<FSig, opt>> const m_FunctionArray;
    std::span<rb::CacheAligned<std::atomic<size_t>>> const m_PositionArray;
    allocator_type m_Allocator;
};
}// namespace rb

#endif
