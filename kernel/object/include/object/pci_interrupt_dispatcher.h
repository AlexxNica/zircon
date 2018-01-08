// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once
#if WITH_DEV_PCIE

#include <fbl/canary.h>
#include <object/interrupt_dispatcher.h>
#include <object/pci_device_dispatcher.h>
#include <sys/types.h>

class PciDeviceDispatcher;

class PciInterruptDispatcher final : public InterruptDispatcher {
public:
    static zx_status_t Create(const fbl::RefPtr<PcieDevice>& device,
                              uint32_t irq_id,
                              bool maskable,
                              zx_rights_t* out_rights,
                              fbl::RefPtr<Dispatcher>* out_interrupt);

    ~PciInterruptDispatcher() final;

    zx_status_t Bind(uint32_t slot, uint32_t vector, uint32_t options) final;
    zx_status_t WaitForInterrupt(uint64_t* out_slots) final;
    zx_status_t GetTimeStamp(uint32_t slot, zx_time_t* out_timestamp) final;
    zx_status_t UserSignal(uint32_t slot, zx_time_t timestamp) final;
    zx_status_t UserCancel() final;
    void PreWait() final;
    void PostWait(uint64_t signals) final;

private:
    static constexpr uint32_t IRQ_SLOT = 0;

    static pcie_irq_handler_retval_t IrqThunk(const PcieDevice& dev,
                                              uint irq_id,
                                              void* ctx);
    PciInterruptDispatcher(uint32_t irq_id, bool maskable)
        : irq_id_(irq_id),
          maskable_(maskable) { }

    fbl::Canary<fbl::magic("INPD")> canary_;

    const uint32_t irq_id_;
    const bool     maskable_;
    zx_time_t      timestamp_ = 0;
    fbl::RefPtr<PcieDevice> device_;
};

#endif  // if WITH_DEV_PCIE
