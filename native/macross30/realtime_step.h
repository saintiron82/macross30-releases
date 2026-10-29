#pragma once
#include <stdint.h>

// Opt-in timing experiment. No guest state is changed by the producer/probe.
extern "C" void macross_realtime_step_begin(uint64_t thread_id);
extern "C" void macross_realtime_step_end(uint64_t thread_id);
extern "C" void macross_realtime_step_consumer(uint64_t thread_id, double incoming);
extern "C" double macross_realtime_step_value(uint64_t thread_id, uint32_t pc,
    uint64_t constant_ea, double original, double incoming);
extern "C" double macross_variable_step_floor(uint64_t thread_id,
    double computed, double original_floor);

class MacrossRealtimeStepFrame {
    uint64_t thread_id_;
public:
    explicit MacrossRealtimeStepFrame(uint64_t thread_id) : thread_id_(thread_id) {
        macross_realtime_step_begin(thread_id_);
    }
    ~MacrossRealtimeStepFrame() { macross_realtime_step_end(thread_id_); }
    MacrossRealtimeStepFrame(const MacrossRealtimeStepFrame&) = delete;
    MacrossRealtimeStepFrame& operator=(const MacrossRealtimeStepFrame&) = delete;
};
