// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <kernel/event.h>
#include <zircon/types.h>
#include <fbl/atomic.h>
#include <object/dispatcher.h>
#include <sys/types.h>

#define SIGNAL_MASK(signal) (1ul << (signal))

// Note that unlike most Dispatcher subclasses, this one is further
// subclassed, and so cannot be final.
class InterruptDispatcher : public Dispatcher {
public:
    InterruptDispatcher& operator=(const InterruptDispatcher &) = delete;

    zx_obj_type_t get_type() const final { return ZX_OBJ_TYPE_INTERRUPT; }

    // Signal the IRQ from non-IRQ state in response to a user-land request.
    virtual zx_status_t UserSignal(uint32_t slot, zx_time_t timestamp) = 0;

    virtual zx_status_t Bind(uint32_t slot, uint32_t vector, uint32_t options) = 0;
    virtual zx_status_t WaitForInterrupt(uint64_t* out_slots) = 0;
    virtual zx_status_t GetTimeStamp(uint32_t slot, zx_time_t* out_timestamp) = 0;
    virtual void PreWait() = 0;
    virtual void PostWait(uint64_t signals) = 0;

protected:
    InterruptDispatcher();

    zx_status_t Wait(uint64_t* out_signals);
    int Signal(uint64_t signals, bool resched = false);
    int Cancel();

private:
    event_t event_;
    fbl::atomic<uint64_t> signals_;
};
