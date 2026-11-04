#pragma once

#include <cstdint>

struct PPCContext;

void GTA4_Ps4CrashTrace(const char* function, const PPCContext& ctx,
                        const uint8_t* base);
void GTA4_Ps4GraphicsCheckpoint(const char* function, const PPCContext& ctx);
void GTA4_Ps4LoadingStateTrace(const char* phase, uint32_t loading_active,
                               uint32_t screen_index, uint32_t ready,
                               uint32_t complete, uint32_t audio_gate,
                               uint32_t frontend_state);
void GTA4_Ps4TransitionDispatchTrace(const char* phase, uint32_t caller,
                                     uint32_t stored_state,
                                     const PPCContext& ctx);
void GTA4_Ps4WorldStepTrace(const char* step, const PPCContext& ctx);
void GTA4_Ps4WorldPathTrace(const char* phase, uint32_t guest_path,
                            const uint8_t* base, const PPCContext& ctx);
void GTA4_Ps4TimebaseSpinTrace(const PPCContext& ctx);
void GTA4_Ps4GuestMemoryTrace(uint32_t address, const char* operation);
bool GTA4_Ps4GuestMemoryWriteAllowed(uint32_t address, const char* operation);
bool GTA4_Ps4GuestMemoryWriteAllowedContext(uint32_t address,
                                           const char* operation,
                                           const PPCContext& ctx,
                                           uint32_t value);
void GTA4_Ps4GuestMemoryTraceContext(uint32_t address, const char* operation,
                                    uint32_t r1, uint32_t r3, uint32_t lr);
void GTA4_Ps4GuestMemoryTraceContextFull(uint32_t address, const char* operation,
                                        const PPCContext& ctx);
void GTA4_Ps4GuestMemoryTraceContextSite(uint32_t address, const char* operation,
                                        const PPCContext& ctx, const char* file,
                                        unsigned line, const char* function);
void GTA4_Ps4GuestStoreContextTrace(const char* site, uint32_t address,
                                   const PPCContext& ctx, uint32_t value);
void GTA4_Ps4DeviceCreateTrace(const char* phase, uint32_t mode, uint32_t behavior,
                              uint32_t presentation, uint32_t output, uint32_t device,
                              uint32_t status, uint32_t lr);
