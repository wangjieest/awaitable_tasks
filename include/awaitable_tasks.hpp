#pragma once
#include <experimental/resumable>
#include <memory>
#include <cassert>

#if defined(AWAITABLE_TASKS_VARIANT_MPARK)
#include "mpark/include/mpark/variant.hpp"
#define NS_VARIANT mpark
#elif defined(AWAITABLE_TASKS_VARIANT_STD)
#include <variant>
#define NS_VARIANT std
#endif

#pragma pack(push)
#pragma pack(8)
namespace awaitable_tasks {
namespace ex = std::experimental;
template<typename T = void>
using coroutine = ex::coroutine_handle<T>;
template<typename>
class task;

namespace detail {
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
    bool operator==(const Unkown& /*other*/) const { return true; }
    bool operator!=(const Unkown& /*other*/) const { return false; }
};

template<typename T>
struct isTaskOrRet : std::false_type {
    using Inner = typename Unkown::Void_To_Unkown<T>;
};

template<typename T>
struct isTaskOrRet<task<T>> : std::true_type {
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

template<typename F, typename T = Unkown>
struct CallArgsWith {
    using CallableInfo =
        typename std::conditional_t<is_callable_v<F()>,
                        callable_traits<F()>,
                        typename std::conditional_t<is_callable_v<F(T)>,
                                        callable_traits<F(T)>,
                                        typename std::conditional_t<is_callable_v<F(T&&)>,
                                                        callable_traits<F(T&&)>,
                                                        callable_traits<F(T&)>>>>;

    using TaskOrRet = typename isTaskOrRet<typename CallableInfo::result_type>;
    using Return = typename TaskOrRet::Inner;
    using TaskReturn = task<typename TaskOrRet::Inner>;
    using OrignalRet = typename CallableInfo::result_type;
};
}

struct promise_base {
    promise_base* _prev = nullptr;
    promise_base* _next = nullptr;
    coroutine<> _coro = nullptr;
    void remove_from_list() noexcept {
        if (_prev)
            _prev->_next = _next;
        if (_next)
            _next->_prev = _prev;
        reset();
    }
    void insert_after(promise_base* target) noexcept {
        assert(!_next);
        _prev = target;
        _next = target->_next;
        if (_next)
            _next->_prev = this;
        target->_next = this;
    }
    void insert_before(promise_base* target) noexcept {
        assert(!_prev);
        _next = target;
        _prev = target->_prev;
        if (_prev)
            _prev->_next = this;
        target->_prev = this;
    }
    void replace(promise_base* target) noexcept {
        assert(!_prev && !_next);
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
    promise_handle() : _result(std::make_shared<T>()) {}
    promise_handle(const promise_handle& rhs) = delete;
    promise_handle& operator=(const promise_handle& rhs) = delete;
    promise_handle(promise_handle&& rhs) = default;
    promise_handle& operator=(promise_handle&& rhs) = default;
    template<typename U>
    void set_value(U&& value) {
        *_result.get() = std::forward<U>(value);
        resume();
    }
    void set_exception(std::exception_ptr eptr) {
        auto coro = static_cast<coroutine<task<T>::promise_type>*>(&_base.prev()->_coro);
        coro->promise().set_eptr(std::move(eptr));
        resume();
    }

    auto get_task() {
        auto result = _result;
        auto t = [](std::shared_ptr<T> value) -> task<T> {
            co_await ex::suspend_always{};
            return *(value.get());
        }(std::move(result));
        _base.insert_after(&t._coro.promise());
        return std::move(t);
    }

    bool resume() {
        if (promise_base::is_resumable(_base.prev())) {
            _base.prev()->_coro.resume();
            destroy();
            return true;
        }
        return false;
    }
    void operator()() { resume(); }

    void destroy() noexcept {
        if (_base.prev()) {
            promise_base::destroy_chain(_base.prev(), false);
            _base.reset();
        }
    }
    ~promise_handle() noexcept { destroy(); }

    // refer to zero-cost impl https://gist.github.com/GorNishanov/65195f6e5620f70721597caf920d4dcc
    // TODO remove shared_ptr for value.
    struct await_type {
        awaitable_tasks::promise_base handle;
        std::shared_ptr<T> value;
        await_type() = default;
        await_type(const await_type&) = delete;
        await_type& operator=(const await_type&) = delete;
        await_type(await_type&&) = default;
        await_type& operator=(await_type&&) = default;

        bool await_ready() { return false; }
        template<typename P>
        void await_suspend(awaitable_tasks::coroutine<P> caller_coro) {
            caller_coro.promise().insert_before(handle.next());
            handle.remove_from_list();
        }
        T await_resume() { return std::move(*value.get()); }
    };

    await_type make_awaiter() {
        await_type await_obj;
        await_obj.value = _result;
        await_obj.handle.insert_before(&_base);
        return std::move(await_obj);
    }

    promise_base& get_base() { return _base; }
    auto get_result() { return _result; }

  protected:
    promise_base _base;
    std::shared_ptr<T> _result;
};

template<typename T>
class task {
  public:
    class promise_type : public promise_base {
      public:
        using result_type = T;
        promise_type& get_return_object() noexcept { return *this; }
        auto initial_suspend() noexcept { return ex::suspend_never{}; }
        auto final_suspend() noexcept {
            struct final_awaiter {
                promise_type* me;
                bool await_ready() noexcept { return false; }
                void await_suspend(coroutine<>) noexcept {
                    if (me->prev() && me->prev()->_coro)
                        me->prev()->_coro();
                }
                void await_resume() noexcept {}
            };
            return final_awaiter{this};
        }

        template<typename U>
        void return_value(U&& value) {
            result_ = std::forward<U>(value);
        }
        void uncought_exceptions() { set_eptr(std::current_exception()); }
        void set_exception(std::exception_ptr eptr) noexcept { set_eptr(std::move(eptr)); }

#if defined(NS_VARIANT)
        void set_eptr(std::exception_ptr eptr) noexcept { result_ = std::move(eptr); }
#else
        void set_eptr(std::exception_ptr eptr) noexcept { eptr_ = std::move(eptr); }
#endif
        template<typename U>
        void set_value(U&& value) {
            result_ = std::move(value);
        }

        void throw_if_exception() const {
#if defined(NS_VARIANT)
            if (NS_VARIANT::get_if<std::exception_ptr>(&result_))
                std::rethrow_exception(NS_VARIANT::get<std::exception_ptr>(result_));
            else if (NS_VARIANT::get_if<monostate>(&result_))
                throw std::runtime_error("value not returned");
#else
            if (eptr_)
                std::rethrow_exception(eptr_);
#endif
        }

        auto& get_result() { return result_; }
#if defined(NS_VARIANT)
        struct monostate {};
        NS_VARIANT::variant<monostate, result_type, std::exception_ptr> result_;
#else
        std::exception_ptr eptr_ = nullptr;
        result_type result_{};
#endif
    };
    bool await_ready() noexcept { return is_done_or_empty(); }
    template<typename P>
    void await_suspend(coroutine<P> caller_coro) noexcept {
        // without promise_handle control ,will leak
        assert(_coro.promise().next() || _coro.promise().prev());
        caller_coro.promise().insert_before(&_coro.promise());
    }
    T await_resume() {
        _coro.promise().throw_if_exception();
#if defined(NS_VARIANT)
        return std::move(NS_VARIANT::get<T>(_coro.promise().get_result()));
#else
        return std::move(_coro.promise().get_result());
#endif
    }

    explicit task(promise_type& prom) noexcept
        : _coro(coroutine<promise_type>::from_promise(prom)) {
        prom._coro = _coro;
    }

    task() = default;
    task(task const&) = delete;
    task& operator=(task const&) = delete;
    task(task&& rhs) noexcept : _coro(rhs._coro) { rhs._coro = nullptr; }
    task& operator=(task&& rhs) noexcept {
        if (this != std::addressof(rhs)) {
            _coro = rhs._coro;
            rhs._coro = nullptr;
        }
        return *this;
    }
    ~task() = default;

    void reset() noexcept {
        if (_coro) {
            promise_base* inner = &_coro.promise();
            while (inner->next())
                inner = inner->next();
            promise_base::destroy_chain(inner->prev(), true);
        }
    }

    bool is_valid() noexcept { return _coro != nullptr; }
    bool is_done_or_empty() noexcept { return _coro ? _coro.done() : true; }

  private:
    template<typename>
    friend class task;
    friend class task_holder;
    template<typename>
    friend class promise_handle;
    coroutine<promise_type> _coro = nullptr;

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

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 1 && R::TaskOrRet::value, typename R::TaskReturn>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto next_task = [](task t, std::decay_t<F> f) -> typename R::TaskReturn {
            auto&& value = co_await t;
            return co_await f(value);
        }
        (std::move(*this), std::move(func));
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 0 && R::TaskOrRet::value &&
                         (std::is_same_v<typename R::OrignalRet, detail::Unkown> ||
                             std::is_same_v<typename R::OrignalRet, void>),
            task>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto next_task = [](task t, std::decay_t<F> f) -> task {
            auto&& value = co_await t;
            return co_await f();
            return value;
        }(std::move(*this), std::move(func));
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 0 && R::TaskOrRet::value &&
                         (!std::is_same_v<typename R::OrignalRet, detail::Unkown> &&
                             !std::is_same_v<typename R::OrignalRet, void>),
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

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 1 && !R::TaskOrRet::value &&
                         std::is_same_v<typename R::OrignalRet, void>,
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
    std::enable_if_t<sizeof...(Args) == 1 && !R::TaskOrRet::value &&
                         !std::is_same_v<typename R::OrignalRet, void>,
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
    std::enable_if_t<sizeof...(Args) == 0 && !R::TaskOrRet::value &&
                         std::is_same_v<typename R::OrignalRet, void>,
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
    std::enable_if_t<sizeof...(Args) == 0 && !R::TaskOrRet::value &&
                         !std::is_same_v<typename R::OrignalRet, void>,
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
        if (t._coro) {
            _base.insert_before(&t._coro.promise());
        }
    }
    task_holder(task_holder&& rhs) = default;
    task_holder& operator=(task_holder&& rhs) = default;
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
}

#pragma region task helpers
// when_all range
#include <vector>
namespace awaitable_tasks {
namespace detail {
template<typename T>
struct when_all_range_context {
    using retrun_type = task<std::vector<T>>;
    promise_handle<detail::Unkown> handle;
    std::vector<T> results;
    size_t task_count = 0;
    std::vector<task<detail::Unkown>> tasks_holder;
};
}

// range when_all returns type of std::pair<size_t, T>
template<typename InputIterator,
    typename T =
        typename detail::isTaskOrRet<std::iterator_traits<InputIterator>::value_type>::Inner,
    typename Ctx = typename detail::when_all_range_context<T>>
typename Ctx::retrun_type when_all(InputIterator first, InputIterator last) {
    auto ctx = std::make_shared<Ctx>();
    const size_t all_task_count = std::distance(first, last);
    ctx->task_count = all_task_count;
    ctx->results.resize(all_task_count);
    ctx->tasks_holder.reserve(all_task_count);
    using task_type = typename detail::isTaskOrRet<T>::Inner;
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
    return ctx->handle.get_task().then([ctx] { return std::move(ctx->results); });
}
}

// when_n/when_any
namespace awaitable_tasks {
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
}

// when_n returns type of std::vector<std::pair<size_t, T>>
template<typename InputIterator,
    typename T =
        typename detail::isTaskOrRet<std::iterator_traits<InputIterator>::value_type>::Inner,
    typename Ctx = typename detail::when_n_range_context<T>>
typename Ctx::retrun_type when_n(InputIterator first, InputIterator last, size_t n = 0) {
    auto ctx = std::make_shared<Ctx>();
    const size_t all_task_count = std::distance(first, last);
    n = ((n && n < all_task_count) ? n : all_task_count);
    ctx->task_count = n;
    ctx->tasks_holder.reserve(n);
    using task_type = typename detail::isTaskOrRet<T>::Inner;
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
    return ctx->handle.get_task().then([ctx] { return std::move(ctx->results); });
}

// when_any returns type of std::pair<size_t, T>
template<typename InputIterator,
    typename T =
        typename detail::isTaskOrRet<std::iterator_traits<InputIterator>::value_type>::Inner,
    typename Pair = std::pair<size_t, T>>
task<Pair> when_any(InputIterator first, InputIterator last) {
    return when_n(first, last, 1).then([](std::vector<Pair>& vec) -> Pair { return vec[0]; });
}
}

// when_all variadic/zip
#include <tuple>
#include <array>
namespace awaitable_tasks {
namespace detail {
template<typename... Ts>
struct when_variadic_context {
    using result_type = std::tuple<std::decay_t<typename isTaskOrRet<Ts>::Inner>...>;
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
        task_transform(ts, [ctx](typename detail::isTaskOrRet<Ts>::Inner a) -> Unkown {
            ctx->set_variadic_result<Is>(a);
            return Unkown{};
        })...};
    return ctx->handle.get_task().then([ctx] { return std::move(ctx->results); });
}
}

template<typename T, typename... Ts>
auto when_all(task<T>& t, Ts&... ts) {
    return detail::when_variant_impl(sizeof...(Ts) + 1,
                    std::make_index_sequence<sizeof...(Ts) + 1>{},
                    t,
                    ts...);
}
#if defined(NS_VARIANT)
template<typename T, typename... Ts>
auto when_any(task<T>& t, Ts&... ts) {
    return detail::when_variant_impl(1, std::make_index_sequence<sizeof...(Ts) + 1>{}, t, ts...);
}
template<typename T, typename... Ts>
auto when_n(size_t N, task<T>& t, Ts&... ts) {
    return detail::when_variant_impl(N, std::make_index_sequence<sizeof...(Ts) + 1>{}, t, ts...);
}
#endif
}
#pragma endregion

#pragma pack(pop)
