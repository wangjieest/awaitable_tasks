#ifndef AWAITABLE_TASKS_H
#define AWAITABLE_TASKS_H

#pragma once
#include <memory>
#include <experimental/resumable>

#define AWAITABLE_TASKS_TRACE_COROUTINE
#ifdef AWAITABLE_TASKS_TRACE_COROUTINE
#define AWAITABLE_TASKS_TRACE(fmt, ...) printf("\n" fmt "\n", ##__VA_ARGS__)
__declspec(selectany) uint32_t g_frame_count = 0;
#endif

#if !defined(_HAS_CXX17) || !_HAS_CXX17
#include "mpark/variant.hpp"
#define NS_VARIANT mpark
#else
#include <variant>
#define NS_VARIANT std
#endif
#define AWAITTASK_ASSERT _ASSERTE

#pragma pack(push, 4)
namespace awaitable {
namespace ex = std::experimental;
template<typename T = void>
using coroutine = ex::coroutine_handle<T>;
template<typename>
class task;

namespace detail {
struct mono_state_t {};
template<typename T>
struct FunctionReferenceToPointer {
    using type = T;
};

template<typename R, typename... Args>
struct FunctionReferenceToPointer<R (&)(Args...)> {
    using type = R (*)(Args...);
};

struct Unkown {
    template<typename T>
    using Void_To_Unkown = std::conditional_t<std::is_same_v<T, void>, Unkown, T>;
    constexpr bool operator==(const Unkown& /*other*/) const { return true; }
    constexpr bool operator!=(const Unkown& /*other*/) const { return false; }
};

template<typename T>
struct IsTaskOrRet : std::false_type {
    using Inner = typename Unkown::Void_To_Unkown<T>;
};

template<typename T>
struct IsTaskOrRet<task<T>> : std::true_type {
    using Inner = T;
};

template<typename, typename R = void>
struct is_callable;
template<typename F, typename... Args, typename R>
struct is_callable<F(Args...), R> {
    template<typename T, typename = std::result_of_t<T(Args...)>>
    static constexpr std::true_type check(std::nullptr_t) {
        return {};
    };
    template<typename>
    static constexpr std::false_type check(...) {
        return {};
    };
    static constexpr bool value = decltype(check<F>(nullptr))::value;
};

template<typename T, typename R = void>
constexpr bool is_callable_v = is_callable<T, R>::value;

template<typename T>
struct callable_traits;

template<typename F, typename... Args>
struct callable_traits<F(Args...)> {
    using result_type = std::result_of_t<F(Args...)>;
};

template<typename F, typename T>
struct CallArgsWith {
    using CallableInfo = std::conditional_t<is_callable_v<F()>,
                                callable_traits<F()>,
                                std::conditional_t<is_callable_v<F(T)>,
                                        callable_traits<F(T)>,
                                        std::conditional_t<is_callable_v<F(T&&)>,
                                                callable_traits<F(T&&)>,
                                                callable_traits<F(T&)>>>>;

    using TaskOrRet = typename IsTaskOrRet<typename CallableInfo::result_type>;
    enum { is_task = TaskOrRet::value };
    using Return = typename TaskOrRet::Inner;
    using TaskReturn = task<typename TaskOrRet::Inner>;
    using OrignalRet = typename CallableInfo::result_type;
};
template<typename F>
struct CallArgsWith<F, void> {
    using CallableInfo = typename callable_traits<F()>;
    using TaskOrRet = typename IsTaskOrRet<typename CallableInfo::result_type>;
    enum { is_task = TaskOrRet::value };
    using Return = typename TaskOrRet::Inner;
    using TaskReturn = task<typename TaskOrRet::Inner>;
    using OrignalRet = typename CallableInfo::result_type;
};

template<class TOut, class TIn>
union horrible_union {
    TOut out;
    TIn in;
};
template<class TOut, class TIn>
inline TOut horrible_cast(TIn mIn) noexcept {
    horrible_union<TOut, TIn> u;
    static_assert(sizeof(TIn) == sizeof(u) && sizeof(TIn) == sizeof(TOut),
        "cannot use horrible_cast<>");
    u.in = mIn;
    return u.out;
}
}  // namespace detail

struct promise_base {
    promise_base* _prev = nullptr;
    promise_base* _next = nullptr;
    coroutine<> _coro = nullptr;
    void* _data = nullptr;

    void remove_from_list(bool clear = true) noexcept {
        if (_prev)
            _prev->_next = _next;
        if (_next)
            _next->_prev = _prev;
        if (clear)
            reset();
    }
    void insert_after(promise_base* target) noexcept {
        AWAITTASK_ASSERT(!_next);
        _prev = target;
        _next = target->_next;
        if (_next)
            _next->_prev = this;
        target->_next = this;
    }
    void insert_before(promise_base* target) noexcept {
        AWAITTASK_ASSERT(!_prev);
        _next = target;
        _prev = target->_prev;
        if (_prev)
            _prev->_next = this;
        target->_prev = this;
    }
    void replace(promise_base* target) noexcept {
        AWAITTASK_ASSERT(!_prev && !_next);
        std::swap(_prev, target->_prev);
        std::swap(_next, target->_next);
        if (_next)
            _next->_prev = this;
        if (_prev)
            _prev->_next = this;
    }
    promise_base* prev() noexcept { return _prev; }
    promise_base* next() noexcept { return _next; }
    void reset() noexcept {
        _prev = nullptr;
        _next = nullptr;
        _coro = nullptr;
        _data = nullptr;
    }
    static bool is_valid(promise_base* coro_base) noexcept {
        return coro_base && coro_base->_coro && coro_base->_coro;
    }
    static bool is_resumable(promise_base* coro_base) noexcept {
        return is_valid(coro_base) && !coro_base->_coro.done();
    }
    static void destroy_chain(promise_base* target, bool force) {
        if (target) {
            auto outer = target->prev();
            auto coro = target->_coro;
            if (coro && (force || coro.done())) {
                target->remove_from_list();
                destroy_chain(outer, force);
                coro.destroy();
            }
        }
    }
    promise_base() = default;
    promise_base(const promise_base&) = delete;
    promise_base& operator=(const promise_base&) = delete;
    promise_base(promise_base&& rhs) noexcept {
        replace(&rhs);
        std::swap(_coro, rhs._coro);
    }
    promise_base& operator=(promise_base&& rhs) noexcept {
        if (this != std::addressof(rhs)) {
            replace(&rhs);
            std::swap(_coro, rhs._coro);
        }
        return *this;
    }
};

template<typename T>
class promise_handle {
  public:
    using value_type = typename detail::IsTaskOrRet<T>::Inner;
    struct shared_state : public promise_base {
        NS_VARIANT::variant<value_type, std::exception_ptr> value;
        ~shared_state() {
            if (prev()) {
                promise_base::destroy_chain(prev(), true);
            }
        }
    };
    std::shared_ptr<shared_state> _state = std::make_shared<shared_state>();

    template<typename U>
    void set_value(U&& value) {
        if (_state) {
            _state->value = std::forward<U>(value);
        }
    }
    void set_exception(std::exception_ptr eptr) {
        if (_state) {
            _state->value = eptr;
        }
    }
    bool resume() {
        if (_state && promise_base::is_resumable(_state->prev())) {
            auto coro = _state->prev()->_coro;
            _state->remove_from_list();
            coro.resume();
            return true;
        }
        return false;
    }

    struct await_type {
        shared_state* _state;
        bool await_ready() {
            AWAITTASK_ASSERT(!_state || !_state->prev());  // can only be awaited once
            return false;
        }
        template<typename P>
        void await_suspend(awaitable::coroutine<P> caller_coro) {
            caller_coro.promise().insert_before(_state);
        }

        auto await_resume() {
            auto& val = _state->value;
            if (NS_VARIANT::get_if<std::exception_ptr>(&val))
                std::rethrow_exception(NS_VARIANT::get<std::exception_ptr>(val));
            return NS_VARIANT::get<value_type>(val);
        }
    };

    auto get_awaitable() { return await_type{_state.get()}; }
    auto get_task() {
        return [](await_type awaiter) -> task<T> {
            return co_await awaiter;
        }(std::move(get_awaitable()));
    }
};

template<typename T>
class task {
  public:
    using value_type = typename detail::IsTaskOrRet<T>::Inner;
    class promise_type : public promise_base {
      public:
        using result_type = typename value_type;
        promise_type& get_return_object() noexcept { return *this; }
        bool initial_suspend() const { return false; }
        bool final_suspend() noexcept {
            auto coro = prev() ? prev()->_coro : nullptr;
            remove_from_list();
            if (coro) {
                coro.resume();
            }
            return false;
        }
        template<typename U>
        void return_value(U&& value) noexcept {
            result_ = std::forward<U>(value);
        }
        template<typename U>
        void set_value(U&& value) {
            result_ = std::move(value);
        }
        // auto catch
        void set_exception(std::exception_ptr eptr) noexcept { set_eptr(std::move(eptr)); }
        void set_eptr(std::exception_ptr eptr) noexcept { result_ = std::move(eptr); }
        void throw_if_exception() const {
            if (NS_VARIANT::get_if<std::exception_ptr>(&result_))
                std::rethrow_exception(NS_VARIANT::get<std::exception_ptr>(result_));
            else if (NS_VARIANT::get_if<detail::mono_state_t>(&result_))
                throw std::runtime_error("value not returned");
        }

        auto& get_result() noexcept { return result_; }
        NS_VARIANT::variant<detail::mono_state_t, result_type, std::exception_ptr> result_;
#ifdef AWAITABLE_TASKS_TRACE_COROUTINE
        using alloc_of_char_type = std::allocator<char>;
        void* operator new(size_t size) {
            alloc_of_char_type al;
            auto ptr = al.allocate(size);
            AWAITABLE_TASKS_TRACE("promise created %p %u", ptr, ++g_frame_count);
            return ptr;
        }
        void operator delete(void* ptr, size_t size) noexcept {
            alloc_of_char_type al;
            AWAITABLE_TASKS_TRACE("promise destroy %p %u", ptr, --g_frame_count);
            return al.deallocate(static_cast<char*>(ptr), size);
        }
#endif
    };
    using coroutine_type = coroutine<promise_type>;

    bool await_ready() noexcept { return false; }
    template<typename P>
    void await_suspend(coroutine<P> caller_coro) noexcept {
        AWAITTASK_ASSERT(get_coro().promise().next() || get_coro().promise().prev());
        caller_coro.promise().insert_before(&get_coro().promise());
    }
    auto await_resume() {
        get_coro().promise().throw_if_exception();
        return std::move(NS_VARIANT::get<value_type>(get_coro().promise().get_result()));
    }

    explicit task(promise_type& prom) noexcept {
        auto coro = coroutine<promise_type>::from_promise(prom);
        set_coro(coro);
        prom._coro = coro;
    }
    ~task() = default;
    task() = default;
    task(task const&) = delete;
    task& operator=(task const&) = delete;
    task(task&& rhs) noexcept : _addr(std::exchange(rhs._addr, nullptr)) {
        promise_base* prom = get_promise();
        if (prom)
            prom->_data = &_addr;
    }
    task& operator=(task&& rhs) noexcept {
        if (this != std::addressof(rhs)) {
            _addr = std::exchange(rhs._addr, nullptr);
        }
        return *this;
    }
    void reset() noexcept {
        promise_base* inner = get_promise();
        if (inner) {
            while (inner->next())
                inner = inner->next();
            promise_base::destroy_chain(inner->prev(), true);
        }
    }

    bool is_valid() noexcept { return get_coro() != nullptr; }

  private:
    template<typename>
    friend class task;
    friend class task_holder;
    template<typename>
    friend class promise_handle;
    promise_type* get_promise() {
        auto coro = get_coro();
        return coro ? &coro.promise() : nullptr;
    }
    void set_coro(coroutine_type coro) { _addr = detail::horrible_cast<decltype(_addr)>(coro); }
#if 1
    coroutine_type _addr = nullptr;
    coroutine_type get_coro() { return _addr; }
#else
    coroutine_type get_coro() { return detail::horrible_cast<coroutine_type>(_addr); }
    void* _addr = nullptr;
#endif

#pragma region then_impl
  public:
    template<typename F,
        typename FF = typename detail::FunctionReferenceToPointer<F>::type,
        typename C = typename detail::CallArgsWith<FF, T>>
    auto then(F&& func) {
        using Arguments = typename C::CallableInfo;
        return then_impl<FF, C>(std::forward<FF>(func), Arguments());
    }

    task<T> then() noexcept { return std::move(*this); }
    template<class Callback, class... Callbacks>
    auto then_multi(Callback&& fn, Callbacks&&... fns) noexcept {
        return then(std::forward<Callback>(fn)).then_multi(std::forward<Callbacks>(fns)...);
    }

    template<typename R, typename Caller, typename... Args>
    task<R> then(R (Caller::*memfunc)(Args&&...), Caller* caller) {
        return then([caller, memfunc](Args... args) { cal->*memfunc(st::forward<Args>(args)...); });
    }

  private:
    template<bool, typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) >= 2, void> then_impl(F&& func,
                                                    detail::callable_traits<F(Args...)>) noexcept {
        static_assert(false, "then must use zero/one param");
    }

#if defined(AWAITTASK_ENABLE_THEN_TASK)
    // then make a new task
    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 1 && R::is_task, typename R::TaskReturn>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto next_task = [](task t, std::decay_t<F> f) -> typename R::TaskReturn {
            auto&& value = co_await t;
            return co_await f(value);
        }
        (std::move(*this), std::move(func));
        return std::move(next_task);
    }
    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 0 && R::is_task &&
                         (std::is_same_v<typename R::OrignalRet, detail::Unkown> ||
                             std::is_void_v<typename R::OrignalRet>),
            task>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto next_task = [](task t, std::decay_t<F> f) -> task {
            auto&& value = co_await t;
            co_await f();
            return value;
        }(std::move(*this), std::move(func));
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 0 && R::is_task &&
                         (!std::is_same_v<typename R::OrignalRet, detail::Unkown> &&
                             !std::is_void_v<typename R::OrignalRet>),
            typename R::TaskReturn>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto next_task = [](task t, std::decay_t<F> f) -> typename R::TaskReturn {
            co_await t;
            auto ff = f();
            auto&& value = co_await ff;
            return value;
        }
        (std::move(*this), std::move(func));
        return std::move(next_task);
    }
#endif
    // then continue function
    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 1 && !R::is_task && std::is_void_v<typename R::OrignalRet>,
            task>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto next_task = [](task t, std::decay_t<F> f) -> task {
            auto&& value = co_await t;
            f(value);
            return value;
        }(std::move(*this), std::forward<F>(func));
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 1 && !R::is_task && !std::is_void_v<typename R::OrignalRet>,
            typename R::TaskReturn>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto next_task = [](task t, std::decay_t<F> f) -> typename R::TaskReturn {
            auto&& value = co_await t;
            return f(value);
        }
        (std::move(*this), std::forward<F>(func));
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 0 && !R::is_task && std::is_void_v<typename R::OrignalRet>,
            task>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto next_task = [](task t, std::decay_t<F> f) -> typename task {
            auto&& value = co_await t;
            f();
            return value;
        }
        (std::move(*this), std::forward<F>(func));
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 0 && !R::is_task && !std::is_void_v<typename R::OrignalRet>,
            typename R::TaskReturn>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto next_task = [](task t, std::decay_t<F> f) -> typename R::TaskReturn {
            co_await t;
            return f();
        }
        (std::move(*this), std::forward<F>(func));
        return std::move(next_task);
    }
#pragma endregion
};

class task_holder {
  public:
    task_holder() {}
    template<typename T>
    task_holder(task<T>&& t) {
        if (t.get_coro()) {
            _base.insert_before(&t.get_coro().promise());
        }
    }
    task_holder(task_holder&& rhs) noexcept = default;
    task_holder& operator=(task_holder&& rhs) noexcept = default;
    task_holder(const task_holder& rhs) = delete;
    task_holder& operator=(const task_holder&) = delete;
    void reset() noexcept {
        if (_base.next()) {
            promise_base* inner = _base.next();
            while (inner->next())
                inner = inner->next();
            promise_base::destroy_chain(inner->prev(), true);
            inner->reset();
        }
        _base.reset();
    }
    ~task_holder() noexcept { reset(); }

  private:
    promise_base _base;
};
}  // namespace awaitable

#pragma region task helpers
// when_all range
#include <vector>
namespace awaitable {
namespace detail {
template<typename T>
struct when_all_range_context {
    using retrun_type = task<std::vector<T>>;
    promise_handle<detail::Unkown> handle;
    std::vector<T> results;
    size_t task_count = 0;
    std::vector<task<detail::Unkown>> tasks_holder;
};
}  // namespace detail

// range when_all returns type of std::pair<size_t, T>
template<typename InputIterator,
    typename T =
        typename detail::IsTaskOrRet<std::iterator_traits<InputIterator>::value_type>::Inner,
    typename Ctx = typename detail::when_all_range_context<T>>
typename Ctx::retrun_type when_all(InputIterator first, InputIterator last) {
    auto ctx = std::make_shared<Ctx>();
    const size_t all_task_count = std::distance(first, last);
    ctx->task_count = all_task_count;
    ctx->results.resize(all_task_count);
    ctx->tasks_holder.reserve(all_task_count);
    using task_type = typename detail::IsTaskOrRet<T>::Inner;
    for (size_t idx = 0; first != last; ++idx, ++first) {
        ctx->tasks_holder.emplace_back((*first).then([ctx, idx](task_type& a) {
            auto& data = *ctx;
            if (data.task_count != 0) {
                data.results[idx] = std::move(a);
                if (--data.task_count == 0) {
                    data.handle.resume();
                }
            }
            return detail::Unkown{};
        }));
    }
    return ctx->handle.get_task().then([p{ctx.get()}] { return std::move(p->results); });
}

template<typename Range,
    typename T = typename detail::IsTaskOrRet<typename Range::value_type>::Inner,
    typename Ctx = typename detail::when_all_range_context<T>>
auto when_all(Range& range) -> typename Ctx::retrun_type {
    return when_all(std::begin(range), std::end(range));
}

// when_n/when_any
// decides which type when_n returns to std::vector<std::pair<size_t, T>>
namespace detail {
template<typename T>
struct when_n_range_context {
    using data_type = std::vector<std::pair<size_t, T>>;
    using retrun_type = task<data_type>;
    inline void set_result(size_t idx, T& data) { results.emplace_back(idx, std::move(data)); }
    promise_handle<detail::Unkown> handle;
    data_type results;
    size_t task_count = 0;
    std::vector<task<detail::Unkown>> tasks_holder;
};
}  // namespace detail

// when_n returns type of std::vector<std::pair<size_t, T>>
template<typename InputIterator,
    typename T =
        typename detail::IsTaskOrRet<std::iterator_traits<InputIterator>::value_type>::Inner,
    typename Ctx = typename detail::when_n_range_context<T>>
typename Ctx::retrun_type when_n(InputIterator first, InputIterator last, size_t N = 0) {
    auto ctx = std::make_shared<Ctx>();
    const size_t all_task_count = std::distance(first, last);
    N = ((N && N < all_task_count) ? N : all_task_count);
    ctx->task_count = N;
    ctx->tasks_holder.reserve(N);
    using task_type = typename detail::IsTaskOrRet<T>::Inner;
    for (size_t idx = 0; first != last; ++idx, ++first) {
        ctx->tasks_holder.emplace_back((*first).then([ctx, idx](task_type& a) {
            auto& data = *ctx;
            if (data.task_count != 0) {
                data.set_result(idx, a);
                if (--data.task_count == 0)
                    data.handle.resume();
            }
            return detail::Unkown{};
        }));
    }
    return ctx->handle.get_task().then([p{ctx.get()}] { return std::move(p->results); });
}

template<typename Range,
    typename T = typename detail::IsTaskOrRet<typename Range::value_type>::Inner,
    typename Ctx = typename detail::when_n_range_context<T>>
auto when_n(Range& range, size_t N = 0) -> typename Ctx::retrun_type {
    return when_n(std::begin(range), std::end(range), N);
}

// when_any returns type of std::pair<size_t, T>
template<typename InputIterator,
    typename T =
        typename detail::IsTaskOrRet<std::iterator_traits<InputIterator>::value_type>::Inner,
    typename Pair = std::pair<size_t, T>>
task<Pair> when_any(InputIterator first, InputIterator last) {
    return when_n(first, last, 1).then([](std::vector<Pair>& vec) -> Pair { return vec[0]; });
}

template<typename Range,
    typename T = typename detail::IsTaskOrRet<typename Range::value_type>::Inner,
    typename Pair = std::pair<size_t, T>>
auto when_any(Range& range) -> typename task<Pair> {
    return when_any(std::begin(range), std::end(range));
}
}  // namespace awaitable

// when_all variadic/zip
#include <tuple>
#include <array>
namespace awaitable {
namespace detail {
template<typename... Ts>
struct when_variadic_context {
    using result_type = std::tuple<std::decay_t<typename IsTaskOrRet<Ts>::Inner>...>;
    using task_type = task<result_type>;
    template<size_t I, typename T>
    inline void set_variadic_result(T& t) {
        if (task_count != 0) {
            std::get<I>(results) = std::move(t);
            if (--task_count == 0)
                handle.resume();
        }
    }
    promise_handle<detail::Unkown> handle;
    result_type results;
    size_t task_count = sizeof...(Ts);
    std::array<task<detail::Unkown>, sizeof...(Ts)> tasks_holder;
};
template<typename T, typename F>
inline auto task_transform(T& t, F&& f) {
    return t.then(std::move(f));
}
template<size_t... Is, typename... Ts, typename Ctx = when_variadic_context<Ts...>>
typename Ctx::task_type when_variant_impl(size_t N, std::index_sequence<Is...>, Ts&... ts) {
    auto ctx = std::make_shared<Ctx>();
    ctx->task_count = N < sizeof...(Ts) ? (N > 0 ? N : 1) : sizeof...(Ts);
    ctx->tasks_holder = {
        task_transform(ts, [ctx](typename detail::IsTaskOrRet<Ts>::Inner a) -> Unkown {
            ctx->set_variadic_result<Is>(a);
            return Unkown{};
        })...};
    return ctx->handle.get_task().then([p{ctx.get()}] { return std::move(p->results); });
}
}  // namespace detail

template<typename T, typename... Ts>
auto when_any(task<T>& t, task<Ts>&... ts) {
    return detail::when_variant_impl(1, std::make_index_sequence<sizeof...(Ts) + 1>{}, t, ts...);
}
template<typename T, typename... Ts>
auto when_n(size_t N, task<T>& t, task<Ts>&... ts) {
    return detail::when_variant_impl(N, std::make_index_sequence<sizeof...(Ts) + 1>{}, t, ts...);
}
template<typename T, typename... Ts>
auto when_all(task<T>& t, task<Ts>&... ts) {
    return detail::when_variant_impl(sizeof...(Ts) + 1,
                    std::make_index_sequence<sizeof...(Ts) + 1>{},
                    t,
                    ts...);
}
}  // namespace awaitable
#pragma endregion
#pragma pack(pop)
#endif  // !defined(AWAITABLE_TASKS_H)
