// task.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <string>
#include <iostream>
#include "../include/awaitable_tasks.hpp"
#pragma warning(disable : 4100)
int g_data = 42;

template<typename T>
struct Param {
    T value;
    Param() { std::cout << "default ctor" << std::endl; }
    Param(T v) : value(std::move(v)) { std::cout << "value ctor" << std::endl; }
    Param(Param&& rhs) : value(std::move(rhs.value)) { std::cout << "move ctor" << std::endl; }
    Param(const Param& rhs) : value(std::move(rhs.value)) { std::cout << "copy ctor" << std::endl; }
    Param& operator=(Param&& rhs) {
        value = std::move(rhs.value);
        std::cout << "move assign" << std::endl;
        return *this;
    }
    Param& operator=(const Param& rhs) {
        value = rhs.value;
        std::cout << "copy assign" << std::endl;
        return *this;
    }
    ~Param() { std::cout << "dtor" << std::endl; }
};
#define FWD(x) std::forward<decltype(x)>(x)
template<typename T>
auto calc(T&& t) {
    std::cout << t.value << std::endl;
    return 1;
}
template<typename T1, typename T2, typename... Ts>
auto calc(T1&& t1, T2&& t2, Ts&&... ts) {
    return calc(FWD(t1)) + calc(FWD(t2), FWD(ts)...);
}

void test_capture() {
    {
        Param<std::string> v1("122341234123412341234123412341434535345345345345345345345345");
        Param<std::string> v2("dsfdfasdfasdfasdfasdfasdfasdfasdfasdfssdfasdfasdfasdfasdsdfa");
        auto ttt = [](auto... args) -> awaitable_tasks::task<int> {
            auto f = [&] {
                calc(args...);
                return 2;
            };
            co_await awaitable_tasks::ex::suspend_never{};
            return f();
        }(v1, v2, v1);
    }
}

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
        awaitable_tasks::promise_handle<int> old_task_handle;
        { awaitable_tasks::task_holder new_task = old_task_handle.get_task().then(func); }
        old_task_handle.resume();  // do nothing
    }
    {
        awaitable_tasks::promise_handle<int> old_task_handle;
        auto new_task = old_task_handle.get_task().then(func);
        new_task.reset();
        old_task_handle.resume();  // do nothing
    }
    {
        awaitable_tasks::promise_handle<int> old_task_handle;
        old_task_handle.get_task().then(func);
        old_task_handle.resume();
    }

    // make_task then and then
    {
        awaitable_tasks::promise_handle<int> old_task_handle;
        auto old_task = old_task_handle.get_task();
        auto new_task = old_task.then(fund).then(printval);
        old_task_handle.resume();
    }
    // then task_gen
    {
        awaitable_tasks::promise_handle<int> old_task_handle;
        auto old_task = old_task_handle.get_task().then([]() -> short {
            std::cout << "start" << std::endl;
            return 33;
        });

        awaitable_tasks::promise_handle<int> old_task2_handle;
        // lambdas capture error, so set it static
        static awaitable_tasks::task<short> static_task =
            old_task2_handle.get_task().then([]() -> short {
                std::cout << "end" << std::endl;
                return 44;
            });

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
                                v = co_await static_task;
                                std::cout << "get3 " << v << std::endl;
                                return v;
                            });
        old_task_handle.resume();
        old_task2_handle.resume();
    }

    // when_all zip
    {
        awaitable_tasks::promise_handle<int> task_a_handle;
        awaitable_tasks::promise_handle<int> task_b_handle;
        awaitable_tasks::task<int> task_a = task_a_handle.get_task().then(func);
        awaitable_tasks::task<int> task_b = task_b_handle.get_task().then(fund);
        auto new_task = awaitable_tasks::when_all(task_a, task_b).then([](std::tuple<int, int>&) {
            std::cout << "ok " << std::endl;
        });
        task_a_handle.resume();
        task_b_handle.resume();
    }
    // when_all zip
    {
        awaitable_tasks::promise_handle<int> task_a_handle;
        awaitable_tasks::promise_handle<int> task_b_handle;
        awaitable_tasks::task<int> task_a = task_a_handle.get_task().then(func);
        awaitable_tasks::task<int> task_b = task_b_handle.get_task().then(fund);
        auto new_task = awaitable_tasks::when_all(task_a, task_b).then([](std::tuple<int, int>&) {
            std::cout << "ok " << std::endl;
        });
        task_a_handle.resume();
        new_task.reset();
        task_b_handle.resume();
    }
    // when_all map
    {
        awaitable_tasks::promise_handle<int> task_handle_a;
        awaitable_tasks::promise_handle<int> task_handle_b;
        std::vector<awaitable_tasks::task<int>> tasks;
        tasks.emplace_back(task_handle_a.get_task().then(func));
        tasks.emplace_back(task_handle_b.get_task().then(func));
        auto new_task =
            awaitable_tasks::when_all(tasks.begin(), tasks.end()).then([](std::vector<int>&) {
                std::cout << "ok " << std::endl;
            });
        task_handle_a.resume();
        task_handle_b.resume();
    }
    // when_n all
    {
        awaitable_tasks::promise_handle<int> task_handle_a;
        awaitable_tasks::promise_handle<int> task_handle_b;
        awaitable_tasks::promise_handle<int> task_handle_c;
        std::vector<awaitable_tasks::task<int>> tasks;
        tasks.emplace_back(task_handle_a.get_task().then(func));
        tasks.emplace_back(task_handle_b.get_task().then(func));
        tasks.emplace_back(task_handle_c.get_task().then(func));
        auto new_task = awaitable_tasks::when_n(tasks.begin(), tasks.end(), tasks.size())
                            .then([](std::vector<std::pair<size_t, int>>&) {
                                std::cout << "ok " << std::endl;
                            });
        task_handle_a.resume();
        task_handle_b.resume();
        task_handle_c.resume();
    }
    // when_n some
    {
        awaitable_tasks::promise_handle<int> task_handle_a;
        awaitable_tasks::promise_handle<int> task_handle_b;
        awaitable_tasks::promise_handle<int> task_handle_c;
        std::vector<awaitable_tasks::task<int>> tasks;
        tasks.emplace_back(task_handle_a.get_task().then(func));
        tasks.emplace_back(task_handle_b.get_task().then(func));
        tasks.emplace_back(task_handle_c.get_task().then(func));
        auto new_task = awaitable_tasks::when_n(tasks.begin(), tasks.end(), 2)
                            .then([](std::vector<std::pair<size_t, int>>&) {
                                std::cout << "ok " << std::endl;
                            });
        task_handle_a.resume();
        task_handle_b.resume();
        task_handle_c.resume();
    }
    // when_n one
    {
        awaitable_tasks::promise_handle<int> task_handle_a;
        awaitable_tasks::promise_handle<int> task_handle_b;
        awaitable_tasks::promise_handle<int> task_handle_c;
        std::vector<awaitable_tasks::task<int>> tasks;
        tasks.emplace_back(task_handle_a.get_task().then(func));
        tasks.emplace_back(task_handle_b.get_task().then(func));
        tasks.emplace_back(task_handle_c.get_task().then(func));
        auto new_task = awaitable_tasks::when_n(tasks.begin(), tasks.end(), 1)
                            .then([](std::vector<std::pair<size_t, int>>& xx) {
                                std::cout << "ok " << std::endl;
                            })
                            .then([]() { printf("ok"); });
        task_handle_a.resume();
        task_handle_b.resume();
        task_handle_c.resume();
    }
    // when_any
    {
        awaitable_tasks::promise_handle<int> task_handle_a;
        awaitable_tasks::promise_handle<int> task_handle_b;
        awaitable_tasks::promise_handle<int> task_handle_c;
        std::vector<awaitable_tasks::task<int>> tasks;
        tasks.emplace_back(task_handle_a.get_task().then(func));
        tasks.emplace_back(task_handle_b.get_task().then(func));
        tasks.emplace_back(task_handle_c.get_task().then(func));
        auto new_task =
            awaitable_tasks::when_any(tasks.begin(), tasks.end())
                .then([](std::pair<size_t, int>& xx) { std::cout << "ok " << std::endl; });
        task_handle_a.resume();
        task_handle_b.resume();
        task_handle_c.resume();
    }
    // when_any
    {
        awaitable_tasks::promise_handle<int> task_handle_a;
        awaitable_tasks::promise_handle<int> task_handle_b;
        awaitable_tasks::promise_handle<int> task_handle_c;
        std::vector<awaitable_tasks::task<int>> tasks;
        tasks.emplace_back(task_handle_a.get_task().then(func));
        tasks.emplace_back(task_handle_b.get_task().then(func));
        tasks.emplace_back(task_handle_c.get_task().then(func));
        auto new_task = awaitable_tasks::when_any(tasks.begin(), tasks.end())
                            .then([](std::pair<size_t, int>& xx)
                                      -> awaitable_tasks::task<std::pair<size_t, int>> {
                                // will leak, do not use without promise_handle control
                                co_await awaitable_tasks::ex::suspend_never{};
                                std::cout << "ok " << std::endl;
                                return xx;
                            })
                            .then([](std::pair<size_t, int>& t) {});
        task_handle_a.resume();
        task_handle_b.resume();
        task_handle_c.resume();
    }
    return 0;
}
