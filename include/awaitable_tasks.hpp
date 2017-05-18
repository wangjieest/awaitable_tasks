#pragma once
#include <experimental/resumable>
#include <memory>

#if 1
#define AWAITABLE_TASKS_TRACE(fmt, ...) printf("\n" fmt "\n", ##__VA_ARGS__)
#else
#define AWAITABLE_TASKS_TRACE(fmt, ...)
#endif

#define AWAITABLE_TASKS_CAPTURE_EXCEPTION

#define AWAITABLE_TASKS_VARIANT_MPARK
#if defined(AWAITABLE_TASKS_VARIANT_MAPBOX)
#include "mapbox/variant.hpp"
#elif defined(AWAITABLE_TASKS_VARIANT_MPARK)
#include "mpark/include/mpark/variant.hpp"
#elif defined(AWAITABLE_TASKS_VARIANT_STD)
#include <variant>
#endif

#pragma pack(push)
#ifdef _WIN64
#pragma pack(8)
#else
#pragma pack(8)
#endif
namespace awaitable_tasks {
#if defined(AWAITABLE_TASKS_VARIANT_STD)
namespace ns_variant = std;
#elif defined(AWAITABLE_TASKS_VARIANT_MPARK)
namespace ns_variant = mpark;
#endif
template<typename T = void>
using coroutine = std::experimental::coroutine_handle<T>;
namespace ex = std::experimental;
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

struct shared_state : public coroutine<> {
    shared_state(coroutine<> coro) { *static_cast<coroutine<>*>(this) = coro; }

    bool is_release_by_task() { return release_by_task_; }
    void set_release_by_task(bool b) {
        release_by_task_ = b;
        if (next_)
            next_->set_release_by_task(b);
    }
    template<typename>
    friend class task;
    ~shared_state() = default;

    bool valid() { return address() != nullptr; }
    void destroy_state() {
        if (valid())
            destroy();
        *static_cast<coroutine<>*>(this) = nullptr;
    }
    void destroy_state_chain() { recursive_destroy(this); }

  protected:
    static void recursive_destroy(shared_state* target) {
        if (target) {
            if (target->next_)
                recursive_destroy(target->next_.get());
            if (!target->is_release_by_task())
                target->destroy_state();
        }
    }

  protected:
    std::shared_ptr<shared_state> next_;
    bool release_by_task_ = true;
};

template<typename T = void>
class promise_handle;

template<>
class promise_handle<void> {
  public:
    promise_handle() = default;
    template<typename V>
    promise_handle(const promise_handle<V>& rhs) noexcept {
        ctb_ = rhs.ctb_;
    }
    template<typename V>
    promise_handle& operator=(const promise_handle<V>& rhs) noexcept {
        ctb_ = rhs.ctb_;
    }
    template<typename V>
    promise_handle(promise_handle<V>&& rhs) noexcept {
        std::swap(ctb_, rhs.ctb_);
    }
    template<typename V>
    promise_handle& operator=(promise_handle<V>&& rhs) noexcept {
        if (this != &rhs) {
            std::swap(ctb_, rhs.ctb_);
        }
        return *this;
    }

    bool resume() {
        if (valid() && !ctb_->done()) {
            ctb_->resume();
            return true;
        }
        return false;
    }
    void operator()() { resume(); }

    bool valid() noexcept { return ctb_ && ctb_->valid(); }
    bool is_done() noexcept { return ctb_ && ctb_->done(); }
    void cancel_self_release() {
        if (ctb_)
            ctb_->set_release_by_task(false);
    }

    ~promise_handle() {
        if (valid() && !ctb_->is_release_by_task()) {
            ctb_->destroy_state_chain();
        }
    }

  protected:
    std::shared_ptr<shared_state> ctb_;
    std::shared_ptr<void> result_;
};

template<typename T>
class promise_handle : public promise_handle<> {
  public:
    promise_handle() { result_ = std::make_shared<T>(); };
    ~promise_handle() = default;
    promise_handle(const promise_handle& rhs) noexcept { ctb_ = rhs.ctb_; }
    promise_handle& operator=(const promise_handle& rhs) noexcept { ctb_ = rhs.ctb_; }
    promise_handle(promise_handle&& rhs) noexcept { ctb_ = std::move(rhs.ctb_); }
    promise_handle& operator=(promise_handle&& rhs) noexcept {
        if (this != &rhs)
            ctb_ = std::move(rhs.ctb_);
        return *this;
    }

    template<typename U>
    void set_value(U&& value) {
        _ASSERT(!is_done());
        *reinterpret_cast<T*>(result_.get()) = std::forward<U>(value);
        resume();
    }
    void set_exception(std::exception_ptr eptr) {
        _ASSERT(!is_done());
        auto coro =
            static_cast<coroutine<task<T>::promise_type>*>(static_cast<coroutine<>*>(ctb_.get()));
        coro->promise().set_eptr(std::move(eptr));
        resume();
    }

    auto get_task() {
        _ASSERT(!ctb_);
        auto result = std::static_pointer_cast<T>(result_);
        auto t = [](std::shared_ptr<T> value) -> task<T> {
            co_await ex::suspend_always{};
            return *(value.get());
        }(std::move(result));
        ctb_ = t.ctb_;
        return std::move(t);
    }
};

template<typename T>
class task {
  public:
    class promise_type {
      public:
        using result_type = T;
        promise_type& get_return_object() noexcept { return *this; }
        auto initial_suspend() noexcept { return ex::suspend_never{}; }
        auto final_suspend() noexcept {
            struct final_awaiter {
                promise_type* me;
                bool await_ready() noexcept {
                    // suspend point
                    return false;
                }
                void await_suspend(coroutine<>) noexcept {
                    // if suspend by caller , then resume to it.
                    if (me->caller_)
                        me->caller_();
                }
                void await_resume() noexcept {}
            };
            return final_awaiter{this};
        }

        template<typename U>
        void return_value(U&& value) {
            result_ = std::forward<U>(value);
        }

#ifdef AWAITABLE_TASKS_CAPTURE_EXCEPTION
        void uncought_exceptions() { set_eptr(std::current_exception()); }
        void set_exception(std::exception_ptr eptr) noexcept { set_eptr(std::move(eptr)); }
#endif

#if defined(AWAITABLE_TASKS_VARIANT_MAPBOX) || defined(AWAITABLE_TASKS_VARIANT_MPARK) || \
    defined(AWAITABLE_TASKS_VARIANT_STD)
        void set_eptr(std::exception_ptr eptr) noexcept { result_ = std::move(eptr); }
#else
        void set_eptr(std::exception_ptr eptr) noexcept { eptr_ = std::move(eptr); }
#endif
        template<typename U>
        void set_value(U&& value) {
            result_ = std::move(value);
        }

        void set_caller(coroutine<> caller_coro) noexcept { caller_ = caller_coro; }
        void throw_if_exception() const {
#if defined(AWAITABLE_TASKS_VARIANT_MAPBOX)
            if (result_.which() == result_.which<std::exception_ptr>())
                std::rethrow_exception(result_.get<std::exception_ptr>());
            else if (result_.which() != result_.which<result_type>())
                throw std::runtime_error("value not returned");
#elif defined(AWAITABLE_TASKS_VARIANT_MPARK) || defined(AWAITABLE_TASKS_VARIANT_STD)
            if (ns_variant::get_if<std::exception_ptr>(&result_))
                std::rethrow_exception(ns_variant::get<std::exception_ptr>(result_));
            else if (ns_variant::get_if<monostate>(&result_))
                throw std::runtime_error("value not returned");
#else
            if (eptr_)
                std::rethrow_exception(eptr_);
#endif
        }

        auto& get_result() { return result_; }
#define AWAIT_TASKS_TRACE_PROMISE
#ifdef AWAIT_TASKS_TRACE_PROMISE
        using alloc_of_char_type = std::allocator<char>;
        void* operator new(size_t size) {
            alloc_of_char_type al;
            auto ptr = al.allocate(size);
            AWAITABLE_TASKS_TRACE("promise created %p", ptr);
            return ptr;
        }

        void operator delete(void* ptr, size_t size) noexcept {
            alloc_of_char_type al;
            AWAITABLE_TASKS_TRACE("promise destroy %p", ptr);
            return al.deallocate(static_cast<char*>(ptr), size);
        }
#endif
#if defined(AWAITABLE_TASKS_VARIANT_MAPBOX)
        struct monostate {};
        mapbox::util::variant<monostate, result_type, std::exception_ptr> result_;
#elif defined(AWAITABLE_TASKS_VARIANT_MPARK) || defined(AWAITABLE_TASKS_VARIANT_STD)
        struct monostate {};
        ns_variant::variant<monostate, result_type, std::exception_ptr> result_;
#else
        std::exception_ptr eptr_ = nullptr;
        result_type result_{};
#endif
        coroutine<> caller_;
    };
    bool await_ready() noexcept { return is_done_or_empty(); }

    T await_resume() {
        coro_.promise().throw_if_exception();
#if defined(AWAITABLE_TASKS_VARIANT_MAPBOX)
        return std::move(coro_.promise().get_result().get<T>());
#elif defined(AWAITABLE_TASKS_VARIANT_MPARK) || defined(AWAITABLE_TASKS_VARIANT_STD)
        return std::move(ns_variant::get<T>(coro_.promise().get_result()));
#else
        return std::move(coro_.promise().get_result());
#endif
    }
    void await_suspend(coroutine<> caller_coro) noexcept {
        coro_.promise().set_caller(caller_coro);
    }

    explicit task(promise_type& prom) noexcept
        : coro_(coroutine<promise_type>::from_promise(prom)) {
        ctb_ = std::make_shared<shared_state>(coro_);
    }

    task() = default;
    task(task const&) = delete;
    task& operator=(task const&) = delete;
    task(task&& rhs) noexcept : coro_(rhs.coro_), ctb_(rhs.ctb_) {
        rhs.coro_ = nullptr;
        rhs.ctb_ = nullptr;
    }
    task& operator=(task&& rhs) noexcept {
        if (&rhs != this) {
            coro_ = rhs.coro_;
            ctb_ = rhs.ctb_;
            rhs.ctb_ = nullptr;
            rhs.coro_ = nullptr;
        }
        return *this;
    }

    void reset() noexcept {
        if (coro_) {
            ctb_->destroy_state();
            coro_ = nullptr;
        }
    }

    ~task() noexcept {
        if (ctb_ && ctb_->is_release_by_task())
            reset();
    }

  public:
    bool is_done_or_empty() noexcept { return coro_ ? coro_.done() : true; }
    const T* value_ref() {
        if (coro_) {
#if defined(AWAITABLE_TASKS_VARIANT_MAPBOX)
            auto& result = coro_.promise().get_result();
            if (result.which() == result.which<T>())
                return &result.get<T>();
            else
                return nullptr;
#elif defined(AWAITABLE_TASKS_VARIANT_MPARK) || defined(AWAITABLE_TASKS_VARIANT_STD)
            auto& result = coro_.promise().get_result();
            return ns_variant::get_if<T>(&result);
#else
            return &coro_.promise().get_result();
#endif
        } else {
            return nullptr;
        }
    }

  private:
    template<typename>
    friend class task;
    template<typename>
    friend class promise_handle;

    std::shared_ptr<shared_state> ctb_;
    coroutine<promise_type> coro_ = nullptr;

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
        auto callee_ctb = ctb_;
        auto next_task = [](task t, std::decay_t<F> f) -> typename R::TaskReturn {
            auto&& value = co_await t;
            return co_await f(value);
        }
        (std::move(*this), std::move(func));
        next_task.ctb_->set_release_by_task(callee_ctb->is_release_by_task());
        callee_ctb->next_ = next_task.ctb_;
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 0 && R::TaskOrRet::value &&
                         (std::is_same_v<typename R::OrignalRet, detail::Unkown> ||
                             std::is_same_v<typename R::OrignalRet, void>),
            task>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto callee_ctb = ctb_;
        auto next_task = [](task t, std::decay_t<F> f) -> task {
            auto&& value = co_await t;
            return co_await f();
            return value;
        }(std::move(*this), std::move(func));
        next_task.ctb_->set_release_by_task(callee_ctb->is_release_by_task());
        callee_ctb->next_ = next_task.ctb_;
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 0 && R::TaskOrRet::value &&
                         (!std::is_same_v<typename R::OrignalRet, detail::Unkown> &&
                             !std::is_same_v<typename R::OrignalRet, void>),
            typename R::TaskReturn>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto callee_ctb = ctb_;
        auto next_task = [](task t, std::decay_t<F> f) -> typename R::TaskReturn {
            co_await t;
            auto ff = f();
            auto&& value = co_await ff;
            return value;
        }
        (std::move(*this), std::move(func));
        next_task.ctb_->set_release_by_task(callee_ctb->is_release_by_task());
        callee_ctb->next_ = next_task.ctb_;
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 1 && !R::TaskOrRet::value &&
                         std::is_same_v<typename R::OrignalRet, void>,
            task>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto callee_ctb = ctb_;
        auto next_task = [](task t, std::decay_t<F> f) -> task {
            auto&& value = co_await t;
            f(value);
            return value;
        }(std::move(*this), std::forward<F>(func));
        next_task.ctb_->set_release_by_task(callee_ctb->is_release_by_task());
        callee_ctb->next_ = next_task.ctb_;
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 1 && !R::TaskOrRet::value &&
                         !std::is_same_v<typename R::OrignalRet, void>,
            typename R::TaskReturn>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto callee_ctb = ctb_;
        auto next_task = [](task t, std::decay_t<F> f) -> typename R::TaskReturn {
            auto&& value = co_await t;
            return f(value);
        }
        (std::move(*this), std::forward<F>(func));
        next_task.ctb_->set_release_by_task(callee_ctb->is_release_by_task());
        callee_ctb->next_ = next_task.ctb_;
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 0 && !R::TaskOrRet::value &&
                         std::is_same_v<typename R::OrignalRet, void>,
            task>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto callee_ctb = ctb_;
        auto next_task = [](task t, std::decay_t<F> f) -> typename task {
            auto&& value = co_await t;
            f();
            return value;
        }
        (std::move(*this), std::forward<F>(func));
        next_task.ctb_->set_release_by_task(callee_ctb->is_release_by_task());
        callee_ctb->next_ = next_task.ctb_;
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 0 && !R::TaskOrRet::value &&
                         !std::is_same_v<typename R::OrignalRet, void>,
            typename R::TaskReturn>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto callee_ctb = ctb_;
        auto next_task = [](task t, std::decay_t<F> f) -> typename R::TaskReturn {
            co_await t;
            return f();
        }
        (std::move(*this), std::forward<F>(func));
        next_task.ctb_->set_release_by_task(callee_ctb->is_release_by_task());
        callee_ctb->next_ = next_task.ctb_;
        return std::move(next_task);
    }
#pragma endregion
};
}

#pragma region task helpers

// when_all range
#include <vector>
namespace awaitable_tasks {
namespace detail {
template<typename T>
struct when_all_range_context {
    template<typename R>
    using result_type = std::vector<R>;

    using data_type = result_type<T>;
    using retrun_type = task<data_type>;

    using holder_type = detail::Unkown;
    using task_holder_type = task<holder_type>;
    using task_holder_container_type = std::vector<task_holder_type>;

    promise_handle<holder_type> handle;
    data_type results;
    size_t task_count = 0;
    task_holder_container_type tasks_holder;
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
    template<typename R>
    using result_type = std::vector<std::pair<size_t, R>>;
    using data_type = result_type<T>;
    using retrun_type = task<data_type>;
    using holder_type = detail::Unkown;
    using task_holder_type = task<holder_type>;
    using task_holder_container_type = std::vector<task_holder_type>;

    inline void set_result(size_t idx, T& data) { results.emplace_back(idx, std::move(data)); }

    promise_handle<holder_type> handle;
    data_type results;
    size_t task_count = 0;
    task_holder_container_type tasks_holder;
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
                if (--data.task_count == 0) {
                    data.handle.resume();
                }
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
template<bool any, typename... Ts>
struct when_variadic_context;
template<typename... Ts>
struct when_variadic_context<false, Ts...> {
    using result_type = std::tuple<std::decay_t<typename isTaskOrRet<Ts>::Inner>...>;
    using data_type = result_type;
    using task_type = task<data_type>;

    template<bool any, size_t I, typename T>
    inline void set_variadic_result(T& t) {
        if (task_count != 0) {
            std::get<I>(results) = std::move(t);
            if (--task_count == 0) {
                handle.resume();
            }
        }
    }
    promise_handle<detail::Unkown> handle;
    data_type results;
    size_t task_count = sizeof...(Ts);
    std::array<task<Unkown>, sizeof...(Ts)> tasks_holder;
};

template<typename T, typename F>
inline decltype(auto) task_transform(T& t, F&& f) {
    return t.then(std::move(f));
}

template<bool any, size_t... Is, typename... Ts, typename Ctx = when_variadic_context<any, Ts...>>
typename Ctx::task_type when_variant_impl(std::index_sequence<Is...>, Ts&... ts) {
    auto ctx = std::make_shared<Ctx>();
    ctx->tasks_holder = {
        task_transform(ts, [ctx](typename detail::isTaskOrRet<Ts>::Inner a) -> Unkown {
            ctx->set_variadic_result<any, Is>(a);
            return Unkown{};
        })...};
    return ctx->handle.get_task().then([ctx] { return std::move(ctx->results); });
}
}

template<typename T, typename... Ts>
decltype(auto) when_all(task<T>& t, Ts&... ts) {
    return detail::when_variant_impl<false>(std::make_index_sequence<sizeof...(Ts) + 1>{},
                                            t,
                                            ts...);
}
#if defined(AWAITABLE_TASKS_VARIANT_MAPBOX) || defined(AWAITABLE_TASKS_VARIANT_MPARK) || \
    defined(AWAITABLE_TASKS_VARIANT_STD)
namespace detail {
template<typename... Ts>
struct when_variadic_context<true, Ts...> {
#if defined(AWAITABLE_TASKS_VARIANT_MAPBOX)
    using result_type = mapbox::util::variant<std::decay_t<typename isTaskOrRet<Ts>::Inner>...>;
#elif defined(AWAITABLE_TASKS_VARIANT_MPARK) || defined(AWAITABLE_TASKS_VARIANT_STD)
    using result_type = ns_variant::variant<std::decay_t<typename isTaskOrRet<Ts>::Inner>...>;
#endif
    using data_type = result_type;
    using task_type = task<data_type>;

    template<bool any, size_t I, typename T>
    inline void set_variadic_result(T& t) {
        if (task_count != 0) {
            results = std::move(t);
            handle.resume();
        }
    }
    promise_handle<detail::Unkown> handle;
    data_type results;
    size_t task_count = sizeof...(Ts);
    std::array<task<Unkown>, sizeof...(Ts)> tasks_holder;
};
}
template<typename T, typename... Ts>
decltype(auto) when_any(task<T>& t, Ts&... ts) {
    return detail::when_variant_impl<true>(std::make_index_sequence<sizeof...(Ts) + 1>{}, t, ts...);
}
#endif
}
#pragma endregion

#pragma pack(pop)
