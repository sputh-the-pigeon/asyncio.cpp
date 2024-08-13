#include <coroutine>
#include <algorithm>
#include <vector>
#include <tuple>
#include <chrono>

using namespace std;

template<typename T, typename U>
tuple<bool, int> vector_find(vector<T>& v, U fn) {
    for (int i = 0; i < v.size(); i++) {
        if (fn(v[i])) return { true, i };
    };
    return {false, 0};
};

namespace asyncio {

    class EventLoop {
    private:
        vector<coroutine_handle<>> coros;
        vector<tuple<coroutine_handle<>, coroutine_handle<>>> suspended;
        vector<tuple<vector<coroutine_handle<>>, coroutine_handle<>>> gather_sus;
        vector<tuple<int, chrono::time_point<chrono::steady_clock>, coroutine_handle<>>> sleeping;
        vector<tuple<bool, coroutine_handle<>>> futures;

    public:
        EventLoop() : coros{}, suspended{} {};

        void add(coroutine_handle<> coro) {
            coros.push_back(coro);
        };

        void suspended_add(coroutine_handle<> suspend, coroutine_handle<> current) {
            suspended.push_back({ suspend, current });
            coros.push_back(current);
        };

        void gather_add(vector<coroutine_handle<>> gather, coroutine_handle<> current) {
            gather_sus.push_back({ gather, current });
            coros.push_back(current);
        };

        void future_add(bool& done, coroutine_handle<> current) {
            futures.push_back({ done, current });
        };

        void sleep_add(int seconds, chrono::time_point<chrono::steady_clock> current, coroutine_handle<> handle) {
            sleeping.push_back({ seconds, current, handle });
        };

        void run() {
            while (!all_of(coros.begin(), coros.end(), [](auto coro) -> bool { return coro.done(); })) {
                for (int i = 0; i < gather_sus.size(); i++) {
                    if (all_of(get<0>(gather_sus[i]).begin(), get<0>(gather_sus[i]).end(), [](auto& elem) -> bool { return elem.done(); })) {
                        gather_sus.erase(gather_sus.begin() + i);
                    };
                };

                for (int i = 0; i < sleeping.size(); i++) {
                    if (((chrono::duration<double>)(chrono::steady_clock::now() - get<1>(sleeping[i]))).count() >= get<0>(sleeping[i])) {
                        sleeping.erase(sleeping.begin() + i);
                    };
                };

                for (int i = 0; i < futures.size(); i++) {
                    if (get<0>(futures[i])) {
                        futures.erase(futures.begin() + i);
                    };
                };

                for (int i = 0; i < coros.size(); i++) {
                    auto coro = coros[i];
                    if (!(coro.done()) && !get<0>(vector_find(this->suspended, [&coro](auto& elem) -> bool {
                            return get<0>(elem) == coro;
                        })) && !get<0>(vector_find(gather_sus, [&coro](auto& elem) -> bool {
                            return get<1>(elem) == coro;
                        })) && !get<0>(vector_find(sleeping, [&coro](auto& elem) -> bool {
                            return get<2>(elem) == coro;
                        })) && !get<0>(vector_find(futures, [&coro](auto& elem) -> bool {
                            return get<1>(elem) == coro;
                         })))
                    {
                        coro.resume();
                        auto d = vector_find(this->suspended, [&coro](auto& elem) -> bool {
                            if (get<1>(elem) == coro) return true;
                            });
                        if (coro.done() && get<0>(d))
                        {
                            suspended.erase(suspended.begin() + get<1>(d));
                            coros.erase(coros.begin() + i);
                        };
                    };
                };

            };
        };
    };

    EventLoop LOOP = EventLoop{};

    EventLoop& new_event_loop(){
        LOOP = EventLoop{};
        return LOOP;
    };

    EventLoop& get_running_event_loop() {
        return LOOP;
    }

    template<typename T>
    struct promise;

    template<typename T>
    struct coroutine : coroutine_handle<promise<T>> {
        using promise_type = asyncio::promise<T>;

        T resval;

        bool await_ready() {
            return false;
        }

        void await_suspend(coroutine_handle<> h) {
            //h -> This handle is of the coroutine that is enclosing the co_awaited coroutine
            LOOP.suspended_add(h, *(coroutine_handle<>*)this);
        }

        T await_resume() {
            this->resval = this->promise().resval;
            //this->destroy();
            return this->resval;
        }
    };

    template<typename T>
    struct promise {
        T resval;

        coroutine<T> get_return_object() {
            return { coroutine<T>::from_promise(*this) };
        };

        suspend_always initial_suspend() noexcept {
            return {};
        };
        suspend_always final_suspend() noexcept {
            return {};
        };
        void return_value(T val) {
            this->resval = val;
        }
        void unhandled_exception() {};
    };

    template<typename... T>
    bool check_all(T... args) {
        bool all = true;
        (([&all]<typename U>(U arg) -> void { if (arg == false) all = false; })(args), ...);
        return all;
    };

    template<typename... T>
    bool check_all(tuple<coroutine<T>...>& tup) {
        bool all = true;
        apply([&all]<typename... T>(T&... args) -> void {
            (([&all]<typename U>(U arg) -> void { if (arg.done() == false) all = false; })(args), ...);
        }, tup);
        return all;
    };

    template<typename ...T>
    class gather : coroutine_handle<promise<tuple<T...>>> {
    public:
        tuple<coroutine<T>...> coro_store;

        gather(coroutine<T>... coros) {
            ((LOOP.add(coros)), ...);
            this->coro_store = make_tuple(coros...);
        };

        bool await_ready() { return false; };

        void await_suspend(coroutine_handle<> h) {
            vector<coroutine_handle<>> v{};
            apply([&v]<typename... T>(T&... args) -> void {
                ((v.push_back((coroutine_handle<>)args)), ...);
            }, coro_store);
            LOOP.gather_add(v, h);
        };

        tuple<T...> await_resume() {
            tuple<T...> tup;
            apply([&tup]<typename... T>(T&... args) -> void {
                tup = make_tuple(args.promise().resval...);
            }, coro_store);
            return tup;
        };

    };

    template<typename T>
    class task : public gather<T> {
    public:

        task(coroutine<T> coro) : gather<T>(coro) {};

        T await_resume() {
            return get<0>(gather<T>::await_resume());
        };
    };

    template<typename T>
    task<T> create_task(coroutine<T> coro) {
        return task{ coro };
    };

    template<typename T>
    class future : coroutine_handle<promise<T>> {
    public:
        bool fulfilled = false;
        T result;

        future() {

        };

        void callback(future&) {};

        void add_done_callback(void callb(future&)) {
            callback = callb;
            if (fulfilled) {
                callback(*this);
            };
        };

        void set_result(T res) {
            result = res;
            fulfilled = true;
            callback(*this);
        };

        void remove_done_callback() { callback = (void(future&) {};) };

        bool await_ready() { return fulfilled; };

        void await_suspend(coroutine_handle<> h) {
            LOOP.future_add(fulfilled, h);
        };

        T await_resume() { return result; };

    };

    class sleep : coroutine_handle<promise<int>> {
    public:
        int seconds;

        sleep(int seconds) : seconds(seconds) { };

        bool await_ready() { return false; }

        void await_suspend(coroutine_handle<> h) {
            LOOP.sleep_add(seconds, chrono::steady_clock::now(), h);
        };

        void await_resume() {};
    };
};
