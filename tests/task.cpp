// task.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <string>
#include <iostream>
#include "../include/awaitable_tasks.hpp"
#pragma warning(disable : 4100)
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
    auto func2 = []() -> int {
        std::cout << ++g_data << " in func2 " << std::endl;
        return g_data;
    };
    auto printval = [](int v) {
        std::cout << v << std::endl;
        return v;
    };
    {
        awaitable::promise_handle<void> task_handle;
        awaitable::task<void> t = task_handle.get_task().then([] {});
        auto tt = t.then(func);
        task_handle.resume();
    }
    // leak test
    {
        awaitable::task<int> t = [](int xx) -> awaitable::task<int> {
            std::cout << "befroe " << std::endl;
            co_await awaitable::ex::suspend_never{};
            std::cout << "after " << std::endl;
            return xx;
        }(2);
    }
    {
        awaitable::promise_handle<int> old_task_handle;
        old_task_handle.get_task().then(func);
        old_task_handle.resume();  // will do
    }
    {
        awaitable::promise_handle<int> old_task_handle;
        { awaitable::task_holder new_task = old_task_handle.get_task().then(func); }
        old_task_handle.resume();  // do nothing
    }
    {
        awaitable::promise_handle<int> old_task_handle;
        auto new_task = old_task_handle.get_task().then(func);
        new_task.reset();
        old_task_handle.resume();  // do nothing
    }

    // make_task then and then
    {
        awaitable::promise_handle<int> old_task_handle;
        old_task_handle.get_task().then(fund).then(printval);
        old_task_handle.resume();
    }

    // when_all zip
    {
        awaitable::promise_handle<int> handle_a;
        awaitable::promise_handle<int> handle_b;
        awaitable::task<int> task_a = handle_a.get_task().then(func);
        awaitable::task<int> task_b = handle_b.get_task().then(fund);
        awaitable::when_all(task_a, task_b).then([](std::tuple<int, int>&) {
            std::cout << "ok " << std::endl;
        });
        handle_a.resume();
        handle_b.resume();
    }
    // when_all zip
    {
        awaitable::promise_handle<int> handle_a;
        awaitable::promise_handle<int> handle_b;
        awaitable::task<int> task_a = handle_a.get_task().then(func);
        awaitable::task<int> task_b = handle_b.get_task().then(fund);
        awaitable::when_all(task_a, task_b).then([](std::tuple<int, int>&) {
            std::cout << "ok " << std::endl;
        });
        handle_a.resume();
        // leave task_b_handle
    }
    // when_all zip
    {
        awaitable::promise_handle<int> handle_a;
        awaitable::promise_handle<int> handle_b;
        awaitable::task<int> task_a = handle_a.get_task().then(func);
        awaitable::task<int> task_b = handle_b.get_task().then(fund);
        auto new_task = awaitable::when_all(task_a, task_b).then([](std::tuple<int, int>&) {
            std::cout << "will not output " << std::endl;
        });
        handle_a.resume();
        new_task.reset();
        handle_b.resume();
    }
    // when_all map
    {
        awaitable::promise_handle<int> handle_a;
        awaitable::promise_handle<int> handle_b;
        std::vector<awaitable::task<int>> tasks;
        tasks.emplace_back(handle_a.get_task().then(func));
        tasks.emplace_back(handle_b.get_task().then(func));
        awaitable::when_all(tasks).then([](std::vector<int>&) { std::cout << "ok " << std::endl; });
        handle_a.resume();
        handle_b.resume();
    }
    // when_n all
    {
        awaitable::promise_handle<int> handle_a;
        awaitable::promise_handle<int> handle_b;
        awaitable::promise_handle<int> handle_c;
        std::vector<awaitable::task<int>> tasks;
        tasks.emplace_back(handle_a.get_task().then(func));
        tasks.emplace_back(handle_b.get_task().then(func));
        tasks.emplace_back(handle_c.get_task().then(func));
        awaitable::when_n(tasks).then([](std::vector<std::pair<size_t, int>>&) {
            std::cout << "ok " << std::endl;
        });
        handle_a.resume();
        handle_b.resume();
        handle_c.resume();
    }
    // when_n some
    {
        awaitable::promise_handle<int> handle_a;
        awaitable::promise_handle<int> handle_b;
        awaitable::promise_handle<int> handle_c;
        std::vector<awaitable::task<int>> tasks;
        tasks.emplace_back(handle_a.get_task().then(func));
        tasks.emplace_back(handle_b.get_task().then(func));
        tasks.emplace_back(handle_c.get_task().then(func));
        awaitable::when_n(tasks, 2).then([](std::vector<std::pair<size_t, int>>&) {
            std::cout << "ok " << std::endl;
        });
        handle_a.resume();
        handle_b.resume();
        handle_c.resume();
    }
    // when_n one
    {
        awaitable::promise_handle<int> handle_a;
        awaitable::promise_handle<int> handle_b;
        awaitable::promise_handle<int> handle_c;
        std::vector<awaitable::task<int>> tasks;
        tasks.emplace_back(handle_a.get_task().then(func));
        tasks.emplace_back(handle_b.get_task().then(func));
        tasks.emplace_back(handle_c.get_task().then(func));
        awaitable::when_n(tasks, 1)
            .then([](std::vector<std::pair<size_t, int>>& xx) { std::cout << "ok " << std::endl; })
            .then([]() { printf("ok"); });
        handle_a.resume();
        handle_b.resume();
        // leave task_handle_c
    }
    // when_any
    {
        awaitable::promise_handle<int> handle_a;
        awaitable::promise_handle<int> handle_b;
        awaitable::promise_handle<int> handle_c;
        std::vector<awaitable::task<int>> tasks;
        tasks.emplace_back(handle_a.get_task().then(func));
        tasks.emplace_back(handle_b.get_task().then(func));
        tasks.emplace_back(handle_c.get_task().then(func));
        awaitable::when_any(tasks).then([](std::pair<size_t, int>& xx) {
            std::cout << "ok " << std::endl;
        });
        handle_a.resume();
        handle_b.resume();
        // leave handle_c
    }
#if defined(AWAITTASK_ENABLE_THEN_TASK)
    // do not use
    // then task_gen
    {
        awaitable::promise_handle<int> old_handle;

        // lambdas capture error, so set it static
        awaitable::promise_handle<int> new_handle;
        static awaitable::task<short> static_task = new_handle.get_task().then([]() -> short {
            std::cout << "end" << std::endl;
            return 44;
        });

        old_handle.get_task()
            .then([]() -> short {
                std::cout << "start" << std::endl;
                return 33;
            })
            .then(func2)
            .then([](int& v) -> short {
                std::cout << "get1 " << v << std::endl;
                return 33;
            })
            // [t = std::move(old_task2)]{ co_await t;}
            // would be an error
            .then([](short& v) -> awaitable::task<int> {
                std::cout << "get2 " << v << std::endl;
                // usually this drived async
                v = co_await static_task;
                std::cout << "get3 " << v << std::endl;
                return v;
            })  // will leak
            .then([] {});
        old_handle.resume();
        new_handle.resume();
    }

    // then generate task and then
    {
        // will leak
        awaitable::promise_handle<void> handle;
        awaitable::promise_handle<int> handle_a;
        awaitable::promise_handle<int> handle_b;
        awaitable::promise_handle<int> handle_c;
        std::vector<awaitable::task<int>> tasks;
        tasks.emplace_back(handle_a.get_task().then(func));
        tasks.emplace_back(handle_b.get_task().then(func));
        tasks.emplace_back(handle_c.get_task().then(func));
        awaitable::when_any(tasks)
            .then([&](std::pair<size_t, int>& xx) -> awaitable::task<std::pair<size_t, int>> {
                std::cout << "before " << std::endl;
                co_await handle.get_awaitable();
                std::cout << "after " << std::endl;
                return xx;
            })
            .then([](std::pair<size_t, int>& xx) { std::cout << "finished" << std::endl; });
        handle_a.resume();
        handle_b.resume();
        handle.resume();
    }
#endif
    return 0;
}
