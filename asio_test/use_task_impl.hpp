//
// impl/use_task.hpp
// ~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_IMPL_use_task_HPP
#define ASIO_IMPL_use_task_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include "asio/async_result.hpp"
#include "asio/error_code.hpp"
#include "asio/handler_type.hpp"
#include "asio/system_error.hpp"
#include "asio/detail/push_options.hpp"

#include "awaitable_tasks.hpp"

namespace asio {
namespace detail {

  // Completion handler to adapt a promise as a completion handler.
  template <typename T>
  class promise_handler
  {
  public:
#if AWAITABLE_TASKS_EXCEPTION
	  using result_type_t = T;
#else
	  using result_type_t = std::tuple<asio::error_code, T>;
#endif
    // Construct from use_task special value.
    template <typename Allocator>
    promise_handler(use_task_t<Allocator> /*uf*/)
    {
		auto tmp = std::make_shared<data>();
		data_ = tmp;
		data_->task_ = awaitable_tasks::make_task([tmp]()->result_type_t {return std::move(tmp->value_); });
		data_->promise_handle_ = data_->task_.get_promise_handle();
    }

    void operator()(T t)
    {
#if AWAITABLE_TASKS_EXCEPTION
		data_->value_ = std::move(t);
#else
		data_->value_ = result_type_t(asio::error_code(), std::move(t));
#endif
		data_->promise_handle_.resume();
    }

    void operator()(const asio::error_code& ec, T t)
    {
#if AWAITABLE_TASKS_EXCEPTION
		if (ec) {
			data_->promise_handle_.set_exception(
				std::make_exception_ptr(asio::system_error(ec)));
		}
		else {
			data_->value_ = std::move(t);
		}
#else
		data_->value_ = result_type_t(ec, std::move(t));
#endif
		data_->promise_handle_.resume();
    }

  //private:
	struct data {
		awaitable_tasks::promise_handle<result_type_t> promise_handle_;
		awaitable_tasks::task<result_type_t> task_;
		result_type_t value_;
	};
    std::shared_ptr<data > data_;
  };

  // Completion handler to adapt a void promise as a completion handler.
  template <>
  class promise_handler<void>
  {
  public:
#if AWAITABLE_TASKS_EXCEPTION
	  using result_type_t = asio::error_code;
#else
	  using result_type_t = asio::error_code;;
#endif
	  
	  // Construct from use_task special value. Used during rebinding.
    template <typename Allocator>
    promise_handler(use_task_t<Allocator> uf)
    {
		data_ = std::make_shared<data>();
		data_->task_ = awaitable_tasks::make_task<result_type_t>();
		data_->promise_handle_ = data_->task_.get_promise_handle();
    }

    void operator()()
    {
		data_->promise_handle_.resume();
	}

	void operator()(const asio::error_code& ec)
	{
#if AWAITABLE_TASKS_EXCEPTION
		if (ec) {
			data_->promise_handle_.set_exception(std::make_exception_ptr(asio::system_error(ec)));
		}
		else {
			data_->value_ = std::move(ec);
		}
#else
		data_->value_ = std::move(ec);
#endif
		data_->promise_handle_.resume();
	}

  //private:
	struct data {
		awaitable_tasks::promise_handle<result_type_t> promise_handle_;
		awaitable_tasks::task<result_type_t> task_;
		result_type_t value_;
	};
	std::shared_ptr<data > data_;
  };

  // Ensure any exceptions thrown from the handler are propagated back to the
  // caller via the future.
  template <typename Function, typename T>
  void asio_handler_invoke(Function f, promise_handler<T>* h)
  {
    auto p(h->data_);
#if AWAITABLE_TASKS_EXCEPTION
	try
    {
      f();
    }
	catch (...)
    {
      p->promise_handle_.set_exception(std::current_exception());
    }
#else
	f();
#endif
  }

} // namespace detail

#if !defined(GENERATING_DOCUMENTATION)

// Handler traits specialisation for promise_handler.
template <typename T>
class async_result<detail::promise_handler<T> >
{
public:
  // The initiating function will return a future.
  typedef awaitable_tasks::task<typename detail::promise_handler<T>::result_type_t> type;

  // Constructor creates a new promise for the async operation, and obtains the
  // corresponding future.
  explicit async_result(detail::promise_handler<T>& h)
  {
    value_ = std::move(h.data_->task_);
  }

  // Obtain the future to be returned from the initiating function.
  type get() { return std::move(value_); }

private:
  type value_;
};

// Handler type specialisation for use_task.
template <typename Allocator, typename ReturnType>
struct handler_type<use_task_t<Allocator>, ReturnType()>
{
  typedef detail::promise_handler<void> type;
};

// Handler type specialisation for use_task.
template <typename Allocator, typename ReturnType, typename Arg1>
struct handler_type<use_task_t<Allocator>, ReturnType(Arg1)>
{
  typedef detail::promise_handler<Arg1> type;
};

// Handler type specialisation for use_task.
template <typename Allocator, typename ReturnType>
struct handler_type<use_task_t<Allocator>,
    ReturnType(asio::error_code)>
{
  typedef detail::promise_handler<void> type;
};

// Handler type specialisation for use_task.
template <typename Allocator, typename ReturnType, typename Arg2>
struct handler_type<use_task_t<Allocator>,
    ReturnType(asio::error_code, Arg2)>
{
  typedef detail::promise_handler<Arg2> type;
};

#endif // !defined(GENERATING_DOCUMENTATION)

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_IMPL_use_task_HPP
