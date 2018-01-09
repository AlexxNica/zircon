// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <object/interrupt_dispatcher.h>

#include <fbl/auto_lock.h>

InterruptDispatcher::InterruptDispatcher() : signals_(0) {
    event_init(&event_, false, EVENT_FLAG_AUTOUNSIGNAL);
}

zx_status_t InterruptDispatcher::AddSlot(uint32_t slot, uint32_t vector, bool is_maskable,
                                         bool is_virtual) {
    size_t index = interrupts_.size();

    for (size_t i = 0; i < index; i++) {
        const auto& interrupt = interrupts_[i];
        if (interrupt.slot == slot)
            return ZX_ERR_ALREADY_BOUND;
        if (!is_virtual && interrupt.vector == vector)
            return ZX_ERR_ALREADY_BOUND;
    }

    Interrupt interrupt;
    interrupt.dispatcher = this;
    interrupt.timestamp = 0;
    interrupt.is_maskable = is_maskable;
    interrupt.is_virtual = is_virtual;
    interrupt.vector = vector;
    interrupt.slot = slot;
    fbl::AllocChecker ac;
    interrupts_.push_back(interrupt, &ac);
    if (!ac.check())
        return ZX_ERR_NO_MEMORY;

    if (!is_virtual) {
        zx_status_t status = RegisterInterruptHandler(vector, &interrupts_[index]);
        if (status != ZX_OK) {
            interrupts_.erase(index);
            return status;
        }
    }

    return ZX_OK;
}

zx_status_t InterruptDispatcher::WaitForInterrupt(uint64_t* out_slots) {
    while (true) {
        uint64_t signals = signals_.exchange(0);
        if (signals) {
            if (signals & SIGNAL_MASK(ZX_INTERRUPT_CANCEL)) {
                return ZX_ERR_CANCELED;
            }
            PostWait(signals);
            *out_slots = signals;
            return ZX_OK;
        }

        PreWait();
        zx_status_t status = event_wait_deadline(&event_, ZX_TIME_INFINITE, true);
        if (status != ZX_OK) {
            return status;
        }
    }
}

zx_status_t InterruptDispatcher::GetTimeStamp(uint32_t slot, zx_time_t* out_timestamp) {
    if (slot >= ZX_INTERRUPT_MAX_WAIT_SLOTS)
        return ZX_ERR_INVALID_ARGS;

    fbl::AutoLock lock(&lock_);

    size_t size = interrupts_.size();
    for (size_t i = 0; i < size; i++) {
        Interrupt& interrupt = interrupts_[i];
        if (interrupt.slot == slot) {
            zx_time_t timestamp = interrupt.timestamp;
            if (timestamp) {
                *out_timestamp = timestamp;
                return ZX_OK;
            } else {
                return ZX_ERR_BAD_STATE;
            }
        }
    }

    return ZX_ERR_NOT_FOUND;
}


zx_status_t InterruptDispatcher::UserSignal(uint32_t slot, zx_time_t timestamp) {
    if (slot >= ZX_INTERRUPT_MAX_WAIT_SLOTS)
        return ZX_ERR_INVALID_ARGS;

    size_t size = interrupts_.size();
    for (size_t i = 0; i < size; i++) {
        Interrupt& interrupt = interrupts_[i];
        if (interrupt.slot == slot) {
            if (!interrupt.is_virtual)
                return ZX_ERR_BAD_STATE;
            if (!interrupt.timestamp)
                interrupt.timestamp = timestamp;
            Signal(SIGNAL_MASK(slot), true);
            return ZX_OK;
        }
    }

    return ZX_ERR_NOT_FOUND;
}

int InterruptDispatcher::Signal(uint64_t signals, bool resched) {
    signals_.fetch_or(signals);
    return event_signal_etc(&event_, resched, ZX_OK);
}

int InterruptDispatcher::Cancel() {
    return Signal(SIGNAL_MASK(ZX_INTERRUPT_CANCEL), true);
}
