#pragma once
#include <experimental/resumable>
#include <memory>

#if 1
#define AWAITABLE_TASKS_TRACE(fmt, ...) printf("\n" fmt "\n", ##__VA_ARGS__)
#else
#define AWAITABLE_TASKS_TRACE(fmt, ...)
#endif

#define AWAITABLE_TASKS_CAPTURE_EXCEPTION

//#define AWAITABLE_TASKS_VARIANT

#ifdef AWAITABLE_TASKS_VARIANT
#include "mapbox/variant.hpp"
#endif

namespace awaitable_tasks {
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

struct shared_state : public ex::coroutine_handle<> {
    shared_state(ex::coroutine_handle<> coro) {
        *static_cast<ex::coroutine_handle<>*>(this) = coro;
    }

    bool is_self_release() { return self_release_; }
    void set_self_release(bool b) {
        self_release_ = b;
        if (next_)
            next_->set_self_release(b);
    }
    template<typename>
    friend class task;
    template<typename>
    friend class promise;
    ~shared_state() = default;

    bool valid() { return address() != nullptr; }
    void destroy_self() {
        if (valid())
            destroy();
        *static_cast<ex::coroutine_handle<>*>(this) = nullptr;
    }
    void destroy_chain() { recursive_destroy(this); }

  protected:
    static void recursive_destroy(shared_state* target) {
        if (target) {
            if (target->next_)
                recursive_destroy(target->next_.get());
            if (!target->is_self_release())
                target->destroy_self();
        }
    }

  protected:
    std::shared_ptr<shared_state> next_;
    bool self_release_ = true;
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

    void cancel_self_release() {
        if (ctb_)
            ctb_->set_self_release(false);
    }

    ~promise_handle() {
        if (valid() && !ctb_->is_self_release()) {
            ctb_->destroy_chain();
        }
    }

  protected:
    std::shared_ptr<shared_state> ctb_;
    std::shared_ptr<void> result_;
};

template<typename T>
class promise_handle : public promise_handle<> {
  public:
    promise_handle() = default;
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
        *((T*)(result_).get()) = std::forward<U>(value);
        resume();
    }
    void set_exception(std::exception_ptr eptr) {
        auto coro = static_cast<ex::coroutine_handle<
            task<T>::promise_type>*>(static_cast<ex::coroutine_handle<>*>(ctb_.get()));
        coro->promise().set_eptr(std::move(eptr));
        resume();
    }

    auto get_task() {
        auto result = std::make_shared<T>();
        result_ = result;
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
                bool await_ready() noexcept { return false; }
                void await_suspend(ex::coroutine_handle<>) noexcept {
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
        void set_exception(std::exception_ptr eptr) noexcept { set_eptr(std::move(eptr)); }
#endif

#ifdef AWAITABLE_TASKS_VARIANT
        void set_eptr(std::exception_ptr eptr) noexcept { result_ = std::move(eptr); }
        template<typename U>
        void set_value(U&& value) {
            result_ = std::move(value);
        }
#else
        void set_eptr(std::exception_ptr eptr) noexcept { eptr_ = std::move(eptr); }
        template<typename U>
        void set_value(U&& value) {
            result_ = std::move(value);
        }
#endif
        void set_caller(ex::coroutine_handle<> caller_coro) noexcept { caller_ = caller_coro; }
        void throw_if_exception() const {
#ifdef AWAITABLE_TASKS_VARIANT
            if (result_.which() == result_.which<std::exception_ptr>())
                std::rethrow_exception(result_.get<std::exception_ptr>());
            else if (result_.which() != result_.which<result_type>())
                throw std::runtime_error("value not returned");
#else
            if (eptr_)
                std::rethrow_exception(eptr_);
#endif
        }

        auto& get_result() { return result_; }
//#define AWAIT_TASKS_TRACE_PROMISE
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
#ifdef AWAITABLE_TASKS_VARIANT
        template<typename V>
        auto& get_result() {
            return result_.get<V>();
        }

        struct monostate {};
        mapbox::util::variant<monostate, result_type, std::exception_ptr> result_;
#else
        std::exception_ptr eptr_ = nullptr;
        result_type result_{};
#endif
        ex::coroutine_handle<> caller_;
    };
    bool await_ready() noexcept { return is_done_or_empty(); }

    T await_resume() {
        coro_.promise().throw_if_exception();
#ifdef AWAITABLE_TASKS_VARIANT
        return std::move(coro_.promise().get_result<T>());
#else
        return std::move(coro_.promise().get_result());
#endif
    }
    void await_suspend(ex::coroutine_handle<> caller_coro) noexcept {
        coro_.promise().set_caller(caller_coro);
    }

    explicit task(promise_type& prom) noexcept
        : coro_(ex::coroutine_handle<promise_type>::from_promise(prom)) {
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
            ctb_->destroy_self();
            coro_ = nullptr;
        }
    }

    ~task() noexcept {
        if (ctb_ && ctb_->is_self_release())
            reset();
    }

  public:
    bool is_done_or_empty() noexcept { return coro_ ? coro_.done() : true; }
    const T* value_ref() {
        if (coro_) {
#ifdef AWAITABLE_TASKS_VARIANT
            auto& result = coro_.promise().get_result();
            if (result.which() == result.which<T>())
                return &result.get<T>();
            else
                return nullptr;
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
    ex::coroutine_handle<promise_type> coro_ = nullptr;

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
        R::TaskReturn next_task = [](task t, std::decay_t<F> f) -> typename R::TaskReturn {
            auto&& value = co_await t;
            return co_await f(value);
        }
        (std::move(*this), std::move(func));
        next_task.ctb_->set_self_release(callee_ctb->is_self_release());
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
        next_task.ctb_->set_self_release(callee_ctb->is_self_release());
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
        R::TaskReturn next_task = [](task t, std::decay_t<F> f) -> typename R::TaskReturn {
            co_await t;
            auto ff = f();
            auto&& value = co_await ff;
            return value;
        }
        (std::move(*this), std::move(func));
        next_task.ctb_->set_self_release(callee_ctb->is_self_release());
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
        next_task.ctb_->set_self_release(callee_ctb->is_self_release());
        callee_ctb->next_ = next_task.ctb_;
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 1 && !R::TaskOrRet::value &&
                         !std::is_same_v<typename R::OrignalRet, void>,
            typename R::TaskReturn>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto callee_ctb = ctb_;
        R::TaskReturn next_task = [](task t, std::decay_t<F> f) -> typename R::TaskReturn {
            auto&& value = co_await t;
            return f(value);
        }
        (std::move(*this), std::forward<F>(func));
        next_task.ctb_->set_self_release(callee_ctb->is_self_release());
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
        next_task.ctb_->set_self_release(callee_ctb->is_self_release());
        callee_ctb->next_ = next_task.ctb_;
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 0 && !R::TaskOrRet::value &&
                         !std::is_same_v<typename R::OrignalRet, void>,
            typename R::TaskReturn>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto callee_ctb = ctb_;
        R::TaskReturn next_task = [](task t, std::decay_t<F> f) -> typename R::TaskReturn {
            co_await t;
            return f();
        }
        (std::move(*this), std::forward<F>(func));
        next_task.ctb_->set_self_release(callee_ctb->is_self_release());
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
    // save tasks in the vector
    using tasks_type = typename Ctx::task_holder_container_type;
    tasks_type task_datas;
    task_datas.reserve(all_task_count);

    for (size_t idx = 0; first != last; ++idx, ++first) {
        task_datas.emplace_back((*first).then([ctx, idx](typename detail::isTaskOrRet<T>::Inner& a)
                                                  -> detail::Unkown {
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
    return ctx->handle.get_task().then([ ctx, holder = std::move(task_datas) ] {
        return std::move(ctx->results);
    });
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
    // save tasks in the vector
    using tasks_type = typename Ctx::task_holder_container_type;
    tasks_type task_datas;
    task_datas.reserve(n);

    for (size_t idx = 0; first != last; ++idx, ++first) {
        task_datas.emplace_back(
                    (*first).then([ctx, idx](typename detail::isTaskOrRet<T>::Inner& a) -> decltype(
                                                                                            auto) {
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

    return ctx->handle.get_task().then([ ctx, holder = std::move(task_datas) ] {
        return std::move(ctx->results);
    });
}

// when_any returns type of std::pair<size_t, T>
template<typename InputIterator,
    typename T =
        typename detail::isTaskOrRet<std::iterator_traits<InputIterator>::value_type>::Inner>
task<std::pair<size_t, T>> when_any(InputIterator first, InputIterator last) {
    return when_n(first, last, 1)
        .then(
            [](std::vector<std::pair<size_t, T>>& vec) -> std::pair<size_t, T> { return vec[0]; });
}
}

// when_all variadic/zip
#include <tuple>
#include <array>
namespace awaitable_tasks {
namespace detail {
template<typename... Ts>
struct when_variadic_context {
#ifdef ASIO_TASK_MAPBOX_VARIANT
    using result_type = mapbox::util::variant<std::decay_t<typename isTaskOrRet<Ts>::Inner>...>;
#else
    using result_type = std::tuple<std::decay_t<typename isTaskOrRet<Ts>::Inner>...>;
#endif
    using data_type = result_type;
    using task_type = task<data_type>;

    template<bool any, size_t I, typename T>
    inline void set_variadic_result(T& t) {
        if (task_count != 0) {
#ifdef ASIO_TASK_MAPBOX_VARIANT
            results = std::move(t);
#else
            std::get<I>(results) = std::move(t);
#endif
            bool trigger = any;
            if (trigger && --task_count == 0) {
                handle.resume();
            }
        }
    }

    promise_handle<detail::Unkown> handle;  // shared
    data_type results;
    size_t task_count = sizeof...(Ts);
};

template<typename T, typename F>
inline decltype(auto) task_transform(T& t, F&& f) {
    return t.then(std::move(f));
}
template<size_t I>
using UnkownSequence = task<detail::Unkown>;

template<bool any, size_t... Is, typename... Ts, typename Ctx = when_variadic_context<Ts...>>
typename Ctx::task_type when_variant_impl(std::index_sequence<Is...>, Ts&... ts) {
    auto ctx = std::make_shared<Ctx>();
    std::array<task<Unkown>, sizeof...(Is)> task_datas{
        task_transform(ts, [ctx](typename detail::isTaskOrRet<Ts>::Inner a) -> Unkown {
            ctx->set_variadic_result<any, Is>(a);
            return Unkown{};
        })...};

    return ctx->handle.get_task().then([ ctx, holder = std::move(task_datas) ] {
        return std::move(ctx->results);
    });
}
}

template<typename T, typename... Ts>
decltype(auto) when_all(T& t, Ts&... ts) {
    return detail::when_variant_impl<false>(std::make_index_sequence<sizeof...(Ts) + 1>{},
                                            t,
                                            ts...);
}
#ifdef AWAITABLE_TASKS_VARIANT
template<typename T, typename... Ts>
decltype(auto) when_any(T& t, Ts&... ts) {
    return detail::when_variant_impl<true>(std::make_index_sequence<sizeof...(Ts) + 1>{}, t, ts...);
}
#endif
}
#pragma endregion
