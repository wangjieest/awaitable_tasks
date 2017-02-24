// task.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <string>
#include <iostream>
#include "../include/awaitable_tasks.hpp"

int g_data = 42;

int main() {
    using fn = int (*)();
    fn func = []() -> int {
        std::cout << ++g_data << " in func " << std::endl;
        return g_data;
    };
    fn fund = []() -> int {
        std::cout << ++g_data << " in fund " << std::endl;
        return g_data;
    };
    auto func2 = [&]() -> int {
        std::cout << ++g_data << " in func2 " << std::endl;
        return g_data;
    };
    {
        auto old_task = awaitable_tasks::make_task();
        auto old_task_handle = old_task.get_promise_handle();
        { auto new_task = old_task.then(func).set_self_release(); }
        old_task_handle.resume();
        // auto v = new_task.cur_value_ref();
    }

    // for C interface
    {
        auto old_task = awaitable_tasks::make_task();
        auto old_task_handle = old_task.get_promise_handle().lock();
        auto new_task = old_task.then(func).set_self_release();
        old_task_handle->resume();
        auto v = new_task.cur_value_ref();
		old_task_handle->unlock();
    }
    // make_task then and then
    {
        auto old_task = awaitable_tasks::make_task(func);
        auto old_task_handle = old_task.get_promise_handle();
        auto new_task = old_task.then(fund).set_self_release();
        old_task_handle.resume();
        auto v = new_task.cur_value_ref();
    }
    // then task_gen
    {
        auto old_task = awaitable_tasks::make_task([]() -> short {
            std::cout << "start" << std::endl;
            return 33;
        });

        // lambdas capture error, so set it static
        static auto old_task2 = awaitable_tasks::make_task([]() -> short {
            std::cout << "end" << std::endl;
            return 44;
        });

        auto old_task2_handle = old_task2.get_promise_handle();
        auto old_task_handle = old_task.get_promise_handle();
        auto new_task = old_task.then(func2)
                            .then([](int& v) -> short {
                                std::cout << "get1 " << v << std::endl;
                                return 33;
                            })
                            // [t = std::move(old_task2)]{ co_await t;}
                            // would be an error
                            .then([](short& v) -> awaitable_tasks::task<int> {
                                std::cout << "get2 " << v << std::endl;
                                // usually this drived async
                                v = co_await old_task2;
                                std::cout << "get3 " << v << std::endl;
                                return v;
                            })
                            .set_self_release();
        old_task_handle.resume();
        old_task2_handle.resume();
        auto v = new_task.cur_value_ref();
    }
    // then task_gen
    {
        auto old_task = awaitable_tasks::make_task(func);
        auto old_task_handle = old_task.get_promise_handle();
        auto new_task = old_task.then(func2)
                            .then([](int& v) -> short {
                                std::cout << "get1 " << v << std::endl;
                                return 33;
                            })
                            .then([](short& v) -> awaitable_tasks::task<int> {
                                std::cout << "get2 " << v << std::endl;
                                co_await awaitable_tasks::ex::suspend_always{};
                                std::cout << "get3 " << v << std::endl;
                                return 55;
                            })
                            .set_self_release();
        old_task_handle.resume();
        new_task.get_promise_handle().resume();
        auto v = new_task.cur_value_ref();
    }
    // when_all zip
    {
        auto task_a = awaitable_tasks::make_task(func);
        auto task_b = awaitable_tasks::make_task(func);
        auto task_a_handle = task_a.get_promise_handle();
        auto task_b_handle = task_b.get_promise_handle();
        auto new_task = awaitable_tasks::when_all(task_a, task_b)
                            .then([](std::tuple<int, int>&) { std::cout << "ok " << std::endl; })
                            .set_self_release();
        task_a_handle.resume();
        task_b_handle.resume();
        auto v = new_task.cur_value_ref();
    }
    // when_all map
    {
        std::vector<awaitable_tasks::task<int>> tasks;
        tasks.emplace_back(awaitable_tasks::make_task(func));
        tasks.emplace_back(awaitable_tasks::make_task(func));
        auto task_handle_a = tasks[0].get_promise_handle();
        auto task_handle_b = tasks[1].get_promise_handle();
        auto new_task = awaitable_tasks::when_all(tasks.begin(), tasks.end())
                            .then([](std::vector<int>&) { std::cout << "ok " << std::endl; })
                            .set_self_release();
        task_handle_a.resume();
        task_handle_b.resume();
        auto v = new_task.cur_value_ref();
    }
    // when_n all
    {
        std::vector<awaitable_tasks::task<int>> tasks;
        tasks.emplace_back(awaitable_tasks::make_task(func));
        tasks.emplace_back(awaitable_tasks::make_task(func));
        tasks.emplace_back(awaitable_tasks::make_task(func));
        auto task_handle_a = tasks[0].get_promise_handle();
        auto task_handle_b = tasks[1].get_promise_handle();
        auto task_handle_c = tasks[2].get_promise_handle();
        auto new_task =
            awaitable_tasks::when_n(tasks.begin(), tasks.end(), tasks.size())
                .then([](std::vector<std::pair<size_t, int>>&) { std::cout << "ok " << std::endl; })
                .set_self_release();
        task_handle_a.resume();
        task_handle_b.resume();
        task_handle_c.resume();
        auto v = new_task.cur_value_ref();
    }
    // when_n some
    {
        std::vector<awaitable_tasks::task<int>> tasks;
        tasks.emplace_back(awaitable_tasks::make_task(func));
        tasks.emplace_back(awaitable_tasks::make_task(func));
        tasks.emplace_back(awaitable_tasks::make_task(func));
        auto task_handle_a = tasks[0].get_promise_handle();
        auto task_handle_b = tasks[1].get_promise_handle();
        auto task_handle_c = tasks[2].get_promise_handle();
        {
            auto new_task = awaitable_tasks::when_n(tasks.begin(), tasks.end(), 2)
                                .then([](std::vector<std::pair<size_t, int>>&) {
                                    std::cout << "ok " << std::endl;
                                })
                                .set_self_release();
            task_handle_a.resume();
            task_handle_b.resume();
            auto v = new_task.cur_value_ref();
        }
        task_handle_c.resume();
    }
    // when_n one
    {
        std::vector<awaitable_tasks::task<int>> tasks;
        tasks.emplace_back(awaitable_tasks::make_task(func));
        tasks.emplace_back(awaitable_tasks::make_task(func));
        tasks.emplace_back(awaitable_tasks::make_task(func));
        auto task_handle_a = tasks[0].get_promise_handle();
        auto task_handle_b = tasks[1].get_promise_handle();
        auto task_handle_c = tasks[2].get_promise_handle();
        {
            auto new_task = awaitable_tasks::when_n(tasks.begin(), tasks.end(), 1)
                                .then([](std::vector<std::pair<size_t, int>>& xx) {
                                    std::cout << "ok " << std::endl;
                                })
                                .then([]() { printf("ok"); })
                                .set_self_release();
            task_handle_a.resume();
            auto v = new_task.cur_value_ref();
			task_handle_b.resume();
		}
        task_handle_c.resume();
    }
    // when_any
    {
        std::vector<awaitable_tasks::task<int>> tasks;
        tasks.emplace_back(awaitable_tasks::make_task(func));
        tasks.emplace_back(awaitable_tasks::make_task(func));
        tasks.emplace_back(awaitable_tasks::make_task(func));
        auto task_handle_a = tasks[0].get_promise_handle();
        auto task_handle_b = tasks[1].get_promise_handle();
        auto task_handle_c = tasks[2].get_promise_handle();
        {
            auto new_task =
                awaitable_tasks::when_any(tasks.begin(), tasks.end())
                    .then([](std::pair<size_t, int>& xx) { std::cout << "ok " << std::endl; })
                    .set_self_release();
            task_handle_a.resume();
            auto v = new_task.cur_value_ref();
			task_handle_b.resume();
		}
        task_handle_c.resume();
    }
    // when_any
    {
        std::vector<awaitable_tasks::task<int>> tasks;
        tasks.emplace_back(awaitable_tasks::make_task(func));
        tasks.emplace_back(awaitable_tasks::make_task(func));
        tasks.emplace_back(awaitable_tasks::make_task(func));
        auto task_handle_a = tasks[0].get_promise_handle();
        auto task_handle_b = tasks[1].get_promise_handle();
        auto task_handle_c = tasks[2].get_promise_handle();
        {
            auto new_task = awaitable_tasks::when_any(tasks.begin(), tasks.end())
                                .then([](std::pair<size_t, int>& xx)
                                          -> awaitable_tasks::task<std::pair<size_t, int>> {
                                    co_await awaitable_tasks::ex::suspend_never{};
                                    std::cout << "ok " << std::endl;
                                    return xx;
                                })
                                .set_self_release();
            task_handle_a.resume();
            auto v = new_task.cur_value_ref();
			task_handle_b.resume();
		}
        task_handle_c.resume();
    }
    return 0;
}
