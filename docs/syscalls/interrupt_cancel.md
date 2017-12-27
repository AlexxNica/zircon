# zx_interrupt_cancel

## NAME

interrupt_cancel - cancels a call to the interupt_wait syscall

## SYNOPSIS

```
#include <zircon/syscalls.h>

zx_status_t zx_interrupt_cancel(zx_handle_t handle);
```

## DESCRIPTION

**interrupt_cancel**() causes any thread blocked in **zx_interrupt_wait**()
for the interrupt handle *handle* to to unblock and return **ZX_ERR_CANCELED**.
**interrupt_cancel**() will also mask all interrupts bound to *handle*.
This can be used to unblock an interrupt thread so it can exit when shutting down a driver.

## RETURN VALUE

**interrupt_cancel**() returns **ZX_OK** on success. In the event
of failure, a negative error value is returned.

## ERRORS

**ZX_ERR_BAD_HANDLE**  *handle* is not a valid handle.

## SEE ALSO

[interrupt_create](interrupt_create.md),
[interrupt_bind](interrupt_bind.md),
[interrupt_unbind](interrupt_unbind.md),
[interrupt_wait](interrupt_wait.md),
[interrupt_get_timestamp](interrupt_get_timestamp.md),
[interrupt_signal](interrupt_signal.md),
[handle_close](handle_close.md).
