// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <object/interrupt_dispatcher.h>

InterruptDispatcher::InterruptDispatcher() : signals_(0) {
    event_init(&event_, false, EVENT_FLAG_AUTOUNSIGNAL);
}

zx_status_t InterruptDispatcher::UserSignal(uint32_t slot, zx_time_t timestamp) {
    if (slot >= ZX_INTERRUPT_MAX_WAIT_SLOTS)
        return ZX_ERR_INVALID_ARGS;

    size_t size = interrupts_.size();
    for (size_t i = 0; i < size; i++) {
        Interrupt& interrupt = interrupts_[i];
        if (interrupt.slot == slot) {
            if (!(interrupt.flags & ZX_INTERRUPT_VIRTUAL))
                return ZX_ERR_BAD_STATE;
            if (!interrupt.timestamp)
                interrupt.timestamp = timestamp;
            Signal(SIGNAL_MASK(slot), true);
            return ZX_OK;
        }
    }

    return ZX_ERR_NOT_FOUND;
}

zx_status_t InterruptDispatcher::Wait(uint64_t* out_signals) {
    while (true) {
        uint64_t signals = signals_.exchange(0);
        if (signals) {
            if (signals & SIGNAL_MASK(ZX_INTERRUPT_CANCEL)) {
                return ZX_ERR_CANCELED;
            }
            PostWait(signals);
            *out_signals = signals;
            return ZX_OK;
        }

        PreWait();
        zx_status_t status = event_wait_deadline(&event_, ZX_TIME_INFINITE, true);
        if (status != ZX_OK) {
            return status;
        }
    }
}

int InterruptDispatcher::Signal(uint64_t signals, bool resched) {
    signals_.fetch_or(signals);
    return event_signal_etc(&event_, resched, ZX_OK);
}

int InterruptDispatcher::Cancel() {
    return Signal(SIGNAL_MASK(ZX_INTERRUPT_CANCEL), true);
}
