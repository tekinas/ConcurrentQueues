#ifndef SPIN_LOCK
#define SPIN_LOCK

#include <atomic>
#include <thread>

namespace util {
class SpinLock {
public:
    bool try_lock() {
        if (lock_flag.test(std::memory_order::relaxed)) return false;
        return not lock_flag.test_and_set(std::memory_order::acquire);
    }

    void lock() {
        constexpr size_t max_checks = 8;
        constexpr auto sleep_dur = std::chrono::nanoseconds{1};
        for (size_t count = 1;
             lock_flag.test(std::memory_order::relaxed) or lock_flag.test_and_set(std::memory_order::acquire); ++count)
            if (count == max_checks) {
                count = 0;
                std::this_thread::sleep_for(sleep_dur);
            }
    }

    void unlock() { lock_flag.clear(std::memory_order::release); }

private:
    std::atomic_flag lock_flag{};
};
}// namespace util

#endif
