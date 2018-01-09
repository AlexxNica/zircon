// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <object/interrupt_event_dispatcher.h>

#include <kernel/auto_lock.h>
#include <dev/interrupt.h>
#include <zircon/rights.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <platform.h>

// static
zx_status_t InterruptEventDispatcher::Create(fbl::RefPtr<Dispatcher>* dispatcher,
                                             zx_rights_t* rights) {
    // Attempt to construct the dispatcher.
    fbl::AllocChecker ac;
    InterruptEventDispatcher* disp = new (&ac) InterruptEventDispatcher();
    if (!ac.check())
        return ZX_ERR_NO_MEMORY;

    // Hold a ref while we check to see if someone else owns this vector or not.
    // If things go wrong, this ref will be released and the IED will get
    // cleaned up automatically.
    auto disp_ref = fbl::AdoptRef<Dispatcher>(disp);

    // Transfer control of the new dispatcher to the creator and we are done.
    *rights     = ZX_DEFAULT_INTERRUPT_RIGHTS;
    *dispatcher = fbl::move(disp_ref);

    return ZX_OK;
}

InterruptEventDispatcher::~InterruptEventDispatcher() {
    for (const auto& interrupt : interrupts_) {
        if (!(interrupt.flags & ZX_INTERRUPT_VIRTUAL)) {
            mask_interrupt(interrupt.vector);
            register_int_handler(interrupt.vector, nullptr, nullptr);
        }
    }
}

zx_status_t InterruptEventDispatcher::Bind(uint32_t slot, uint32_t vector, uint32_t options) {
    canary_.Assert();

    if (slot >= ZX_INTERRUPT_MAX_WAIT_SLOTS)
        return ZX_ERR_INVALID_ARGS;

    fbl::AutoLock lock(&lock_);

    bool is_virtual = !!(options & ZX_INTERRUPT_VIRTUAL);
    if (is_virtual) {
        if (options != ZX_INTERRUPT_VIRTUAL) {
            return ZX_ERR_INVALID_ARGS;
        }
    } else {
        if (options & ~(ZX_INTERRUPT_REMAP_IRQ | ZX_INTERRUPT_MODE_MASK))
            return ZX_ERR_INVALID_ARGS;

        // Remap the vector if we have been asked to do so.
        if (options & ZX_INTERRUPT_REMAP_IRQ)
            vector = remap_interrupt(vector);

        if (!is_valid_interrupt(vector, 0))
            return ZX_ERR_INVALID_ARGS;

        bool default_mode = false;
        enum interrupt_trigger_mode tm = IRQ_TRIGGER_MODE_EDGE;
        enum interrupt_polarity pol = IRQ_POLARITY_ACTIVE_LOW;
        switch (options & ZX_INTERRUPT_MODE_MASK) {
            case ZX_INTERRUPT_MODE_DEFAULT:
                default_mode = true;
                break;
            case ZX_INTERRUPT_MODE_EDGE_LOW:
                tm = IRQ_TRIGGER_MODE_EDGE;
                pol = IRQ_POLARITY_ACTIVE_LOW;
                break;
            case ZX_INTERRUPT_MODE_EDGE_HIGH:
                tm = IRQ_TRIGGER_MODE_EDGE;
                pol = IRQ_POLARITY_ACTIVE_HIGH;
                break;
            case ZX_INTERRUPT_MODE_LEVEL_LOW:
                tm = IRQ_TRIGGER_MODE_LEVEL;
                pol = IRQ_POLARITY_ACTIVE_LOW;
                break;
            case ZX_INTERRUPT_MODE_LEVEL_HIGH:
                tm = IRQ_TRIGGER_MODE_LEVEL;
                pol = IRQ_POLARITY_ACTIVE_HIGH;
                break;
            default:
                return ZX_ERR_INVALID_ARGS;
        }

        if (!default_mode) {
            zx_status_t status = configure_interrupt(vector, tm, pol);
            if (status != ZX_OK)
                return status;
        }
    }

    zx_status_t status = AddSlot(slot, vector, options);
    if (status != ZX_OK)
        return status;

    if (!is_virtual)
        unmask_interrupt(vector);

    return ZX_OK;
}

zx_status_t InterruptEventDispatcher::WaitForInterrupt(uint64_t* out_slots) {
    canary_.Assert();

    return Wait(out_slots);
}

void InterruptEventDispatcher::on_zero_handles() {
    for (const auto& interrupt : interrupts_) {
        if (!(interrupt.flags & ZX_INTERRUPT_VIRTUAL))
            mask_interrupt(interrupt.vector);
    }

    Cancel();
}

enum handler_return InterruptEventDispatcher::IrqHandler(void* ctx) {
    Interrupt* interrupt = reinterpret_cast<Interrupt*>(ctx);
    if (!interrupt->timestamp) {
        // only record timestamp if this is the first IRQ since we started waiting
        interrupt->timestamp = current_time();
    }
    InterruptEventDispatcher* thiz
            = reinterpret_cast<InterruptEventDispatcher *>(interrupt->dispatcher);

    if (interrupt->flags & ZX_INTERRUPT_MODE_LEVEL_MASK)
        mask_interrupt(interrupt->vector);

    if (thiz->Signal(SIGNAL_MASK(interrupt->slot)) > 0) {
        return INT_RESCHEDULE;
    } else {
        return INT_NO_RESCHEDULE;
    }
}

void InterruptEventDispatcher::PreWait() {
    uint64_t signals = current_slots_;

    for (auto& interrupt : interrupts_) {
        if ((interrupt.flags & ZX_INTERRUPT_MODE_LEVEL_MASK) &&
                (signals & (SIGNAL_MASK(interrupt.slot))))
            unmask_interrupt(interrupt.vector);
        // clear timestamp so we can know when first IRQ occurs
        interrupt.timestamp = 0;
    }
}

void InterruptEventDispatcher::PostWait(uint64_t signals) {
    current_slots_ = signals;

    for (const auto& interrupt : interrupts_) {
        if ((interrupt.flags & ZX_INTERRUPT_MODE_LEVEL_MASK) &&
                (signals & (SIGNAL_MASK(interrupt.slot))))
            mask_interrupt(interrupt.vector);
    }
}

void InterruptEventDispatcher::MaskInterrupt(uint32_t vector) {
}

void InterruptEventDispatcher::UnmaskInterrupt(uint32_t vector) {
}

zx_status_t InterruptEventDispatcher::RegisterInterruptHandler(uint32_t vector, void* data) {
    return register_int_handler(vector, IrqHandler, data);
}

void InterruptEventDispatcher::UnregisterInterruptHandler(uint32_t vector) {
    register_int_handler(vector, nullptr, nullptr);
}
