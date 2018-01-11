// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <unistd.h>

#include <stdbool.h>
#include <string.h>

#include <ddk/device.h>
#include <ddk/iotxn.h>
#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <fbl/limits.h>
#include <fbl/ref_ptr.h>
#include <zircon/compiler.h>
#include <zircon/device/block.h>
#include <zircon/syscalls.h>
#include <zx/fifo.h>

#include "server.h"

// This signal is set on the FIFO when the server should be instructed
// to terminate. Note that the block client (other end of the fifo) can
// currently also set this bit as an alternative mechanism to shut down
// the block server.
//
// If additional signals are set on the FIFO, it should be noted that
// block clients will also be able to manipulate them.
constexpr zx_signals_t kSignalFifoTerminate = ZX_USER_SIGNAL_0;

namespace {

void OutOfBandErrorRespond(const zx::fifo& fifo, zx_status_t status, txnid_t txnid) {
    block_fifo_response_t response;
    response.status = status;
    response.txnid = txnid;
    response.count = 0;

    uint32_t actual;
    status = fifo.write(&response, sizeof(block_fifo_response_t), &actual);
    if (status != ZX_OK) {
        fprintf(stderr, "Block Server I/O error: Could not write response\n");
    }
}

void BlockComplete(void* cookie, zx_status_t status) {
    block_msg_t* msg = static_cast<block_msg_t*>(cookie);
    // Since iobuf is a RefPtr, it lives at least as long as the txn,
    // and is not discarded underneath the block device driver.
    ZX_DEBUG_ASSERT(msg->iobuf != nullptr);
    ZX_DEBUG_ASSERT(msg->txn != nullptr);
    // Hold an extra copy of the 'blktxn' refptr; if we don't, and 'msg->txn' is
    // the last copy, then when we nullify 'msg->txn' in Complete we end up
    // trying to unlock a lock in a deleted BlockTxn.
    auto blktxn = msg->txn;
    // Pass msg to complete so 'msg->txn' can be nullified while protected
    // by the BlockTransaction's lock.
    blktxn->Complete(msg, status);
}

void BlockCompleteIotxn(iotxn_t* txn, void* cookie) {
    BlockComplete(cookie, txn->status);
    iotxn_release(txn);
}

void BlockCompleteCb(block_op_t* bop, zx_status_t status) {
    BlockComplete(bop->cookie, status);
    free(bop);
}

}  // namespace

void BlockServer::Queue(uint32_t flags, zx_handle_t vmo, uint64_t length,
                        uint64_t vmo_offset, uint64_t dev_offset, block_msg_t* msg) {
    if (bp_.ops == NULL) {
        iotxn_t* txn;
        zx_status_t status;
        if ((status = iotxn_alloc_vmo(&txn, IOTXN_ALLOC_POOL, vmo,
                                      vmo_offset, length)) != ZX_OK) {
            BlockComplete(msg, status);
            return;
        }
        txn->flags = flags;
        txn->opcode = msg->opcode;
        txn->offset = dev_offset;
        txn->cookie = msg;
        txn->complete_cb = BlockCompleteIotxn;
        iotxn_queue(dev_, txn);
    } else {
        size_t bsz = info_.block_size;
        block_op_t* bop = (block_op_t*) malloc(block_op_size_);
        if (bop == NULL) {
            BlockComplete(msg, ZX_ERR_NO_MEMORY);
            return;
        }
        bop->u.rw.command = (msg->opcode == BLOCKIO_READ) ? BLOCK_OP_READ : BLOCK_OP_WRITE;
        bop->u.rw.length = (uint16_t) ((length / bsz) - 1U);
        bop->u.rw.vmo = vmo;
        bop->u.rw.offset_dev = dev_offset / bsz;
        bop->u.rw.offset_vmo = vmo_offset / bsz;
        bop->u.rw.pages = NULL;
        bop->completion_cb = BlockCompleteCb;
        bop->cookie = msg;
        bp_.ops->queue(bp_.ctx, bop);
    }
}

BlockTransaction::BlockTransaction(zx_handle_t fifo, txnid_t txnid) :
    fifo_(fifo), flags_(0), ctr_(0) {
    memset(&response_, 0, sizeof(response_));
    response_.txnid = txnid;
}

BlockTransaction::~BlockTransaction() {}

zx_status_t BlockTransaction::Enqueue(bool do_respond, block_msg_t** msg_out) {
    fbl::AutoLock lock(&lock_);
    if (flags_ & kTxnFlagRespond) {
        // Can't get more than one response for a txn
        goto fail;
    } else if (ctr_ == MAX_TXN_MESSAGES - 1) {
        // This is the last message! We expect TXN_END, and will append it
        // whether or not it was provided.
        // If it WASN'T provided, then it would not be clear when to
        // clear the current block transaction.
        do_respond = true;
    }
    ZX_DEBUG_ASSERT(ctr_ < MAX_TXN_MESSAGES); // Avoid overflowing msgs
    if (ctr_ == 0) {
        msgs_[ctr_].flags = IOTXN_SYNC_BEFORE;
    } else if (do_respond) {
        msgs_[ctr_].flags = IOTXN_SYNC_AFTER;
    } else {
        msgs_[ctr_].flags = 0;
    }
    msgs_[ctr_].sub_txns = 1;
    *msg_out = &msgs_[ctr_++];
    flags_ |= do_respond ? kTxnFlagRespond : 0;
    return ZX_OK;
fail:
    if (do_respond) {
        OutOfBandErrorRespond(zx::unowned_fifo::wrap(fifo_), ZX_ERR_IO, response_.txnid);
    }
    return ZX_ERR_IO;
}

void BlockTransaction::Complete(block_msg_t* msg, zx_status_t status) {
    fbl::AutoLock lock(&lock_);
    if ((status != ZX_OK) && (response_.status == ZX_OK)) {
        response_.status = status;
    }

    ZX_DEBUG_ASSERT(msg->sub_txns > 0);
    msg->sub_txns--;
    if (msg->sub_txns > 0) {
        // The are more pending sub-txns to complete before we respond.
        // This case will only occur for requests larger than the maximum xfer
        // size.
        return;
    }

    response_.count++;
    ZX_DEBUG_ASSERT(ctr_ != 0);
    ZX_DEBUG_ASSERT(response_.count <= ctr_);

    if ((flags_ & kTxnFlagRespond) && (response_.count == ctr_)) {
        // Don't block the block device. Respond if we can (and in the absence
        // of an I/O error or closed remote, this should just work).
        uint32_t actual;
        zx_status_t status = zx_fifo_write(fifo_, &response_,
                                           sizeof(block_fifo_response_t), &actual);
        if (status != ZX_OK) {
            fprintf(stderr, "Block Server I/O error: Could not write response\n");
        }
        response_.count = 0;
        response_.status = ZX_OK;
        ctr_ = 0;
        flags_ &= ~kTxnFlagRespond;
    }
    msg->txn.reset();
    msg->iobuf.reset();
}

IoBuffer::IoBuffer(zx::vmo vmo, vmoid_t id) : io_vmo_(fbl::move(vmo)), vmoid_(id) {}

IoBuffer::~IoBuffer() {}

zx_status_t IoBuffer::ValidateVmoHack(uint64_t length, uint64_t vmo_offset) {
    uint64_t vmo_size;
    zx_status_t status;
    if ((status = io_vmo_.get_size(&vmo_size)) != ZX_OK) {
        return status;
    } else if (length + vmo_offset > vmo_size) {
        return ZX_ERR_INVALID_ARGS;
    }
    return ZX_OK;
}

zx_status_t BlockServer::Read(block_fifo_request_t* requests, uint32_t* count) {
    // Keep trying to read messages from the fifo until we have a reason to
    // terminate
    while (true) {
        zx_status_t status = fifo_.read(requests, sizeof(block_fifo_request_t), count);
        if (status == ZX_ERR_SHOULD_WAIT) {
            zx_signals_t waitfor = ZX_FIFO_READABLE | ZX_FIFO_PEER_CLOSED | kSignalFifoTerminate;
            zx_signals_t observed;
            if ((status = fifo_.wait_one(waitfor, ZX_TIME_INFINITE, &observed)) != ZX_OK) {
                return status;
            }
            if ((observed & ZX_FIFO_PEER_CLOSED) || (observed & kSignalFifoTerminate)) {
                return ZX_ERR_PEER_CLOSED;
            }
            // Try reading again...
        } else {
            return status;
        }
    }
}

zx_status_t BlockServer::FindVmoIDLocked(vmoid_t* out) {
    for (vmoid_t i = last_id_; i < fbl::numeric_limits<vmoid_t>::max(); i++) {
        if (!tree_.find(i).IsValid()) {
            *out = i;
            last_id_ = static_cast<vmoid_t>(i + 1);
            return ZX_OK;
        }
    }
    for (vmoid_t i = VMOID_INVALID + 1; i < last_id_; i++) {
        if (!tree_.find(i).IsValid()) {
            *out = i;
            last_id_ = static_cast<vmoid_t>(i + 1);
            return ZX_OK;
        }
    }
    return ZX_ERR_NO_RESOURCES;
}

zx_status_t BlockServer::AttachVmo(zx::vmo vmo, vmoid_t* out) {
    zx_status_t status;
    vmoid_t id;
    fbl::AutoLock server_lock(&server_lock_);
    if ((status = FindVmoIDLocked(&id)) != ZX_OK) {
        return status;
    }

    fbl::AllocChecker ac;
    fbl::RefPtr<IoBuffer> ibuf = fbl::AdoptRef(new (&ac) IoBuffer(fbl::move(vmo), id));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    tree_.insert(fbl::move(ibuf));
    *out = id;
    return ZX_OK;
}

zx_status_t BlockServer::AllocateTxn(txnid_t* out) {
    fbl::AutoLock server_lock(&server_lock_);
    for (size_t i = 0; i < fbl::count_of(txns_); i++) {
        if (txns_[i] == nullptr) {
            txnid_t txnid = static_cast<txnid_t>(i);
            fbl::AllocChecker ac;
            txns_[i] = fbl::AdoptRef(new (&ac) BlockTransaction(fifo_.get(), txnid));
            if (!ac.check()) {
                return ZX_ERR_NO_MEMORY;
            }
            *out = txnid;
            return ZX_OK;
        }
    }
    return ZX_ERR_NO_RESOURCES;
}

void BlockServer::FreeTxn(txnid_t txnid) {
    fbl::AutoLock server_lock(&server_lock_);
    if (txnid >= fbl::count_of(txns_)) {
        return;
    }
    ZX_DEBUG_ASSERT(txns_[txnid] != nullptr);
    txns_[txnid] = nullptr;
}

zx_status_t BlockServer::Create(zx_device_t* dev, block_protocol_t* bp,
                                zx::fifo* fifo_out, BlockServer** out) {
    fbl::AllocChecker ac;
    BlockServer* bs = new (&ac) BlockServer(dev, bp);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    zx_status_t status;
    if ((status = zx::fifo::create(BLOCK_FIFO_MAX_DEPTH, BLOCK_FIFO_ESIZE, 0,
                                   fifo_out, &bs->fifo_)) != ZX_OK) {
        delete bs;
        return status;
    }

    if (bp->ops != NULL) {
        bp->ops->query(bp->ctx, &bs->info_, &bs->block_op_size_);
    };

    *out = bs;
    return ZX_OK;
}

zx_status_t BlockServer::Serve() {
    zx_status_t status;
    block_fifo_request_t requests[BLOCK_FIFO_MAX_DEPTH];
    uint32_t count;
    while (true) {
        if ((status = Read(requests, &count) != ZX_OK)) {
            return status;
        }

        for (size_t i = 0; i < count; i++) {
            bool wants_reply = requests[i].opcode & BLOCKIO_TXN_END;
            txnid_t txnid = requests[i].txnid;
            vmoid_t vmoid = requests[i].vmoid;

            fbl::AutoLock server_lock(&server_lock_);
            auto iobuf = tree_.find(vmoid);
            if (!iobuf.IsValid()) {
                // Operation which is not accessing a valid vmo
                if (wants_reply) {
                    OutOfBandErrorRespond(fifo_, ZX_ERR_IO, txnid);
                }
                continue;
            }
            if (txnid >= MAX_TXN_COUNT || txns_[txnid] == nullptr) {
                // Operation which is not accessing a valid txn
                if (wants_reply) {
                    OutOfBandErrorRespond(fifo_, ZX_ERR_IO, txnid);
                }
                continue;
            }

            switch (requests[i].opcode & BLOCKIO_OP_MASK) {
            case BLOCKIO_READ:
            case BLOCKIO_WRITE: {
                if (requests[i].length > fbl::numeric_limits<uint32_t>::max()) {
                    // Operation which is too large
                    if (wants_reply) {
                        OutOfBandErrorRespond(fifo_, ZX_ERR_INVALID_ARGS, txnid);
                    }
                    continue;
                }

                // Transaction bytes values must be in multiples of block size
                // Transaction transfer length must fit in a uint16 n+1 field
                size_t bsmask = info_.block_size - 1;
                if ((requests[i].length & bsmask) ||
                    (requests[i].dev_offset & bsmask) ||
                    (requests[i].vmo_offset & bsmask) ||
                    ((requests[i].length / info_.block_size) < 1) ||
                    ((requests[i].length / info_.block_size) > 65536)) {
                    if (wants_reply) {
                        OutOfBandErrorRespond(fifo_, ZX_ERR_INVALID_ARGS, txnid);
                    }
                    continue;
                }

                block_msg_t* msg;
                status = txns_[txnid]->Enqueue(wants_reply, &msg);
                if (status != ZX_OK) {
                    break;
                }
                ZX_DEBUG_ASSERT(msg->txn == nullptr);
                msg->txn = txns_[txnid];
                ZX_DEBUG_ASSERT(msg->iobuf == nullptr);
                msg->iobuf = iobuf.CopyPointer();

                // Hack to ensure that the vmo is valid.
                // In the future, this code will be responsible for pinning VMO pages,
                // and the completion will be responsible for un-pinning those same pages.
                status = iobuf->ValidateVmoHack(requests[i].length, requests[i].vmo_offset);
                if (status != ZX_OK) {
                    BlockComplete(msg, status);
                    break;
                }

                msg->opcode = requests[i].opcode & BLOCKIO_OP_MASK;

                const uint64_t max_xfer = info_.max_transfer_size;
                if (max_xfer != 0 && max_xfer < requests[i].length) {
                    uint64_t len_remaining = requests[i].length;
                    uint64_t vmo_offset = requests[i].vmo_offset;
                    uint64_t dev_offset = requests[i].dev_offset;

                    size_t sub_txns = fbl::round_up(len_remaining, max_xfer) / max_xfer;
                    msg->sub_txns = static_cast<uint32_t>(sub_txns);
                    for (size_t i = 0; i < sub_txns; i++) {
                        uint64_t length = fbl::min(len_remaining, max_xfer);
                        len_remaining -= length;

                        uint32_t flags = msg->flags;
                        // Only allow IOTXN_SYNC_AFTER to be set on the last sub-txn.
                        flags &= ~(i == sub_txns - 1 ? 0 : IOTXN_SYNC_AFTER);
                        // Only allow IOTXN_SYNC_BEFORE to be set on the first sub-txn.
                        flags &= ~(i == 0 ? 0 : IOTXN_SYNC_BEFORE);
                        Queue(flags, iobuf->vmo(), length,
                              vmo_offset, dev_offset, msg);
                        vmo_offset += length;
                        dev_offset += length;
                    }
                    ZX_DEBUG_ASSERT(len_remaining == 0);
                } else {
                    Queue(msg->flags, iobuf->vmo(), requests[i].length,
                          requests[i].vmo_offset, requests[i].dev_offset, msg);
                }

                break;
            }
            case BLOCKIO_SYNC: {
                // TODO(smklein): It might be more useful to have this on a per-vmo basis
                fprintf(stderr, "Warning: BLOCKIO_SYNC is currently unimplemented\n");
                break;
            }
            case BLOCKIO_CLOSE_VMO: {
                tree_.erase(*iobuf);
                if (wants_reply) {
                    OutOfBandErrorRespond(fifo_, ZX_OK, txnid);
                }
                break;
            }
            default: {
                fprintf(stderr, "Unrecognized Block Server operation: %x\n",
                        requests[i].opcode);
            }
            }
        }
    }
}

BlockServer::BlockServer(zx_device_t* dev, block_protocol_t* bp) :
    dev_(dev), bp_(*bp), block_op_size_(sizeof(block_op_t)), last_id_(VMOID_INVALID + 1) {
    size_t actual;
    device_ioctl(dev_, IOCTL_BLOCK_GET_INFO, nullptr, 0, &info_, sizeof(info_), &actual);
}

BlockServer::~BlockServer() {
    ShutDown();
}

void BlockServer::ShutDown() {
    // Identify that the server should stop reading and return,
    // implicitly closing the fifo.
    fifo_.signal(0, kSignalFifoTerminate);
}

// C declarations
zx_status_t blockserver_create(zx_device_t* dev, block_protocol_t* bp,
                               zx_handle_t* fifo_out, BlockServer** out) {
    zx::fifo fifo;
    zx_status_t status = BlockServer::Create(dev, bp, &fifo, out);
    *fifo_out = fifo.release();
    return status;
}
void blockserver_shutdown(BlockServer* bs) {
    bs->ShutDown();
}
void blockserver_free(BlockServer* bs) {
    delete bs;
}
zx_status_t blockserver_serve(BlockServer* bs) {
    return bs->Serve();
}
zx_status_t blockserver_attach_vmo(BlockServer* bs, zx_handle_t raw_vmo, vmoid_t* out) {
    zx::vmo vmo(raw_vmo);
    return bs->AttachVmo(fbl::move(vmo), out);
}
zx_status_t blockserver_allocate_txn(BlockServer* bs, txnid_t* out) {
    return bs->AllocateTxn(out);
}
void blockserver_free_txn(BlockServer* bs, txnid_t txnid) {
    return bs->FreeTxn(txnid);
}
