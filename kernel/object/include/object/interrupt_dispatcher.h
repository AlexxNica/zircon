// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <kernel/event.h>
#include <zircon/types.h>
#include <fbl/atomic.h>
#include <fbl/mutex.h>
#include <fbl/vector.h>
#include <object/dispatcher.h>
#include <sys/types.h>

#define SIGNAL_MASK(signal) (1ul << (signal))

// Note that unlike most Dispatcher subclasses, this one is further
// subclassed, and so cannot be final.
class InterruptDispatcher : public Dispatcher {
public:
    InterruptDispatcher& operator=(const InterruptDispatcher &) = delete;

    zx_obj_type_t get_type() const final { return ZX_OBJ_TYPE_INTERRUPT; }

    virtual zx_status_t Bind(uint32_t slot, uint32_t vector, uint32_t options) = 0;

    // Signal the IRQ from non-IRQ state in response to a user-land request.
    zx_status_t UserSignal(uint32_t slot, zx_time_t timestamp);
    zx_status_t WaitForInterrupt(uint64_t* out_slots);
    zx_status_t GetTimeStamp(uint32_t slot, zx_time_t* out_timestamp);

protected:
    virtual void PreWait() = 0;
    virtual void PostWait(uint64_t signals) = 0;
    virtual void MaskInterrupt(uint32_t vector) = 0;
    virtual void UnmaskInterrupt(uint32_t vector) = 0;
    virtual zx_status_t RegisterInterruptHandler(uint32_t vector, void* data) = 0;
    virtual void UnregisterInterruptHandler(uint32_t vector) = 0;

    zx_status_t AddSlot(uint32_t slot, uint32_t vector, bool is_maskable, bool is_virtual);

protected:
    InterruptDispatcher();

    void on_zero_handles() final;

    int Signal(uint64_t signals, bool resched = false);

protected:
    struct Interrupt {
        InterruptDispatcher* dispatcher;
        zx_time_t timestamp;
        uint32_t vector;
        uint32_t slot;
        bool is_maskable;
        bool is_virtual;
    };

    // interrupts bound to this dispatcher
    fbl::Vector<Interrupt> interrupts_ TA_GUARDED(lock_);
    fbl::Mutex lock_;
    // currently signaled interrupt slots
    uint64_t current_slots_;

private:
    event_t event_;
    fbl::atomic<uint64_t> signals_;
};
