#ifndef FQ_COMMON
#define FQ_COMMON

#include "rb_common.h"

namespace rb {
enum class FQOpt { InvokeOnce, InvokeOnceDNI, InvokeMultiple };
}

namespace rb::detail {
template<typename>
struct IFPtr;

template<typename R, typename... Args>
struct IFPtr<R(Args...)> {
    using type = R (*)(void *, Args...);
};

using DFPtr = void (*)(void *);

template<typename Callable, typename FSig>
constexpr bool is_invocable = []<typename R, typename... Args>(R (*)(Args...)) {
    return std::is_invocable_r_v<R, Callable, Args...>;
}(std::add_pointer_t<FSig>{});

template<typename Callable, typename FSig, typename... CArgs>
concept valid_callable = std::is_object_v<Callable> and std::is_constructible_v<Callable, CArgs...> and
                         std::is_destructible_v<Callable> and is_invocable<Callable, FSig>;

template<typename Callable>
concept empty_callable = std::is_empty_v<Callable> and std::is_trivially_default_constructible_v<Callable> and
                         std::is_trivially_destructible_v<Callable>;

template<typename, typename>
class FunctionPtrs;

template<typename Callable, typename R, typename... Args>
class FunctionPtrs<Callable, R(Args...)> {
private:
    static R invoke_and_destroy(void *data, Args... args) {
        if constexpr (empty_callable<Callable>) return std::invoke(Callable{}, fwd(args)...);
        else {
            auto const ptr = static_cast<Callable *>(data);
            ScopeGaurd _ = [&] { std::destroy_at(ptr); };
            return std::invoke(*ptr, fwd(args)...);
        }
    }

    static R invoke(void *data, Args... args) {
        if constexpr (empty_callable<Callable>) return std::invoke(Callable{}, fwd(args)...);
        else return std::invoke(*static_cast<Callable *>(data), fwd(args)...);
    }

    static void destroy(void *data) { std::destroy_at(static_cast<Callable *>(data)); }

public:
    static constexpr auto indfptr = &invoke_and_destroy;
    static constexpr auto ifptr = &invoke;
    static constexpr auto dfptr = &destroy;
};

template<typename FSig, FQOpt opt>
struct FData {
    std::byte *obj;
    IFPtr<FSig>::type fptr;
    struct Empty {};
    [[no_unique_address]] std::conditional_t<opt == FQOpt::InvokeOnce, Empty, DFPtr> dfptr;
};

template<typename FSig, FQOpt opt>
class Function {
public:
    template<typename... Args>
        requires std::invocable<FSig, Args...>
    decltype(auto) operator()(Args &&...args) {
        ScopeGaurd _ = [&] {
            if constexpr (opt == FQOpt::InvokeOnceDNI) m_FD = nullptr;
        };
        return std::invoke(m_FD->fptr, m_FD->obj, fwd(args)...);
    }

    ~Function() {
        if constexpr (opt == FQOpt::InvokeOnceDNI)
            if (not m_FD) return;
        if (m_FD->dfptr) std::invoke(m_FD->dfptr, m_FD->obj);
    }

    ~Function()
        requires(opt == FQOpt::InvokeOnce)
    = default;

    Function(Function const &) = delete;

    Function &operator=(Function const &) = delete;

    explicit Function(FData<FSig, opt> const *fi) : m_FD{fi} {}

    Function(std::nullptr_t) = delete;

private:
    FData<FSig, opt> const *m_FD;
};

template<typename Func, typename FSig, FQOpt opt>
concept Consumer = requires(Func &&func, FData<FSig, opt> const &fd) { fwd(func)(Function{&fd}); };

template<typename FSig, FQOpt opt>
struct EmplaceResult {
    FData<FSig, opt> fd;
    std::byte *next_pos;
};

template<typename Callable>
inline std::byte *get_storage(RingBuffer<std::byte> const &rb) {
    if constexpr (not empty_callable<Callable>) {
        auto const buffer = get_storage(rb, sizeof(Callable), alignof(Callable));
        return buffer.empty() ? nullptr : buffer.data();
    } else return rb.buffer.data() + rb.input_pos;
}

template<typename Callable, typename FSig, FQOpt opt>
EmplaceResult<FSig, opt> emplace(std::byte *ptr, auto &&...cargs) {
    EmplaceResult<FSig, opt> res;
    res.fd.obj = ptr;
    res.next_pos = ptr;
    if constexpr (not empty_callable<Callable>) {
        std::construct_at(reinterpret_cast<Callable *>(ptr), fwd(cargs)...);
        res.next_pos += sizeof(Callable);
    }
    using FPtrs = FunctionPtrs<Callable, FSig>;
    res.fd.fptr = opt == FQOpt::InvokeMultiple ? FPtrs::ifptr : FPtrs::indfptr;
    if constexpr (opt != FQOpt::InvokeOnce)
        res.fd.dfptr = std::is_trivially_destructible_v<Callable> ? nullptr : FPtrs::dfptr;
    return res;
}

template<typename FSig, FQOpt opt, typename F>
constexpr decltype(auto) invoke(F &&func, FData<FSig, opt> const &fd) {
    return fwd(func)(Function{&fd});
}

template<typename FSig, FQOpt opt>
constexpr size_t invoke(auto &func, RingBuffer<FData<FSig, opt>> const &rb) {
    return detail::apply([&](FData<FSig, opt> const &fd) { return detail::invoke(func, fd); }, rb);
}

template<typename FSig, FQOpt opt>
constexpr void destroy_non_consumed(RingBuffer<FData<FSig, opt>> const &rb) {
    auto destroy = [](auto const &fd) {
        if (fd.dfptr) std::invoke(fd.dfptr, fd.obj);
    };
    if (rb.output_pos == rb.input_pos) return;
    if (rb.output_pos > rb.input_pos) {
        std::ranges::for_each(rb.buffer.subspan(rb.output_pos), destroy);
        std::ranges::for_each(rb.buffer.first(rb.input_pos), destroy);
    } else std::ranges::for_each(rb.buffer.subspan(rb.output_pos, rb.input_pos - rb.output_pos), destroy);
}
}// namespace rb::detail

#endif
