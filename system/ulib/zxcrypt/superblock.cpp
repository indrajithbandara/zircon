// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <crypto/bytes.h>
#include <crypto/cipher.h>
#include <crypto/hkdf.h>
#include <ddk/device.h>
#include <ddk/iotxn.h>
#include <fbl/auto_call.h>
#include <fbl/macros.h>
#include <fbl/unique_fd.h>
#include <fbl/unique_ptr.h>
#include <fdio/debug.h>
#include <sync/completion.h>
#include <zircon/compiler.h>
#include <zircon/device/block.h>
#include <zircon/errors.h>
#include <zircon/status.h>
#include <zircon/types.h>
#include <zxcrypt/superblock.h>
#include <safeint/safe_math.h>

#define MXDEBUG 0

namespace zxcrypt {

// Several copies of the metadata for a zxcrypt volume is saved at the beginning and end of the
// devices.  The number of copies is given by |kReservedPairs|, and the locations of each block can
// be iterated through using |Begin| and |Next|.  The metadata block, or superblock, consists of a
// fixed type GUID, an instance GUID, a 32-bit version, a set of "key slots"  The key slots are data
// cipher key material encrypted with a wrapping crypto::AEAD key derived from the caller-provided
// root key and specific slot.

// Determines what algorithms are in use when creating new zxcrypt devices.
const Superblock::Version Superblock::kDefaultVersion = Superblock::kAES256_XTS_SHA256;

// Maximum number of key slots.  If a device's block size can not hold |kNumSlots| for a particular
// version, then attempting to |Create| or |Open| a zxcrypt volume will fail with
// |ZX_ERR_NOT_SUPPORTED|.
const slot_num_t Superblock::kNumSlots = 16;

// The number of metadata blocks at each end of the device.  That is, there are |kReservedPairs|
// blocks reserved at the start of the device, and another |kReservedPairs| blocks reserved at the
// end of the device.
const size_t Superblock::kReservedPairs = 2;

namespace {

// HKDF labels
const size_t kMaxLabelLen = 16;
const char* kWrapKeyLabel = "wrap key %" PRIu64;
const char* kWrapIvLabel = "wrap iv %" PRIu64;

// Header is type GUID | instance GUID | version.
const size_t kHeaderLen = GUID_LEN + GUID_LEN + sizeof(uint32_t);

// Completes synchronous iotxns queued using |SyncIO||.
void SyncComplete(iotxn_t* txn, void* cookie) {
    completion_signal((completion_t*)cookie);
}

// Performs synchronous I/O
zx_status_t SyncIO(zx_device_t* dev, uint32_t op, void* buf, size_t off, size_t len) {
    zx_status_t rc;
    ssize_t res;

    if (!dev || !buf || len == 0) {
        xprintf("%s: bad parameter(s): dev=%p, buf=%p, len=%zu\n", __PRETTY_FUNCTION__, dev, buf,
                len);
        return ZX_ERR_INVALID_ARGS;
    }

    iotxn_t* txn;
    if ((rc = iotxn_alloc(&txn, 0, len)) != ZX_OK) {
        xprintf("%s: iotxn_alloc(%p, 0, %zu) failed: %s\n", __PRETTY_FUNCTION__, &txn, len,
                zx_status_get_string(rc));
        return rc;
    }

    txn->opcode = op;
    txn->offset = off;
    txn->length = len;
    txn->complete_cb = SyncComplete;

    if (op == IOTXN_OP_WRITE && (res = iotxn_copyto(txn, buf, len, 0)) < 0) {
        rc = static_cast<zx_status_t>(res);
        xprintf("%s: iotxn_copyto(%p, %p, 0, %zu) failed: %s\n", __PRETTY_FUNCTION__, txn, buf, len,
                zx_status_get_string(rc));
        iotxn_release(txn);
        return rc;
    }

    completion_t completion;
    txn->cookie = &completion;
    iotxn_queue(dev, txn);
    completion_wait(&completion, ZX_TIME_INFINITE);

    if (txn->status != ZX_OK) {
        xprintf("%s: iotxn_queue(%p, %p) failed: %s\n", __PRETTY_FUNCTION__, dev, txn,
                zx_status_get_string(txn->status));
        rc = txn->status;
    } else if (txn->actual < txn->length) {
        xprintf("%s: incomplete I/O: have %zu, need %zu\n", __PRETTY_FUNCTION__, txn->actual,
                txn->length);
        rc = ZX_ERR_IO;
    } else if (op == IOTXN_OP_READ && (res = iotxn_copyfrom(txn, buf, len, 0)) < 0) {
        rc = static_cast<zx_status_t>(res);
        xprintf("%s: iotxn_copyfrom(%p, %p, 0, %zu) failed: %s\n", __PRETTY_FUNCTION__, txn, buf,
                len, zx_status_get_string(rc));
    } else {
        rc = ZX_OK;
    }
    iotxn_release(txn);
    return rc;
}

} // namespace

Superblock::~Superblock() {}

// Library methods

zx_status_t Superblock::Create(fbl::unique_fd fd, const crypto::Bytes& key) {
    zx_status_t rc;

    if (!fd) {
        xprintf("%s: bad parameter(s): fd=%d\n", __PRETTY_FUNCTION__, fd.get());
        return ZX_ERR_INVALID_ARGS;
    }

    Superblock superblock(fbl::move(fd));
    if ((rc = superblock.Init()) != ZX_OK || (rc = superblock.CreateBlock()) != ZX_OK ||
        (rc = superblock.SealBlock(key, 0)) != ZX_OK || (rc = superblock.CommitBlock()) != ZX_OK) {
        return rc;
    }

    return ZX_OK;
}

zx_status_t Superblock::Open(fbl::unique_fd fd, const crypto::Bytes& key, slot_num_t slot,
                             fbl::unique_ptr<Superblock>* out) {
    zx_status_t rc;

    if (!fd || slot >= kNumSlots || !out) {
        xprintf("%s: bad parameter(s): fd=%d, slot=%" PRIu64 ", out=%p\n", __PRETTY_FUNCTION__,
                fd.get(), slot, out);
        return ZX_ERR_INVALID_ARGS;
    }

    fbl::AllocChecker ac;
    fbl::unique_ptr<Superblock> superblock(new (&ac) Superblock(fbl::move(fd)));
    if (!ac.check()) {
        xprintf("%s: allocation failed: %zu bytes\n", __PRETTY_FUNCTION__, sizeof(Superblock));
        return ZX_ERR_NO_MEMORY;
    }
    if ((rc = superblock->Init()) != ZX_OK || (rc = superblock->Open(key, slot)) != ZX_OK) {
        return rc;
    }

    *out = fbl::move(superblock);
    return ZX_OK;
}

zx_status_t Superblock::Enroll(const crypto::Bytes& key, slot_num_t slot) {
    zx_status_t rc;
    ZX_DEBUG_ASSERT(!dev_); // Cannot enroll from driver

    if (slot >= kNumSlots) {
        xprintf("%s: bad parameter(s): slot=%" PRIu64 "\n", __PRETTY_FUNCTION__, slot);
        return ZX_ERR_INVALID_ARGS;
    }
    if (!block_.get()) {
        xprintf("%s: not initialized\n", __PRETTY_FUNCTION__);
        return ZX_ERR_BAD_STATE;
    }
    if ((rc = SealBlock(key, slot)) != ZX_OK || (rc = CommitBlock()) != ZX_OK) {
        return rc;
    }

    return ZX_OK;
}

zx_status_t Superblock::Revoke(slot_num_t slot) {
    zx_status_t rc;
    ZX_DEBUG_ASSERT(!dev_); // Cannot revoke from driver

    if (slot >= kNumSlots) {
        xprintf("%s: bad parameter(s): slot=%" PRIu64 "\n", __PRETTY_FUNCTION__, slot);
        return ZX_ERR_INVALID_ARGS;
    }
    if (!block_.get()) {
        xprintf("%s: not initialized\n", __PRETTY_FUNCTION__);
        return ZX_ERR_BAD_STATE;
    }
    zx_off_t off = kHeaderLen + (slot_len_ * slot);
    crypto::Bytes invalid;
    if ((rc = invalid.InitRandom(slot_len_)) != ZX_OK || (rc = block_.Copy(invalid, off)) != ZX_OK ||
        (rc = CommitBlock()) != ZX_OK) {
        return rc;
    }

    return ZX_OK;
}

zx_status_t Superblock::Shred() {
    zx_status_t rc;
    ZX_DEBUG_ASSERT(!dev_); // Cannot shred from driver

    if (!block_.get()) {
        xprintf("%s: not initialized\n", __PRETTY_FUNCTION__);
        return ZX_ERR_BAD_STATE;
    }
    if ((rc = block_.Randomize()) != ZX_OK) {
        return rc;
    }
    for (rc = Begin(); rc == ZX_ERR_NEXT; rc = Next()) {
        if ((rc = Write()) != ZX_OK) {
            return rc;
        }
    }
    Reset();

    return ZX_OK;
}

// Driver methods

zx_status_t Superblock::Open(zx_device_t* dev, const crypto::Bytes& key, slot_num_t slot,
                             fbl::unique_ptr<Superblock>* out) {
    zx_status_t rc;

    if (!dev || slot >= kNumSlots || !out) {
        xprintf("%s: bad parameter(s): dev=%p, slot=%" PRIu64 ", out=%p\n", __PRETTY_FUNCTION__,
                dev, slot, out);
        return ZX_ERR_INVALID_ARGS;
    }
    fbl::AllocChecker ac;
    fbl::unique_ptr<Superblock> superblock(new (&ac) Superblock(dev));
    if (!ac.check()) {
        xprintf("%s: allocation failed: %zu bytes\n", __PRETTY_FUNCTION__, sizeof(Superblock));
        return ZX_ERR_NO_MEMORY;
    }
    if ((rc = superblock->Init()) != ZX_OK || (rc = superblock->Open(key, slot)) != ZX_OK) {
        return rc;
    }

    *out = fbl::move(superblock);
    return ZX_OK;
}

zx_status_t Superblock::GetInfo(block_info_t* out_blk, fvm_info_t* out_fvm) {
    if (!block_.get()) {
        xprintf("%s: not initialized\n", __PRETTY_FUNCTION__);
        return ZX_ERR_BAD_STATE;
    }
    if (out_blk) {
        memcpy(out_blk, &blk_, sizeof(blk_));
    }
    if (out_fvm) {
        memcpy(out_fvm, &fvm_, sizeof(fvm_));
    }

    return ZX_OK;
}

zx_status_t Superblock::BindCiphers(crypto::Cipher* out_encrypt, crypto::Cipher* out_decrypt) {
    zx_status_t rc;
    ZX_DEBUG_ASSERT(dev_); // Cannot bind from library

    if (!out_encrypt || !out_decrypt) {
        xprintf("%s: bad parameter(s): out_encrypt=%p, out_decrypt=%p\n", __PRETTY_FUNCTION__,
                out_encrypt, out_decrypt);
        return ZX_ERR_INVALID_ARGS;
    }
    if (!block_.get()) {
        xprintf("%s: not initialized\n", __PRETTY_FUNCTION__);
        return ZX_ERR_BAD_STATE;
    }
    uint64_t tweakable = UINT64_MAX / blk_.block_size;
    if ((rc = out_encrypt->InitEncrypt(cipher_, data_key_, data_iv_, tweakable)) != ZX_OK ||
        (rc = out_decrypt->InitDecrypt(cipher_, data_key_, data_iv_, tweakable)) != ZX_OK) {
        return rc;
    }

    return ZX_OK;
}

// Private methods

Superblock::Superblock(fbl::unique_fd&& fd) : dev_(nullptr), fd_(fbl::move(fd)) {
    Reset();
}

Superblock::Superblock(zx_device_t* dev) : dev_(dev), fd_() {
    Reset();
}

// Configuration methods

zx_status_t Superblock::Init() {
    zx_status_t rc;

    Reset();
    auto cleanup = fbl::MakeAutoCall([&] { Reset(); });

    // Get block info; align our blocks to pages
    if ((rc = Ioctl(IOCTL_BLOCK_GET_INFO, nullptr, 0, &blk_, sizeof(blk_))) < 0) {
        xprintf("%s: failed to get block info: %s\n", __PRETTY_FUNCTION__,
                zx_status_get_string(rc));
        return rc;
    }
    // Adjust block size and count to be page-aligned
    if (blk_.block_size < PAGE_SIZE) {
        if (PAGE_SIZE % blk_.block_size != 0) {
            xprintf("%s: unsupported block size: %u\n", __PRETTY_FUNCTION__, blk_.block_size);
            return ZX_ERR_NOT_SUPPORTED;
        }
        blk_.block_count /= (PAGE_SIZE / blk_.block_size);
        blk_.block_size = PAGE_SIZE;
    } else {
        if (blk_.block_size % PAGE_SIZE != 0) {
            xprintf("%s: unsupported block size: %u\n", __PRETTY_FUNCTION__, blk_.block_size);
            return ZX_ERR_NOT_SUPPORTED;
        }
    }
    // Allocate block buffer
    if ((rc = block_.Resize(blk_.block_size)) != ZX_OK) {
        return rc;
    }

    safeint::CheckedNumeric<size_t> reserved_size = blk_.block_size;
    reserved_size *= kReservedPairs;

    // Get FVM info
    switch ((rc = Ioctl(IOCTL_BLOCK_FVM_QUERY, nullptr, 0, &fvm_, sizeof(fvm_)))) {
    case ZX_OK: {
        // This *IS* an FVM partition.
        if (fvm_.slice_size < reserved_size.ValueOrDie() || fvm_.vslice_count < 2) {
            xprintf("%s: bad device: slice_size=%zu, vslice_count=%zu\n", __PRETTY_FUNCTION__,
                    fvm_.slice_size, fvm_.vslice_count);
            return ZX_ERR_NO_SPACE;
        }

        // Check if last slice is allocated
        query_request_t request;
        request.count = 1;
        request.vslice_start[0] = fvm_.vslice_count - 1;
        query_response_t response;
        if ((rc = Ioctl(IOCTL_BLOCK_FVM_VSLICE_QUERY, &request, sizeof(request), &response,
                        sizeof(response))) < 0) {
            xprintf("%s: failed to query FVM vslice: %s\n", __PRETTY_FUNCTION__,
                    zx_status_get_string(rc));
            return rc;
        }
        if (response.count == 0 || response.vslice_range[0].count == 0) {
            xprintf("%s: invalid response\n", __PRETTY_FUNCTION__);
            return ZX_ERR_INTERNAL;
        }

        // Allocate last slice if needed
        extend_request_t extend;
        extend.offset = fvm_.vslice_count - 1;
        extend.length = 1;
        if (!response.vslice_range[0].allocated &&
            (rc = Ioctl(IOCTL_BLOCK_FVM_EXTEND, &extend, sizeof(extend), nullptr, 0)) < 0) {
            xprintf("%s: failed to extend FVM partition: %s\n", __PRETTY_FUNCTION__,
                    zx_status_get_string(rc));
            return rc;
        }

        has_fvm_ = true;
        break;
    }

    case ZX_ERR_NOT_SUPPORTED:
        // This is *NOT* an FVM partition.
        if ((blk_.block_count / 2) < kReservedPairs) {
            xprintf("%s: bad device: block_size=%u, block_count=%" PRIu64 "\n", __PRETTY_FUNCTION__,
                    blk_.block_size, blk_.block_count);
            return ZX_ERR_NO_SPACE;
        }

        // Set "slice" parameters to allow us to pretend it is FVM and use one set of logic.
        fvm_.vslice_count = blk_.block_count / kReservedPairs;
        fvm_.slice_size = reserved_size.ValueOrDie();
        has_fvm_ = false;
        break;

    default:
        // An error occurred
        return rc;
    }

    // Adjust counts to reflect the two reserved slices
    fvm_.vslice_count -= 2;
    blk_.block_count -= (fvm_.slice_size / blk_.block_size) * 2;
    cleanup.cancel();
    return ZX_OK;
}

zx_status_t Superblock::Configure(Superblock::Version version) {
    zx_status_t rc;

    switch (version) {
    case Superblock::kAES256_XTS_SHA256:
        aead_ = crypto::AEAD::kAES128_GCM_SIV;
        cipher_ = crypto::Cipher::kAES256_XTS;
        digest_ = crypto::digest::kSHA256;
        break;

    default:
        xprintf("%s: unknown version: %u\n", __PRETTY_FUNCTION__, version);
        return ZX_ERR_NOT_SUPPORTED;
    }

    size_t wrap_key_len, wrap_iv_len, data_key_len, data_iv_len, tag_len;
    if ((rc = crypto::AEAD::GetKeyLen(aead_, &wrap_key_len)) != ZX_OK ||
        (rc = crypto::AEAD::GetIVLen(aead_, &wrap_iv_len)) != ZX_OK ||
        (rc = crypto::AEAD::GetTagLen(aead_, &tag_len)) != ZX_OK ||
        (rc = crypto::Cipher::GetKeyLen(cipher_, &data_key_len)) != ZX_OK ||
        (rc = crypto::Cipher::GetIVLen(cipher_, &data_iv_len)) != ZX_OK ||
        (rc = crypto::digest::GetDigestLen(digest_, &digest_len_)) != ZX_OK ||
        (rc = wrap_key_.Resize(wrap_key_len)) != ZX_OK ||
        (rc = wrap_iv_.Resize(wrap_iv_len)) != ZX_OK ||
        (rc = data_key_.Resize(data_key_len)) != ZX_OK ||
        (rc = data_iv_.Resize(data_iv_len)) != ZX_OK) {
        return rc;
    }
    slot_len_ = data_key_len + data_iv_len + tag_len;

    safeint::CheckedNumeric<size_t> total = slot_len_;
    total *= kNumSlots;
    total += kHeaderLen;
    if (blk_.block_size < total.ValueOrDie()) {
        xprintf("%s: block size is too small; have %u, need %zu\n", __PRETTY_FUNCTION__,
                blk_.block_size, total.ValueOrDie());
        return ZX_ERR_NOT_SUPPORTED;
    }

    return ZX_OK;
}

zx_status_t Superblock::DeriveSlotKeys(const crypto::Bytes& key, slot_num_t slot) {
    zx_status_t rc;

    crypto::HKDF hkdf;
    char label[kMaxLabelLen];
    if ((rc = hkdf.Init(digest_, key, guid_)) != ZX_OK) {
        return rc;
    }
    snprintf(label, kMaxLabelLen, kWrapKeyLabel, slot);
    if ((rc = hkdf.Derive(label, &wrap_key_)) != ZX_OK) {
        return rc;
    }
    snprintf(label, kMaxLabelLen, kWrapIvLabel, slot);
    if ((rc = hkdf.Derive(label, &wrap_iv_)) != ZX_OK) {
        return rc;
    }

    return ZX_OK;
}

void Superblock::Reset() {
    memset(&blk_, 0, sizeof(blk_));
    memset(&fvm_, 0, sizeof(fvm_));
    has_fvm_ = false;
    block_.Reset();
    offset_ = UINT64_MAX;
    guid_.Reset();
    aead_ = crypto::AEAD::kUninitialized;
    wrap_key_.Reset();
    wrap_iv_.Reset();
    cipher_ = crypto::Cipher::kUninitialized;
    data_key_.Reset();
    data_iv_.Reset();
    slot_len_ = 0;
    digest_ = crypto::digest::kUninitialized;
}

// Block methods

zx_status_t Superblock::Begin() {
    if (fvm_.slice_size == 0) {
        xprintf("%s: not initialized\n", __PRETTY_FUNCTION__);
        return ZX_ERR_STOP;
    }
    offset_ = 0;
    return ZX_ERR_NEXT;
}

zx_status_t Superblock::Next() {
    offset_ += block_.len();
    size_t slice_offset = offset_ % fvm_.slice_size;
    // If slice isn't complete, move to next block in slice
    if (slice_offset != 0 && slice_offset < fvm_.slice_size) {
        return ZX_ERR_NEXT;
    }
    // If finished with the first slice, move to the last slice.
    if (offset_ <= fvm_.slice_size) {
        offset_ = (fvm_.vslice_count + 1) * fvm_.slice_size;
        return ZX_ERR_NEXT;
    }
    // Finished last slice; no more offsets
    return ZX_ERR_STOP;
}

zx_status_t Superblock::CreateBlock() {
    zx_status_t rc;

    // Create a "backdrop" of random data
    if ((rc = block_.Randomize()) != ZX_OK) {
        return rc;
    }

    // Write the variant 1/version 1 type GUID according to RFC 4122.
    uint8_t* out = block_.get();
    memcpy(out, kTypeGuid, GUID_LEN);
    out += GUID_LEN;

    // Create a variant 1/version 4 instance GUID according to RFC 4122.
    if ((rc = guid_.InitRandom(GUID_LEN)) != ZX_OK) {
        return rc;
    }
    guid_[6] = (guid_[6] & 0x0F) | 0x40;
    guid_[8] = (guid_[8] & 0x3F) | 0x80;
    memcpy(out, guid_.get(), GUID_LEN);
    out += GUID_LEN;

    // Write the 32-bit version.
    if ((rc = Configure(kDefaultVersion)) != ZX_OK) {
        return rc;
    }
    uint32_t version = htonl(kDefaultVersion);
    memcpy(out, &version, sizeof(version));

    // Generate the data key and IV, and save the AAD.
    if ((rc = data_key_.Randomize()) != ZX_OK || (rc = data_iv_.Randomize()) != ZX_OK ||
        (rc = header_.Copy(block_.get(), kHeaderLen)) != ZX_OK) {
        return rc;
    }

    return ZX_OK;
}

zx_status_t Superblock::CommitBlock() {
    zx_status_t rc;

    // Make a copy to compare the read result to; this reduces the number of writes we must do.
    crypto::Bytes block;
    if ((rc = block.Copy(block_)) != ZX_OK) {
        return rc;
    }
    for (rc = Begin(); rc == ZX_ERR_NEXT; rc = Next()) {
        // Only write back blocks that don't match
        if (Read() == ZX_OK && block_ == block) {
            continue;
        }
        if ((rc = block_.Copy(block)) != ZX_OK || (rc = Write()) != ZX_OK) {
            xprintf("%s: write failed for offset %" PRIu64 ": %s\n", __PRETTY_FUNCTION__, offset_,
                    zx_status_get_string(rc));
        }
    }
    return ZX_OK;
}

zx_status_t Superblock::SealBlock(const crypto::Bytes& key, slot_num_t slot) {
    zx_status_t rc;

    // Encrypt the data key
    crypto::AEAD aead;
    crypto::Bytes ptext, ctext;
    zx_off_t off = kHeaderLen + (slot_len_ * slot);
    if ((rc = ptext.Append(data_key_)) != ZX_OK || (rc = ptext.Append(data_iv_)) != ZX_OK ||
        (rc = DeriveSlotKeys(key, slot)) != ZX_OK ||
        (rc = aead.InitSeal(aead_, wrap_key_, wrap_iv_)) != ZX_OK ||
        (rc = aead.SetAD(header_)) != ZX_OK ||
        (rc = aead.Seal(ptext, &wrap_iv_, &ctext)) != ZX_OK) {
        return rc;
    }
    memcpy(block_.get() + off, ctext.get(), ctext.len());

    return ZX_OK;
}

zx_status_t Superblock::Open(const crypto::Bytes& key, slot_num_t slot) {
    zx_status_t rc;

    for (rc = Begin(); rc == ZX_ERR_NEXT; rc = Next()) {
        if ((rc = Read()) != ZX_OK) {
            xprintf("%s: failed to read block at %" PRIu64 ": %s\n", __PRETTY_FUNCTION__, offset_,
                    zx_status_get_string(rc));
        } else if ((rc = OpenBlock(key, slot)) != ZX_OK) {
            xprintf("%s: failed to open block at %" PRIu64 ": %s\n", __PRETTY_FUNCTION__, offset_,
                    zx_status_get_string(rc));
        } else {
            return CommitBlock();
        }
    }

    return ZX_ERR_ACCESS_DENIED;
}

zx_status_t Superblock::OpenBlock(const crypto::Bytes& key, slot_num_t slot) {
    zx_status_t rc;

    // Check the type GUID matches |kTypeGuid|.
    uint8_t* in = block_.get();
    if (memcmp(in, kTypeGuid, GUID_LEN) != 0) {
        xprintf("%s: not a zxcrypt device\n", __PRETTY_FUNCTION__);
        return ZX_ERR_NOT_SUPPORTED;
    }
    in += GUID_LEN;

    // Save the instance GUID
    if ((rc = guid_.Copy(in, GUID_LEN)) != ZX_OK) {
        return rc;
    }
    in += GUID_LEN;

    // Read the version
    uint32_t version;
    memcpy(&version, in, sizeof(version));
    in += sizeof(version);
    if ((rc != Configure(Version(ntohl(version)))) != ZX_OK ||
        (rc != DeriveSlotKeys(key, slot)) != ZX_OK) {
        return rc;
    }

    // Read in the data
    crypto::AEAD aead;
    crypto::Bytes ptext, ctext;
    zx_off_t off = kHeaderLen + (slot_len_ * slot);
    if ((rc = ctext.Copy(block_.get() + off, slot_len_)) != ZX_OK ||
        (rc = aead.InitOpen(aead_, wrap_key_)) != ZX_OK ||
        (rc = header_.Copy(block_.get(), kHeaderLen)) != ZX_OK ||
        (rc = aead.SetAD(header_)) != ZX_OK ||
        (rc = aead.Open(wrap_iv_, ctext, &ptext)) != ZX_OK ||
        (rc = ptext.Split(&data_iv_)) != ZX_OK || (rc = ptext.Split(&data_key_)) != ZX_OK) {
        return rc;
    }
    if (ptext.len() != 0) {
        xprintf("%s: %zu unused bytes\n", __PRETTY_FUNCTION__, ptext.len());
        return ZX_ERR_INTERNAL;
    }

    return ZX_OK;
}

// Device methods

zx_status_t Superblock::Ioctl(int op, const void* in, size_t in_len, void* out, size_t out_len) {
    zx_status_t rc;
    // Don't include debug messages here; some errors (e.g. ZX_ERR_NOT_SUPPORTED) are expected under
    // certain conditions (e.g. calling FVM ioctls on a non-FVM device).  Handle error reporting at
    // the call sites instead.
    if (dev_) {
        size_t actual;
        if ((rc = device_ioctl(dev_, op, in, in_len, out, out_len, &actual)) < 0) {
            return rc;
        }
    } else {
        ssize_t res;
        if ((res = fdio_ioctl(fd_.get(), op, in, in_len, out, out_len)) < 0) {
            return static_cast<zx_status_t>(res);
        }
    }
    return ZX_OK;
}

zx_status_t Superblock::Read() {
    if (dev_) {
        return SyncIO(dev_, IOTXN_OP_READ, block_.get(), offset_, block_.len());
    } else {
        if (lseek(fd_.get(), offset_, SEEK_SET) < 0) {
            xprintf("%s: lseek(%d, %" PRIu64 ", SEEK_SET) failed: %s\n", __PRETTY_FUNCTION__,
                    fd_.get(), offset_, strerror(errno));
            return ZX_ERR_IO;
        }
        ssize_t res;
        if ((res = read(fd_.get(), block_.get(), block_.len())) < 0) {
            xprintf("%s: read(%d, %p, %zu) failed: %s\n", __PRETTY_FUNCTION__, fd_.get(),
                    block_.get(), block_.len(), strerror(errno));
            return ZX_ERR_IO;
        }
        if (static_cast<size_t>(res) != block_.len()) {
            xprintf("%s: short read: have %zd, need %zu\n", __PRETTY_FUNCTION__, res, block_.len());
            return ZX_ERR_IO;
        }
        return ZX_OK;
    }
}

zx_status_t Superblock::Write() {
    if (dev_) {
        return SyncIO(dev_, IOTXN_OP_WRITE, block_.get(), offset_, block_.len());
    } else {
        if (lseek(fd_.get(), offset_, SEEK_SET) < 0) {
            xprintf("%s: lseek(%d, %" PRIu64 ", SEEK_SET) failed: %s\n", __PRETTY_FUNCTION__,
                    fd_.get(), offset_, strerror(errno));
            return ZX_ERR_IO;
        }
        ssize_t res;
        if ((res = write(fd_.get(), block_.get(), block_.len())) < 0) {
            xprintf("%s: write(%d, %p, %zu) failed: %s\n", __PRETTY_FUNCTION__, fd_.get(),
                    block_.get(), block_.len(), strerror(errno));
            return ZX_ERR_IO;
        }
        if (static_cast<size_t>(res) != block_.len()) {
            xprintf("%s: short read: have %zd, need %zu\n", __PRETTY_FUNCTION__, res, block_.len());
            return ZX_ERR_IO;
        }
        return ZX_OK;
    }
}

} // namespace zxcrypt
