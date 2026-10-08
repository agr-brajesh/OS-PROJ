#pragma once

#include <chrono>
#include <memory>

namespace isp {

class IClock {
public:
    virtual ~IClock() = default;
    virtual std::chrono::steady_clock::time_point now() const = 0;
};

class SystemClock : public IClock {
public:
    std::chrono::steady_clock::time_point now() const override {
        return std::chrono::steady_clock::now();
    }
};

class MockClock : public IClock {
public:
    explicit MockClock(std::chrono::steady_clock::time_point start = std::chrono::steady_clock::time_point{})
        : current_time_(start) {}

    std::chrono::steady_clock::time_point now() const override {
        return current_time_;
    }

    void advance_seconds(int64_t sec) {
        current_time_ += std::chrono::seconds(sec);
    }

    void advance_ms(int64_t ms) {
        current_time_ += std::chrono::milliseconds(ms);
    }

    void set_time(std::chrono::steady_clock::time_point tp) {
        current_time_ = tp;
    }

private:
    std::chrono::steady_clock::time_point current_time_;
};

} // namespace isp
