#include <iostream>
#include <vector>
#include <atomic>
#include <algorithm>
#include <functional>

#if defined(__MINGW32__) && !defined(_GLIBCXX_HAS_GTHREADS)
// MinGW (win32 thread model) does not provide C++11 <thread> and <mutex>.
// We provide drop-in Win32 implementations so the simulation builds cleanly.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

namespace std {
    class mutex {
    private:
        CRITICAL_SECTION cs;
    public:
        mutex() { InitializeCriticalSection(&cs); }
        ~mutex() { DeleteCriticalSection(&cs); }
        mutex(const mutex&) = delete;
        mutex& operator=(const mutex&) = delete;

        void lock() { EnterCriticalSection(&cs); }
        void unlock() { LeaveCriticalSection(&cs); }
    };

    template <typename Mutex>
    class lock_guard {
    private:
        Mutex& mtx;
    public:
        explicit lock_guard(Mutex& m) : mtx(m) { mtx.lock(); }
        ~lock_guard() { mtx.unlock(); }
        lock_guard(const lock_guard&) = delete;
        lock_guard& operator=(const lock_guard&) = delete;
    };

    class thread {
    private:
        HANDLE hThread;
        struct RunnerBase {
            virtual ~RunnerBase() {}
            virtual void run() = 0;
        };
        template <typename F>
        struct Runner : RunnerBase {
            F func;
            Runner(F&& f) : func(std::forward<F>(f)) {}
            void run() override { func(); }
        };

        static unsigned int __stdcall threadEntry(void* arg) {
            RunnerBase* runner = static_cast<RunnerBase*>(arg);
            runner->run();
            delete runner;
            return 0;
        }

    public:
        thread() : hThread(NULL) {}

        template <typename Function, typename... Args>
        explicit thread(Function&& f, Args&&... args) {
            auto task = std::bind(std::forward<Function>(f), std::forward<Args>(args)...);
            using TaskType = decltype(task);
            RunnerBase* runner = new Runner<TaskType>(std::move(task));
            hThread = (HANDLE)_beginthreadex(NULL, 0, threadEntry, runner, 0, NULL);
        }

        ~thread() {
            if (joinable()) {
                CloseHandle(hThread);
            }
        }

        thread(const thread&) = delete;
        thread& operator=(const thread&) = delete;

        thread(thread&& other) noexcept : hThread(other.hThread) {
            other.hThread = NULL;
        }

        thread& operator=(thread&& other) noexcept {
            if (this != &other) {
                if (joinable()) {
                    CloseHandle(hThread);
                }
                hThread = other.hThread;
                other.hThread = NULL;
            }
            return *this;
        }

        bool joinable() const {
            return hThread != NULL;
        }

        void join() {
            if (joinable()) {
                WaitForSingleObject(hThread, INFINITE);
                CloseHandle(hThread);
                hThread = NULL;
            }
        }
    };
}
#else
#include <thread>
#include <mutex>
#endif

using namespace std;

struct Server {
    const int capacityPerRound = 100;
    const int maxQueue = 500;

    atomic<int> queue{0};
    atomic<long long> legitimateServed{0};
    atomic<long long> attackServed{0};
    atomic<long long> dropped{0};
    mutex mtx;

    void addRequests(int n, bool attacker) {
        lock_guard<mutex> lock(mtx);

        int space = maxQueue - queue.load();
        int accepted = max(0, min(n, space));
        int rejected = n - accepted;

        queue += accepted;
        dropped += rejected;

        // Requests are counted by origin only after they are accepted.
        if (attacker)
            attackServed += accepted;
        else
            legitimateServed += accepted;
    }

    void processRound() {
        lock_guard<mutex> lock(mtx);
        int served = min(capacityPerRound, queue.load());
        queue -= served;
    }
};

void legitimateClient(Server& s, int rounds, int requestsPerRound) {
    for (int i = 0; i < rounds; ++i)
        s.addRequests(requestsPerRound, false);
}

void simulatedDoS(Server& s, int rounds, int attackRate) {
    for (int i = 0; i < rounds; ++i)
        s.addRequests(attackRate, true);
}

void simulatedDDoS(Server& s, int rounds, int sources, int ratePerSource) {
    vector<thread> attackers;

    for (int i = 0; i < sources; ++i) {
        attackers.emplace_back([&s, rounds, ratePerSource]() {
            for (int r = 0; r < rounds; ++r)
                s.addRequests(ratePerSource, true);
        });
    }

    for (auto& t : attackers)
        t.join();
}

void runSimulation(const string& name, int rounds,
                   int legitimateRate, int attackerSources,
                   int attackRatePerSource) {
    Server server;

    // Generate legitimate workload.
    thread legitimate(legitimateClient, ref(server),
                      rounds, legitimateRate);

    if (attackerSources == 1) {
        simulatedDoS(server, rounds, attackRatePerSource);
    } else {
        simulatedDDoS(server, rounds, attackerSources,
                      attackRatePerSource);
    }

    legitimate.join();

    // Simulate server processing after workload generation.
    for (int i = 0; i < rounds; ++i)
        server.processRound();

    cout << "\n=== " << name << " Simulation ===\n";
    cout << "Legitimate accepted: " << server.legitimateServed << '\n';
    cout << "Attack accepted:     " << server.attackServed << '\n';
    cout << "Dropped requests:    " << server.dropped << '\n';
    cout << "Requests left queue: " << server.queue << '\n';
}

int main() {
    const int rounds = 20;
    const int legitimateRate = 10;

    // Safe in-memory DoS simulation:
    runSimulation("DoS", rounds, legitimateRate,
                  1, 250);

    // Safe in-memory DDoS simulation:
    runSimulation("DDoS", rounds, legitimateRate,
                  8, 40);

    return 0;
}