# interrupt_unbind

## NAME

interrupt_unbind - Unind an interrupt vector from interrupt handle

## SYNOPSIS

```
#include <zircon/syscalls.h>

zx_status_t zx_interrupt_unbind(zx_handle_t handle, uint32_t slot);

```

## DESCRIPTION

**interrupt_unbind**() unbinds an interrupt vector or virtual interrupt slot
from an interrupt handle.
In other words, it undoes the binding performed by **interrupt_bind**().
It is not necessary to call **interrupt_unbind**() before closing the interrupt handle.

If another thread is blocked in **interrupt_wait**() when **interrupt_unbind**() is called,
the thread will continue waiting until an interrupt occurs on another interrupt slot
bound to the handle or until **interrupt_cancel**() is called on the handle.

## RETURN VALUE

**interrupt_unbind**() returns **ZX_OK** on success. In the event
of failure, a negative error value is returned.

## ERRORS

**ZX_ERR_BAD_HANDLE** *handle* is not a valid handle.

**ZX_ERR_NOT_FOUND** the *slot* parameter is not bound to an interrupt vector or virtual interrupt.

**ZX_ERR_INVALID_ARGS** the *slot* parameter is out of range.

**ZX_ERR_NOT_SUPPORTED** the interrupt handle does not support this operation.
In particular, **interrupt_bind**() is not supported for PCI interrupt handles.

## SEE ALSO

[interrupt_create](interrupt_create.md),
[interrupt_bind](interrupt_bind.md),
[interrupt_wait](interrupt_wait.md),
[interrupt_get_timestamp](interrupt_get_timestamp.md),
[interrupt_signal](interrupt_signal.md),
[interrupt_cancel](interrupt_cancel.md).
[handle_close](handle_close.md).
