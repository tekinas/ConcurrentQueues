#ifndef OBJECTQUEUE_MCSP
#define OBJECTQUEUE_MCSP

#include "detail/rb_common.h"

namespace rb {
template<typename Obj, bool wait_interface>
    requires(std::is_object_v<Obj> and std::is_destructible_v<Obj>)
class ObjectQueueMCSP {
public:
    class Reader {
    public:
        template<bool check_once, bool release>
        bool consume(std::invocable<Obj &> auto &&functor) {
            auto const rp = detail::reserve_one<check_once, tb>(m_OQ->m_OutputPos, m_OQ->m_Writer.input_pos,
                                                                m_OQ->m_Buffer.size());
            if (not rp) return false;
            auto &obj = m_OQ->m_Buffer[rp->output_pos];
            std::invoke(fwd(functor), obj);
            std::destroy_at(&obj);
            if constexpr (release) detail::release_reader(m_OQ->m_PositionArray[m_Index], rp->next_output_pos);
            return true;
        }

        template<bool check_once>
        size_t consume_all(std::invocable<Obj &> auto &&functor) {
            auto const rp = detail::reserve_all<check_once, tb>(m_OQ->m_OutputPos, m_OQ->m_Writer.input_pos);
            return rp ? detail::invoke_and_destroy(functor, RingBuffer{.buffer = m_OQ->m_Buffer,
                                                                       .input_pos = rp->next_output_pos,
                                                                       .output_pos = rp->output_pos})
                      : 0;
        }

        template<bool check_once, bool release>
        size_t consume_n(std::invocable<Obj &> auto &&functor, size_t n) {
            auto const rp = detail::reserve_n<check_once, tb>(m_OQ->m_OutputPos, m_OQ->m_Writer.input_pos,
                                                              m_OQ->m_Buffer.size(), n);
            if (not rp) return 0;
            auto const nc = detail::invoke_and_destroy(functor, RingBuffer{.buffer = m_OQ->m_Buffer,
                                                                           .input_pos = rp->next_output_pos,
                                                                           .output_pos = rp->output_pos});
            if constexpr (release) detail::release_reader(m_OQ->m_PositionArray[m_Index], rp->next_output_pos);
            return nc;
        }

        ~Reader() { detail::release_reader(m_OQ->m_PositionArray[m_Index]); }

        Reader(Reader const &) = delete;

        Reader &operator=(Reader const &) = delete;

    private:
        explicit Reader(ObjectQueueMCSP *oq, size_t i) : m_OQ{oq}, m_Index{i} {
            detail::set_reader<tb>(m_OQ->m_PositionArray[m_Index], m_OQ->m_OutputPos);
        }

        friend ObjectQueueMCSP;

        ObjectQueueMCSP *m_OQ;
        size_t m_Index;
    };

    explicit ObjectQueueMCSP(size_t buffer_size, size_t max_readers, allocator_type allocator = {})
        : m_Buffer{allocator.allocate_object<Obj>(buffer_size + 1), buffer_size + 1},
          m_PositionArray{allocator.allocate_object<rb::CacheAligned<std::atomic<size_t>>>(max_readers), max_readers},
          m_Allocator{allocator} {
        detail::init_readers(m_PositionArray);
    }

    ~ObjectQueueMCSP() {
        detail::destroy_non_consumed(RingBuffer{.buffer = m_Buffer,
                                                .input_pos = detail::value<tb>(m_Writer.input_pos),
                                                .output_pos = detail::value<tb>(m_OutputPos)});
        m_Allocator.deallocate_object(m_Buffer.data(), m_Buffer.size());
        m_Allocator.deallocate_object(m_PositionArray.data(), m_PositionArray.size());
    }

    allocator_type get_allocator() const { return m_Allocator; }

    size_t capacity() const { return m_Buffer.size() - 1; }

    size_t max_readers() const { return m_PositionArray.size(); }

    bool empty() const { return detail::empty<tb>(m_OutputPos, m_Writer.input_pos); }

    size_t count() const { return detail::count<tb>(m_OutputPos, m_Writer.input_pos, m_Buffer.size()); }

    void wait() const
        requires wait_interface
    {
        auto const output_pos = m_OutputPos.load(std::memory_order::relaxed);
        m_Writer.input_pos.wait(output_pos, std::memory_order::relaxed);
    }

    auto get_reader(size_t index) { return Reader{this, index}; }

    bool push(Obj const &obj) { return emplace(obj); }

    bool push(Obj &&obj) { return emplace(std::move(obj)); }

    template<typename... Args>
        requires std::is_constructible_v<Obj, Args...>
    bool emplace(Args &&...args) {
        Index const pos = m_Writer.input_pos.load(std::memory_order::relaxed);
        auto const input_pos = detail::value<tb>(pos);
        auto const next_pos = (input_pos + 1) != m_Buffer.size() ? (input_pos + 1) : 0;
        if (next_pos == m_Writer.output_pos) {
            m_Writer.output_pos = detail::sync<tb>(m_Writer.output_pos, m_PositionArray, m_OutputPos);
            if (next_pos == m_Writer.output_pos) return false;
        }
        std::construct_at(&m_Buffer[input_pos], fwd(args)...);
        detail::publish<tb>(m_Writer.input_pos, pos, next_pos, m_OutputPos);
        if constexpr (wait_interface) m_Writer.input_pos.notify_one();
        return true;
    }

    template<typename Functor>
        requires std::is_invocable_r_v<size_t, Functor, std::span<Obj>>
    size_t emplace_n(Functor &&functor) {
        Index const pos = m_Writer.input_pos.load(std::memory_order::relaxed);
        auto const input_pos = detail::value<tb>(pos);
        auto n_avl = detail::count_avl(m_Writer.output_pos, input_pos, m_Buffer.size());
        if (not n_avl) {
            m_Writer.output_pos = detail::sync<tb>(m_Writer.output_pos, m_PositionArray, m_OutputPos);
            n_avl = detail::count_avl(m_Writer.output_pos, input_pos, m_Buffer.size());
            if (not n_avl) return 0;
        }
        size_t const obj_emplaced = std::invoke(fwd(functor), m_Buffer.subspan(input_pos, n_avl));
        auto const next_pos = input_pos + obj_emplaced;
        detail::publish<tb>(m_Writer.input_pos, pos, next_pos != m_Buffer.size() ? next_pos : 0, m_OutputPos);
        if constexpr (wait_interface)
            obj_emplaced == 1 ? m_Writer.input_pos.notify_one() : m_Writer.input_pos.notify_all();
        return obj_emplaced;
    }

private:
    using RingBuffer = detail::RingBuffer<Obj>;
    using Index = uint64_t;
    static constexpr size_t tb = 16;
    struct alignas(rb::hardware_destructive_interference_size) {
        std::atomic<Index> input_pos{};
        size_t output_pos{};
    } m_Writer;
    alignas(rb::hardware_destructive_interference_size) std::atomic<Index> m_OutputPos{};
    std::span<Obj> const m_Buffer;
    std::span<rb::CacheAligned<std::atomic<size_t>>> const m_PositionArray;
    allocator_type m_Allocator;
};
}// namespace rb

#endif
