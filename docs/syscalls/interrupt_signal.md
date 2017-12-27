# zx_interrupt_signal

## NAME

interrupt_signal - signals a virtual interrupt on an interrupt handle

## SYNOPSIS

```
#include <zircon/syscalls.h>

zx_status_t zx_interrupt_signal(zx_handle_t handle, uint32_t slot, zx_time_t timestamp);
```

## DESCRIPTION

**interrupt_signal**() is used to signal a virtual interrupt on an interrupt handle.
The *slot* parameter must have been bound to the interrupt handle using **interrupt_bind**()
with the **ZX_INTERRUPT_VIRTUAL** flag set. This will unblock a call to **interrupt_wait**()
on the handle with the *slot* bit set in the **interrupt_wait**() *out_slots* parameter.
After **interrupt_wait**() returns, the timestamp for the interrupt can retrieved with
**interrupt_get_timestamp**().

## RETURN VALUE

**interrupt_signal**() returns **ZX_OK** on success. In the event
of failure, a negative error value is returned.

## ERRORS

**ZX_ERR_BAD_HANDLE** *handle* is not a valid handle.

**ZX_ERR_NOT_SUPPORTED** the interrupt handle does not support this operation.
In particular, **interrupt_signal**() is not supported for PCI interrupt handles.

**ZX_ERR_BAD_STATE** *slot* was not bound with the **ZX_INTERRUPT_VIRTUAL** flag set

**ZX_ERR_INVALID_ARGS** the *slot* parameter is out of range.

**ZX_ERR_NOT_FOUND** if *slot* was not bound with **interrupt_bind**()

## SEE ALSO

[interrupt_create](interrupt_create.md),
[interrupt_bind](interrupt_bind.md),
[interrupt_unbind](interrupt_unbind.md),
[interrupt_wait](interrupt_wait.md),
[interrupt_get_timestamp](interrupt_get_timestamp.md),
[interrupt_cancel](interrupt_cancel.md).
[handle_close](handle_close.md).
