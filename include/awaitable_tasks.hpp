#pragma once
#include <experimental/resumable>
#include <memory>
#include <atomic>
#if 1
#define AWAITABLE_TASKS_TRACE(fmt, ...) printf("\n" fmt "\n", ##__VA_ARGS__)
#else
#define AWAITABLE_TASKS_TRACE(fmt, ...)
#endif
#define AWAITABLE_TASKS_EXCEPTION 1
#define AWAITABLE_TASKS_VARIANT 1

namespace awaitable_tasks {
namespace ex = std::experimental;
template<typename>
class task;
template<typename = void>
class promise;

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
    ~shared_state() = default;

    bool valid() { return address() != nullptr; }
    void clear_coro() { *static_cast<ex::coroutine_handle<>*>(this) = nullptr; }
    void destroy_chain() {
        if (self_release_) {
            return;
        }
        rescue_destroy(next_);
        if (valid()) {
            destroy();
        }
    }

  protected:
    static void rescue_destroy(std::shared_ptr<shared_state>& target) {
        if (target) {
            rescue_destroy(target->next_);
            if (target->valid())
                target->destroy();
        }
    }
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
        ctb_ = std::move(rhs.ctb_);
    }
    template<typename V>
    promise_handle& operator=(promise_handle<V>&& rhs) noexcept {
        if (this != &rhs) {
            ctb_ = std::move(rhs.ctb_);
        }
        return *this;
    }

    void resume() {
        if (valid() && !ctb_->done()) {
            ctb_->resume();
        }
    }
    void operator()() { resume(); }

    bool valid() noexcept { return ctb_ && ctb_->valid(); }
    void cancel_self_release() {
        if (ctb_)
            ctb_->set_self_release(false);
    }
    ~promise_handle() {
        if (ctb_)
            ctb_->destroy_chain();
    }

  protected:
    std::shared_ptr<shared_state> ctb_;
};

template<typename T>
class promise_handle : public promise_handle<> {
  public:
    static promise_handle from_task(task<T>& t) noexcept {
        promise_handle ret;
        ret.ctb_ = t.ctb_;
        return ret;
    }
    void set_exception(std::exception_ptr eptr) {
        auto prom =
            (static_cast<
                ex::coroutine_handle<promise<T>>*>(static_cast<ex::coroutine_handle<>*>(ctb_.get()))
                    ->promise());
        prom.eptr_ = std::move(eptr);
        prom.set_value();
    }
};

template<>
class promise<void> {
  public:
    auto initial_suspend() noexcept { return ex::suspend_never{}; }
    auto final_suspend() noexcept {
        struct final_awaiter {
            promise* me;
            bool await_ready() noexcept { return false; }
            void await_suspend(ex::coroutine_handle<>) noexcept { me->set_value(); }
            void await_resume() noexcept {}
        };
        return final_awaiter{this};
    }
    void set_caller(ex::coroutine_handle<> caller_coro) noexcept { caller_coro_ = caller_coro; }
#if AWAITABLE_TASKS_EXCEPTION
    void set_exception(std::exception_ptr eptr) {
        eptr_ = std::move(eptr);
        set_value();
    }
    void throw_if_exception() const {
        if (eptr_)
            std::rethrow_exception(eptr_);
    }
#endif
    void set_value() {
        if (caller_coro_)
            caller_coro_.resume();
    }

  protected:
    template<typename>
    friend class promise_handle;
    promise() = default;
    ~promise() { AWAITABLE_TASKS_TRACE("promise destroy %p", this); }
    std::exception_ptr eptr_;
    ex::coroutine_handle<> caller_coro_;
};

template<typename T>
class promise : public promise<void> {
  public:
    using promise<>::set_value;
    promise& get_return_object() noexcept { return *this; }
    template<typename U>
    void return_value(U&& value) {
        result_ = std::forward<U>(value);
    }
    template<typename U>
    void set_value(U&& value) {
        result_ = std::forward<U>(value);
        set_value();
    }
    T result_;
};

template<typename T>
class task {
  public:
    using result_type = detail::Unkown::Void_To_Unkown<T>;
    using promise_type = promise<result_type>;

    bool await_ready() noexcept { return is_ready(); }

#if AWAITABLE_TASKS_EXCEPTION
    result_type&& await_resume() {
        get_promise()->throw_if_exception();
        return std::move(cur_value_ref());
    }
#else
    result_type&& await_resume() noexcept { return std::move(cur_value_ref()); }
#endif

    void await_suspend(ex::coroutine_handle<> caller_coro) noexcept {
        auto prom = get_promise();
        if (prom) {
            prom->set_caller(caller_coro);
        }
    }

    explicit task(promise_type& prom) noexcept {
        auto coro =
            ex::coroutine_handle<>::from_address(
                                        ex::coroutine_handle<promise_type>::from_promise(prom)
                                            .address());
        AWAITABLE_TASKS_TRACE("promise created %p", &prom);
        ctb_ = std::make_shared<shared_state>(coro);
    }

    task() = default;
    task(task const&) = delete;
    task& operator=(task const&) = delete;
    task(task&& rhs) noexcept : ctb_(std::move(rhs.ctb_)) {}
    task& operator=(task&& rhs) noexcept {
        if (&rhs != this) {
            ctb_ = std::move(rhs.ctb_);
        }
        return *this;
    }

    void reset() noexcept {
        if (ctb_ && ctb_->valid()) {
            ctb_->destroy();
            ctb_->clear_coro();
        }
    }

    ~task() noexcept {
        if (ctb_ && ctb_->is_self_release())
            reset();
    }

  public:
    promise_handle<result_type> get_promise_handle() noexcept {
        return promise_handle<result_type>::from_task(*this);
    }

    bool is_ready() noexcept {
        // when coro_ empty task is just already moved. return true.
        return ctb_ ? ctb_->done() : true;
    }

    // can ref value at any time
    result_type& cur_value_ref() noexcept {
        auto prom = get_promise();
        if (prom) {
            return prom->result_;
        } else {
            // as task are moved.
            static result_type empty{};
            return empty;
        }
    }

  public:
    template<typename F,
        typename FF = typename detail::FunctionReferenceToPointer<F>::type,
        typename C = typename detail::CallArgsWith<FF, result_type>>
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
    template<typename>
    friend class task;
    template<typename>
    friend class promise_handle;
    std::shared_ptr<shared_state> ctb_;

    promise_type* get_promise() {
        if (ctb_)
            return &(static_cast<ex::coroutine_handle<
                         promise_type>*>(static_cast<ex::coroutine_handle<>*>(ctb_.get()))
                         ->promise());
        else
            return nullptr;
    }
    template<bool, typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) >= 2, void> then_impl(F&& func,
                                                    detail::callable_traits<F(Args...)>) noexcept {
        static_assert(false, "then must use zero/one param");
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 1 && R::TaskOrRet::value, typename R::TaskReturn>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto caller_ctb = ctb_;
        R::TaskReturn next_task = [](task<T> t, std::decay_t<F> f) -> typename R::TaskReturn {
            auto&& value = co_await t;
            return co_await f(value);
        }
        (std::move(*this), std::move(func));
        next_task.ctb_->set_self_release(caller_ctb->is_self_release());
        caller_ctb->next_ = next_task.ctb_;
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 0 && R::TaskOrRet::value &&
                         (std::is_same_v<typename R::OrignalRet, detail::Unkown> ||
                             std::is_same_v<typename R::OrignalRet, void>),
            task<T>>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto caller_ctb = ctb_;
        auto next_task = [](task<T> t, std::decay_t<F> f) -> task<T> {
            auto&& value = co_await t;
            return co_await f();
            return value;
        }(std::move(*this), std::move(func));
        next_task.ctb_->set_self_release(caller_ctb->is_self_release());
        caller_ctb->next_ = next_task.ctb_;
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 0 && R::TaskOrRet::value &&
                         (!std::is_same_v<typename R::OrignalRet, detail::Unkown> &&
                             !std::is_same_v<typename R::OrignalRet, void>),
            typename R::TaskReturn>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto caller_ctb = ctb_;
        R::TaskReturn next_task = [](task<T> t, std::decay_t<F> f) -> typename R::TaskReturn {
            co_await t;
            auto ff = f();
            auto&& value = co_await ff;
            return value;
        }
        (std::move(*this), std::move(func));
        next_task.ctb_->set_self_release(caller_ctb->is_self_release());
        caller_ctb->next_ = next_task.ctb_;
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 1 && !R::TaskOrRet::value &&
                         std::is_same_v<typename R::OrignalRet, void>,
            task<T>>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto caller_ctb = ctb_;
        auto next_task = [](task<T> t, std::decay_t<F> f) -> task<T> {
            auto&& value = co_await t;
            f(value);
            return value;
        }(std::move(*this), std::forward<F>(func));
        next_task.ctb_->set_self_release(caller_ctb->is_self_release());
        caller_ctb->next_ = next_task.ctb_;
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 1 && !R::TaskOrRet::value &&
                         !std::is_same_v<typename R::OrignalRet, void>,
            typename R::TaskReturn>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto caller_ctb = ctb_;
        R::TaskReturn next_task = [](task<T> t, std::decay_t<F> f) -> typename R::TaskReturn {
            auto&& value = co_await t;
            return f(value);
        }
        (std::move(*this), std::forward<F>(func));
        next_task.ctb_->set_self_release(caller_ctb->is_self_release());
        caller_ctb->next_ = next_task.ctb_;
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 0 && !R::TaskOrRet::value &&
                         std::is_same_v<typename R::OrignalRet, void>,
            task<T>>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto caller_ctb = ctb_;
        auto next_task = [](task<T> t, std::decay_t<F> f) -> typename task<T> {
            auto&& value = co_await t;
            f();
            return value;
        }
        (std::move(*this), std::forward<F>(func));
        next_task.ctb_->set_self_release(caller_ctb->is_self_release());
        caller_ctb->next_ = next_task.ctb_;
        return std::move(next_task);
    }

    template<typename F, typename R, typename... Args>
    std::enable_if_t<sizeof...(Args) == 0 && !R::TaskOrRet::value &&
                         !std::is_same_v<typename R::OrignalRet, void>,
            typename R::TaskReturn>
    then_impl(F&& func, detail::callable_traits<F(Args...)>) noexcept {
        auto caller_ctb = ctb_;
        R::TaskReturn next_task = [](task<T> t, std::decay_t<F> f) -> typename R::TaskReturn {
            co_await t;
            return f();
        }
        (std::move(*this), std::forward<F>(func));
        next_task.ctb_->set_self_release(caller_ctb->is_self_release());
        caller_ctb->next_ = next_task.ctb_;
        return std::move(next_task);
    }
};
}

// make_task
namespace awaitable_tasks {
template<bool Suspend = true,
    typename F,
    typename FF = typename detail::FunctionReferenceToPointer<F>::type,
    typename R = typename detail::CallArgsWith<FF>::TaskReturn>
R make_task(F&& func) noexcept {
    return [](FF f) -> R {
        co_await ex::suspend_if{Suspend};
        return f();
    }(std::forward<FF>(func));
}
}

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

    promise_handle<> handle;
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
    typename Ctx::task_holder_container_type tasks;
    tasks.reserve(all_task_count);

    for (size_t idx = 0; first != last; ++idx, ++first) {
        tasks.emplace_back((*first).then([ctx, idx](typename detail::isTaskOrRet<T>::Inner& a)
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

    auto ret = [](bool suspend,
                    std::shared_ptr<Ctx> c,
                    typename Ctx::task_holder_container_type ts) -> typename Ctx::retrun_type {
        // if no task resume at once
        co_await ex::suspend_if{suspend};
        return std::move(c->results);
    }
    (all_task_count != 0, ctx, std::move(tasks));
    ctx->handle = ret.get_promise_handle();
    return ret;
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

    promise_handle<> handle;
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
    typename Ctx::task_holder_container_type tasks;
    tasks.reserve(n);

    for (size_t idx = 0; first != last; ++idx, ++first) {
        tasks.emplace_back(
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

    auto ret = [](bool suspend,
                    std::shared_ptr<Ctx> c,
                    typename Ctx::task_holder_container_type ts) -> typename Ctx::retrun_type {
        // if no task resume at once
        co_await ex::suspend_if{suspend};
        return c->results;
    }
    (n != 0, ctx, std::move(tasks));
    ctx->handle = ret.get_promise_handle();
    return ret;
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
namespace awaitable_tasks {
namespace detail {
template<typename... Ts>
struct when_all_variadic_context {
    using result_type = std::tuple<std::decay_t<typename isTaskOrRet<Ts>::Inner>...>;
    using data_type = result_type;
    using task_type = task<data_type>;

    template<size_t I, typename T>
    inline void set_variadic_result(T& t) {
        if (task_count != 0) {
            std::get<I>(results) = std::move(t);

            if (--task_count == 0) {
                handle.resume();
            }
        }
    }

    promise_handle<> handle;  // shared
    data_type results;
    size_t task_count = sizeof...(Ts);
};

template<typename T, typename F>
inline decltype(auto) task_transform(T& t, F&& f) {
    return t.then(std::move(f));
}
template<size_t I>
using UnkownSequence = task<detail::Unkown>;

template<size_t... Is, typename... Ts, typename Ctx = when_all_variadic_context<Ts...>>
typename Ctx::task_type when_all_impl(std::index_sequence<Is...>, Ts&... ts) {
    auto ctx = std::make_shared<Ctx>();
    auto ret = [](std::shared_ptr<Ctx> c, typename UnkownSequence<Is>... tasks) ->
        typename Ctx::task_type {
        co_await ex::suspend_always{};
        return c->results;
    }
    (ctx, std::move(task_transform(ts, [ctx](typename detail::isTaskOrRet<Ts>::Inner a) -> Unkown {
        ctx->set_variadic_result<Is>(a);
        return Unkown{};
    }))...);
    ctx->handle = ret.get_promise_handle();
    return ret;
}
}

template<typename T, typename... Ts>
decltype(auto) when_all(T& t, Ts&... ts) {
    return detail::when_all_impl(std::make_index_sequence<sizeof...(Ts) + 1>{}, t, ts...);
}
}
