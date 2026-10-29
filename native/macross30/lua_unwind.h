#pragma once
#include <stdint.h>

class MacrossLuaBoundary;
[[noreturn]] void macross_lua_guest_longjmp(void* context, uint32_t aligned_buffer);
struct MacrossLuaJump {
    const MacrossLuaBoundary* target;
};

/* Host lifetime mirrors the active guest errorJmp lifetime. No guest register
 * snapshots here: the original longjmp restores the complete guest ABI. */
class MacrossLuaBoundary {
public:
    MacrossLuaBoundary(void* context, uint32_t buffer);
    ~MacrossLuaBoundary();
    MacrossLuaBoundary(const MacrossLuaBoundary&) = delete;
    MacrossLuaBoundary& operator=(const MacrossLuaBoundary&) = delete;
    bool accepts(const MacrossLuaJump& jump) const;
private:
    friend void macross_lua_guest_longjmp(void*, uint32_t);
    void* context_;
    uint32_t buffer_;
    MacrossLuaBoundary* previous_;
    void (*trampoline_)(void*);
};

