// The kernel's threads and scheduler. The Xbox has one processor: one
// guest thread runs at a time, the highest-priority ready one, preempted
// when its quantum ends with another of its priority ready, or at once by
// a higher one or a DPC (unless it runs at DISPATCH_LEVEL or above). Each
// guest thread here is a host thread with its own registers; the one that
// holds the processor runs, the others block. Switches happen where lifted
// code ticks (its backward branches, QILIN_TICK) and in the kernel's waits.
#pragma once

#include <chrono>
#include <cstdint>
#include <initializer_list>

#include "guest.hpp"

namespace qilin::host::sched {

using Clock = std::chrono::steady_clock;

// The main thread: the host thread that calls this, already holding the
// processor, with the KTHREAD init_main_thread set up. Starts the clock.
// `call` runs a guest function from the runtime (host::call_guest; passed in
// so the library doesn't need lifted code linked).
using GuestCall = void (*)(rt::Context&, uint32_t, std::initializer_list<uint32_t>);
void init_main(rt::Context& c, uint32_t kthread, GuestCall call);
bool started();
bool on_main_thread();  // the host thread the runtime started on (SDL's events go there)

// The calling guest thread gives up the processor until `t` (Swap's vblank
// wait, which blocks the thread on the console), letting others run.
void sleep_until(Clock::time_point t);

// Sets a KEVENT in guest memory from runtime code (a DirectSound packet's
// completion event), waking what waits on it, as KeSetEvent.
void set_event(uint32_t event);

// The interrupt clock: 100 ns since the runtime started.
uint64_t interrupt_time();

// The objects behind data exports the scheduler keeps (by ordinal).
void set_data_export(uint32_t ordinal, uint32_t addr);

// KeSetTimer[Ex] and KeCancelTimer: armed timers fire on the clock.
uint32_t set_timer(rt::Context& c, uint32_t timer, int64_t due, uint32_t period_ms, uint32_t dpc);
bool cancel_timer(rt::Context& c, uint32_t timer);

// A wait from runtime code on behalf of the calling guest thread (a
// critical section's event): KeWaitForSingleObject with no timeout.
void wait_object(uint32_t object);

// NtClose on a kernel object's handle: true if it was one.
bool close_handle(uint32_t handle);

void register_kernel_sched();

}  // namespace qilin::host::sched
