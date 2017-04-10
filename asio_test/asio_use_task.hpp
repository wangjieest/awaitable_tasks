//
// asio_use_task.hpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_USE_TASK_HPP
#define ASIO_USE_TASK_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
#pragma once
#endif  // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "awaitable_tasks.hpp"
#include "asio/detail/config.hpp"
#include <memory>

#include "asio/detail/push_options.hpp"
#include "asio/async_result.hpp"
#include "asio/error_code.hpp"
#include "asio/handler_type.hpp"
#include "asio/system_error.hpp"

#define ASIO_TASK_EXCEPTION 0
#define ASIO_TASK_TUPLE 1
#define ASIO_TASK_MAPBOX_VARIANT 2

#ifndef ASIO_TASK_IMPL
#define ASIO_TASK_IMPL ASIO_TASK_EXCEPTION
#endif

#if ASIO_TASK_IMPL == ASIO_TASK_MAPBOX_VARIANT
#include "mapbox/variant.hpp"
#endif

namespace asio {

/// Class used to specify that an asynchronous operation should return a task.
/**
* The use_task_t class is used to indicate that an asynchronous operation
* should return a task object. A use_task_t object may be passed as a
* handler to an asynchronous operation, typically using the special value @c
* asio::use_task. For example:
*
* @code std::task<std::size_t> my_task
*   = my_socket.async_read_some(my_buffer, asio::use_task); @endcode
*
* The initiating function (async_read_some in the above example) returns a
* task that will receive the result of the operation. If the operation
* completes with an error_code indicating failure, it is converted into a
* system_error and passed back to the caller via the task.
*/
template<typename Allocator = std::allocator<void> >
class use_task_t {
  public:
    /// The allocator type. The allocator is used when constructing the
    /// @c std::promise object for a given asynchronous operation.
    typedef Allocator allocator_type;

    /// Construct using default-constructed allocator.
    ASIO_CONSTEXPR use_task_t() {}

    /// Construct using specified allocator.
    explicit use_task_t(const Allocator& allocator) : allocator_(allocator) {}

    /// Specify an alternate allocator.
    template<typename OtherAllocator>
    use_task_t<OtherAllocator> operator[](const OtherAllocator& allocator) const {
        return use_task_t<OtherAllocator>(allocator);
    }

    /// Obtain allocator.
    allocator_type get_allocator() const { return allocator_; }

  private:
    Allocator allocator_;
};

/// A special value, similar to std::nothrow.
/**
* See the documentation for asio::use_task_t for a usage example.
*/
#if defined(ASIO_HAS_CONSTEXPR) || defined(GENERATING_DOCUMENTATION)
// constexpr use_task_t<> use_task;
// #elif defined(ASIO_MSVC)
#pragma warning(push)
#pragma warning(disable : 4592)
__declspec(selectany) use_task_t<> use_task;
#pragma warning(pop)
#endif

namespace detail {

// Completion handler to adapt a promise as a completion handler.
template<typename T>
class promise_handler {
  public:
#if ASIO_TASK_IMPL == ASIO_TASK_EXCEPTION
    using result_type_t = T;
#elif ASIO_TASK_IMPL == ASIO_TASK_TUPLE
    using result_type_t = std::tuple<asio::error_code, T>;
#elif ASIO_TASK_IMPL == ASIO_TASK_MAPBOX_VARIANT
    using result_type_t = mapbox::util::variant<asio::error_code, T>;
#endif
    // Construct from use_task special value.
    template<typename Allocator>
    promise_handler(use_task_t<Allocator> uf) {
        auto tmp = std::allocate_shared<data>(uf.get_allocator());
        data_ = tmp;
        data_->task_ =
            awaitable_tasks::make_task([tmp]() -> result_type_t { return std::move(tmp->value_); });
        data_->promise_handle_ = data_->task_.get_promise_handle();
    }

    void operator()(T t) {
#if ASIO_TASK_IMPL == ASIO_TASK_TUPLE
        data_->value_ = result_type_t(asio::error_code(), std::move(t));
#else
        data_->value_ = std::move(t);
#endif
        data_->promise_handle_.resume();
    }

    void operator()(const asio::error_code& ec, T t) {
#if ASIO_TASK_IMPL == ASIO_TASK_EXCEPTION
        if (ec) {
            data_->promise_handle_.set_exception(std::make_exception_ptr(asio::system_error(ec)));
        } else {
            data_->value_ = std::move(t);
        }
#elif ASIO_TASK_IMPL == ASIO_TASK_TUPLE
        data_->value_ = result_type_t(ec, std::move(t));
#elif ASIO_TASK_IMPL == ASIO_TASK_MAPBOX_VARIANT
        if (ec) {
            data_->value_ = std::move(ec);
        } else {
            data_->value_ = std::move(t);
        }
#endif
        data_->promise_handle_.resume();
    }

    // private:
    struct data {
        awaitable_tasks::promise_handle<result_type_t> promise_handle_;
        awaitable_tasks::task<result_type_t> task_;
        result_type_t value_;
    };
    std::shared_ptr<data> data_;
};

// Completion handler to adapt a void promise as a completion handler.
template<>
class promise_handler<void> {
  public:
#if ASIO_TASK_IMPL == ASIO_TASK_EXCEPTION
    using result_type_t = asio::error_code;
#elif ASIO_TASK_IMPL == ASIO_TASK_TUPLE
    using result_type_t = asio::error_code;
#elif ASIO_TASK_IMPL == ASIO_TASK_MAPBOX_VARIANT
    using result_type_t = asio::error_code;
#endif

    // Construct from use_task special value. Used during rebinding.
    template<typename Allocator>
    promise_handler(use_task_t<Allocator> uf) {
        data_ = std::allocate_shared<data>(uf.get_allocator());
        data_->task_ = awaitable_tasks::make_task<result_type_t>();
        data_->promise_handle_ = data_->task_.get_promise_handle();
    }

    void operator()() { data_->promise_handle_.resume(); }

    void operator()(const asio::error_code& ec) {
#if ASIO_TASK_IMPL == ASIO_TASK_EXCEPTION
        if (ec) {
            data_->promise_handle_.set_exception(std::make_exception_ptr(asio::system_error(ec)));
        } else {
            data_->value_ = std::move(ec);
        }
#elif ASIO_TASK_IMPL == ASIO_TASK_TUPLE
        data_->value_ = std::move(ec);
#elif ASIO_TASK_IMPL == ASIO_TASK_MAPBOX_VARIANT
        data_->value_ = std::move(ec);
#endif
        data_->promise_handle_.resume();
    }

    // private:
    struct data {
        awaitable_tasks::promise_handle<result_type_t> promise_handle_;
        awaitable_tasks::task<result_type_t> task_;
        result_type_t value_;
    };
    std::shared_ptr<data> data_;
};

#ifdef AWAITABLE_TASKS_CAPTURE_EXCEPTION
// promise has set_exception will handle exception self.
#else
// Ensure any exceptions thrown from the handler are propagated back to the
// caller via the task.
template<typename Function, typename T>
void asio_handler_invoke(Function f, promise_handler<T>* h) {
    auto p(h->data_);
    try {
        f();
    } catch (...) {
        p->promise_handle_.set_exception(std::current_exception());
    }
}
#endif
}  // namespace detail

#if !defined(GENERATING_DOCUMENTATION)

// Handler traits specialisation for promise_handler.
template<typename T>
class async_result<detail::promise_handler<T> > {
  public:
    // The initiating function will return a task.
    typedef awaitable_tasks::task<typename detail::promise_handler<T>::result_type_t> type;

    // Constructor creates a new promise for the async operation, and obtains the
    // corresponding task.
    explicit async_result(detail::promise_handler<T>& h) { task_ = std::move(h.data_->task_); }

    // Obtain the task to be returned from the initiating function.
    type get() { return std::move(task_); }

  private:
    type task_;
};

// Handler type specialisation for zero arg.
template<typename Allocator, typename ReturnType>
struct handler_type<use_task_t<Allocator>, ReturnType()> {
    typedef detail::promise_handler<void> type;
};

// Handler type specialisation for one arg.
template<typename Allocator, typename ReturnType, typename Arg1>
struct handler_type<use_task_t<Allocator>, ReturnType(Arg1)> {
    typedef detail::promise_handler<Arg1> type;
};

// Handler type specialisation for two arg.
template<typename Allocator, typename ReturnType, typename Arg2>
struct handler_type<use_task_t<Allocator>, ReturnType(asio::error_code, Arg2)> {
    typedef detail::promise_handler<Arg2> type;
};

// Handler type specialisation for special arg asio::error_code.
#if ASIO_TASK_IMPL == ASIO_TASK_MAPBOX_VARIANT
#else
template<typename Allocator, typename ReturnType>
struct handler_type<use_task_t<Allocator>, ReturnType(asio::error_code)> {
    typedef detail::promise_handler<void> type;
};
#endif

#endif  // !defined(GENERATING_DOCUMENTATION)

}  // namespace asio

#include "asio/detail/pop_options.hpp"

#endif  // ASIO_USE_TASK_HPP
