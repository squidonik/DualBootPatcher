/*
 * Copyright (C) 2015-2017  Andrew Gunnerson <andrewgunnerson@gmail.com>
 *
 * This file is part of DualBootPatcher
 *
 * DualBootPatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * DualBootPatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with DualBootPatcher.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mbbootimg/format/android_reader_p.h"

#include <algorithm>
#include <type_traits>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "mbcommon/endian.h"
#include "mbcommon/file.h"
#include "mbcommon/file_util.h"
#include "mbcommon/libc/string.h"
#include "mbcommon/string.h"

#include "mbbootimg/entry.h"
#include "mbbootimg/format/align_p.h"
#include "mbbootimg/format/bump_defs.h"
#include "mbbootimg/header.h"
#include "mbbootimg/reader.h"
#include "mbbootimg/reader_p.h"


namespace mb
{
namespace bootimg
{
namespace android
{

/*!
 * \brief Find and read Android boot image header
 *
 * \note The integral fields in the header will be converted to the host's byte
 *       order.
 *
 * \pre The file position can be at any offset prior to calling this function.
 *
 * \post The file pointer position is undefined after this function returns.
 *       Use File::seek() to return to a known position.
 *
 * \param[in] bir MbBiReader for setting error messages
 * \param[in] file File handle
 * \param[in] max_header_offset Maximum offset that a header can start (must be
 *                              less than #MAX_HEADER_OFFSET)
 * \param[out] header_out Pointer to store header
 * \param[out] offset_out Pointer to store header offset
 *
 * \return
 *   * #RET_OK if the header is found
 *   * #RET_WARN if the header is not found
 *   * #RET_FAILED if any file operation fails non-fatally
 *   * #RET_FATAL if any file operation fails fatally
 */
int find_android_header(MbBiReader *bir, File &file,
                        uint64_t max_header_offset,
                        AndroidHeader &header_out, uint64_t &offset_out)
{
    unsigned char buf[MAX_HEADER_OFFSET + sizeof(AndroidHeader)];
    size_t n;
    void *ptr;
    size_t offset;

    if (max_header_offset > MAX_HEADER_OFFSET) {
        mb_bi_reader_set_error(bir, ERROR_INVALID_ARGUMENT,
                               "Max header offset (%" PRIu64
                               ") must be less than %" MB_PRIzu,
                               max_header_offset, MAX_HEADER_OFFSET);
        return RET_WARN;
    }

    if (!file.seek(0, SEEK_SET, nullptr)) {
        mb_bi_reader_set_error(bir, file.error().value() /* TODO */,
                               "Failed to seek to beginning: %s",
                               file.error_string().c_str());
        return file.is_fatal() ? RET_FATAL : RET_FAILED;
    }

    if (!file_read_fully(file, buf,
                         max_header_offset + sizeof(AndroidHeader), n)) {
        mb_bi_reader_set_error(bir, file.error().value() /* TODO */,
                               "Failed to read header: %s",
                               file.error_string().c_str());
        return file.is_fatal() ? RET_FATAL : RET_FAILED;
    }

    ptr = mb_memmem(buf, n, BOOT_MAGIC, BOOT_MAGIC_SIZE);
    if (!ptr) {
        mb_bi_reader_set_error(bir, ERROR_FILE_FORMAT,
                               "Android magic not found in first %" MB_PRIzu
                               " bytes", MAX_HEADER_OFFSET);
        return RET_WARN;
    }

    offset = static_cast<unsigned char *>(ptr) - buf;

    if (n - offset < sizeof(AndroidHeader)) {
        mb_bi_reader_set_error(bir, ERROR_FILE_FORMAT,
                               "Android header at %" MB_PRIzu
                               " exceeds file size", offset);
        return RET_WARN;
    }

    // Copy header
    memcpy(&header_out, ptr, sizeof(AndroidHeader));
    android_fix_header_byte_order(header_out);
    offset_out = offset;

    return RET_OK;
}

/*!
 * \brief Find location of Samsung SEAndroid magic
 *
 * \pre The file position can be at any offset prior to calling this function.
 *
 * \post The file pointer position is undefined after this function returns.
 *       Use File::seek() to return to a known position.
 *
 * \param[in] bir MbBiReader for setting error messages
 * \param[in] file File handle
 * \param[in] hdr Android boot image header (in host byte order)
 * \param[out] offset_out Pointer to store magic offset
 *
 * \return
 *   * #RET_OK if the magic is found
 *   * #RET_WARN if the magic is not found
 *   * #RET_FAILED if any file operation fails non-fatally
 *   * #RET_FATAL if any file operation fails fatally
 */
int find_samsung_seandroid_magic(MbBiReader *bir, File &file,
                                 const AndroidHeader &hdr, uint64_t &offset_out)
{
    unsigned char buf[SAMSUNG_SEANDROID_MAGIC_SIZE];
    size_t n;
    uint64_t pos = 0;

    // Skip header, whose size cannot exceed the page size
    pos += hdr.page_size;

    // Skip kernel
    pos += hdr.kernel_size;
    pos += align_page_size<uint64_t>(pos, hdr.page_size);

    // Skip ramdisk
    pos += hdr.ramdisk_size;
    pos += align_page_size<uint64_t>(pos, hdr.page_size);

    // Skip second bootloader
    pos += hdr.second_size;
    pos += align_page_size<uint64_t>(pos, hdr.page_size);

    // Skip device tree
    pos += hdr.dt_size;
    pos += align_page_size<uint64_t>(pos, hdr.page_size);

    if (!file.seek(pos, SEEK_SET, nullptr)) {
        mb_bi_reader_set_error(bir, file.error().value() /* TODO */,
                               "SEAndroid magic not found: %s",
                               file.error_string().c_str());
        return file.is_fatal() ? RET_FATAL : RET_FAILED;
    }

    if (!file_read_fully(file, buf, sizeof(buf), n)) {
        mb_bi_reader_set_error(bir, file.error().value() /* TODO */,
                               "Failed to read SEAndroid magic: %s",
                               file.error_string().c_str());
        return file.is_fatal() ? RET_FATAL : RET_FAILED;
    }

    if (n != SAMSUNG_SEANDROID_MAGIC_SIZE
            || memcmp(buf, SAMSUNG_SEANDROID_MAGIC, n) != 0) {
        mb_bi_reader_set_error(bir, ERROR_FILE_FORMAT,
                               "SEAndroid magic not found in last %" MB_PRIzu
                               " bytes", SAMSUNG_SEANDROID_MAGIC_SIZE);
        return RET_WARN;
    }

    offset_out = pos;
    return RET_OK;
}

/*!
 * \brief Find location of Bump magic
 *
 * \pre The file position can be at any offset prior to calling this function.
 *
 * \post The file pointer position is undefined after this function returns.
 *       Use File::seek() to return to a known position.
 *
 * \param[in] bir MbBiReader for setting error messages
 * \param[in] file File handle
 * \param[in] hdr Android boot image header (in host byte order)
 * \param[out] offset_out Pointer to store magic offset
 *
 * \return
 *   * #RET_OK if the magic is found
 *   * #RET_WARN if the magic is not found
 *   * #RET_FAILED if any file operation fails non-fatally
 *   * #RET_FATAL if any file operation fails fatally
 */
int find_bump_magic(MbBiReader *bir, File &file,
                    const AndroidHeader &hdr, uint64_t &offset_out)
{
    unsigned char buf[bump::BUMP_MAGIC_SIZE];
    size_t n;
    uint64_t pos = 0;

    // Skip header, whose size cannot exceed the page size
    pos += hdr.page_size;

    // Skip kernel
    pos += hdr.kernel_size;
    pos += align_page_size<uint64_t>(pos, hdr.page_size);

    // Skip ramdisk
    pos += hdr.ramdisk_size;
    pos += align_page_size<uint64_t>(pos, hdr.page_size);

    // Skip second bootloader
    pos += hdr.second_size;
    pos += align_page_size<uint64_t>(pos, hdr.page_size);

    // Skip device tree
    pos += hdr.dt_size;
    pos += align_page_size<uint64_t>(pos, hdr.page_size);

    if (!file.seek(pos, SEEK_SET, nullptr)) {
        mb_bi_reader_set_error(bir, file.error().value() /* TODO */,
                               "SEAndroid magic not found: %s",
                               file.error_string().c_str());
        return file.is_fatal() ? RET_FATAL : RET_FAILED;
    }

    if (!file_read_fully(file, buf, sizeof(buf), n)) {
        mb_bi_reader_set_error(bir, file.error().value() /* TODO */,
                               "Failed to read SEAndroid magic: %s",
                               file.error_string().c_str());
        return file.is_fatal() ? RET_FATAL : RET_FAILED;
    }

    if (n != bump::BUMP_MAGIC_SIZE || memcmp(buf, bump::BUMP_MAGIC, n) != 0) {
        mb_bi_reader_set_error(bir, ERROR_FILE_FORMAT,
                               "Bump magic not found in last %" MB_PRIzu
                               " bytes", bump::BUMP_MAGIC_SIZE);
        return RET_WARN;
    }

    offset_out = pos;
    return RET_OK;
}

int android_set_header(const AndroidHeader &hdr, Header &header)
{
    char board_name[sizeof(hdr.name) + 1];
    char cmdline[sizeof(hdr.cmdline) + 1];

    strncpy(board_name, reinterpret_cast<const char *>(hdr.name),
            sizeof(hdr.name));
    strncpy(cmdline, reinterpret_cast<const char *>(hdr.cmdline),
            sizeof(hdr.cmdline));
    board_name[sizeof(hdr.name)] = '\0';
    cmdline[sizeof(hdr.cmdline)] = '\0';

    header.set_supported_fields(SUPPORTED_FIELDS);

    if (!header.set_board_name({board_name})
            || !header.set_kernel_cmdline({cmdline})
            || !header.set_page_size(hdr.page_size)
            || !header.set_kernel_address(hdr.kernel_addr)
            || !header.set_ramdisk_address(hdr.ramdisk_addr)
            || !header.set_secondboot_address(hdr.second_addr)
            || !header.set_kernel_tags_address(hdr.tags_addr)) {
        return RET_UNSUPPORTED;
    }

    // TODO: unused
    // TODO: id

    return RET_OK;
}

/*!
 * \brief Perform a bid
 *
 * \return
 *   * If \>= 0, the number of bits that conform to the Android format
 *   * #RET_WARN if this is a bid that can't be won
 *   * #RET_FAILED if any file operations fail non-fatally
 *   * #RET_FATAL if any file operations fail fatally
 */
int android_reader_bid(MbBiReader *bir, void *userdata, int best_bid)
{
    AndroidReaderCtx *const ctx = static_cast<AndroidReaderCtx *>(userdata);
    int bid = 0;
    int ret;

    if (best_bid >= static_cast<int>(
            BOOT_MAGIC_SIZE + SAMSUNG_SEANDROID_MAGIC_SIZE) * 8) {
        // This is a bid we can't win, so bail out
        return RET_WARN;
    }

    // Find the Android header
    ret = find_android_header(bir, *bir->file, MAX_HEADER_OFFSET,
                              ctx->hdr, ctx->header_offset);
    if (ret == RET_OK) {
        // Update bid to account for matched bits
        ctx->have_header_offset = true;
        bid += BOOT_MAGIC_SIZE * 8;
    } else if (ret == RET_WARN) {
        // Header not found. This can't be an Android boot image.
        return 0;
    } else {
        return ret;
    }

    // Find the Samsung magic
    ret = find_samsung_seandroid_magic(bir, *bir->file, ctx->hdr,
                                       ctx->samsung_offset);
    if (ret == RET_OK) {
        // Update bid to account for matched bits
        ctx->have_samsung_offset = true;
        bid += SAMSUNG_SEANDROID_MAGIC_SIZE * 8;
    } else if (ret == RET_WARN) {
        // Nothing found. Don't change bid
    } else {
        return ret;
    }

    return bid;
}

/*!
 * \brief Perform a bid
 *
 * \return
 *   * If \>= 0, the number of bits that conform to the Bump format
 *   * #RET_WARN if this is a bid that can't be won
 *   * #RET_FAILED if any file operations fail non-fatally
 *   * #RET_FATAL if any file operations fail fatally
 */
int bump_reader_bid(MbBiReader *bir, void *userdata, int best_bid)
{
    AndroidReaderCtx *const ctx = static_cast<AndroidReaderCtx *>(userdata);
    int bid = 0;
    int ret;

    if (best_bid >= static_cast<int>(
            BOOT_MAGIC_SIZE + bump::BUMP_MAGIC_SIZE) * 8) {
        // This is a bid we can't win, so bail out
        return RET_WARN;
    }

    // Find the Android header
    ret = find_android_header(bir, *bir->file, MAX_HEADER_OFFSET,
                              ctx->hdr, ctx->header_offset);
    if (ret == RET_OK) {
        // Update bid to account for matched bits
        ctx->have_header_offset = true;
        bid += BOOT_MAGIC_SIZE * 8;
    } else if (ret == RET_WARN) {
        // Header not found. This can't be an Android boot image.
        return 0;
    } else {
        return ret;
    }

    // Find the Bump magic
    ret = find_bump_magic(bir, *bir->file, ctx->hdr, ctx->bump_offset);
    if (ret == RET_OK) {
        // Update bid to account for matched bits
        ctx->have_bump_offset = true;
        bid += bump::BUMP_MAGIC_SIZE * 8;
    } else if (ret == RET_WARN) {
        // Nothing found. Don't change bid
    } else {
        return ret;
    }

    return bid;
}

int android_reader_set_option(MbBiReader *bir, void *userdata,
                              const char *key, const char *value)
{
    (void) bir;

    AndroidReaderCtx *const ctx = static_cast<AndroidReaderCtx *>(userdata);

    if (strcmp(key, "strict") == 0) {
        bool strict = strcasecmp(value, "true") == 0
                || strcasecmp(value, "yes") == 0
                || strcasecmp(value, "y") == 0
                || strcmp(value, "1") == 0;
        ctx->allow_truncated_dt = !strict;
        return RET_OK;
    } else {
        return RET_WARN;
    }
}

int android_reader_read_header(MbBiReader *bir, void *userdata, Header &header)
{
    AndroidReaderCtx *const ctx = static_cast<AndroidReaderCtx *>(userdata);
    int ret;

    if (!ctx->have_header_offset) {
        // A bid might not have been performed if the user forced a particular
        // format
        ret = find_android_header(bir, *bir->file, MAX_HEADER_OFFSET,
                                  ctx->hdr, ctx->header_offset);
        if (ret < 0) {
            return ret;
        }
        ctx->have_header_offset = true;
    }

    ret = android_set_header(ctx->hdr, header);
    if (ret != RET_OK) {
        mb_bi_reader_set_error(bir, ERROR_INTERNAL_ERROR,
                               "Failed to set header fields");
        return ret;
    }

    // Calculate offsets for each section

    uint64_t pos = 0;
    uint32_t page_size = *header.page_size();
    uint64_t kernel_offset;
    uint64_t ramdisk_offset;
    uint64_t second_offset;
    uint64_t dt_offset;

    // pos cannot overflow due to the nature of the operands (adding UINT32_MAX
    // a few times can't overflow a uint64_t). File length overflow is checked
    // during read.

    // Header
    pos += ctx->header_offset;
    pos += sizeof(AndroidHeader);
    pos += align_page_size<uint64_t>(pos, page_size);

    // Kernel
    kernel_offset = pos;
    pos += ctx->hdr.kernel_size;
    pos += align_page_size<uint64_t>(pos, page_size);

    // Ramdisk
    ramdisk_offset = pos;
    pos += ctx->hdr.ramdisk_size;
    pos += align_page_size<uint64_t>(pos, page_size);

    // Second bootloader
    second_offset = pos;
    pos += ctx->hdr.second_size;
    pos += align_page_size<uint64_t>(pos, page_size);

    // Device tree
    dt_offset = pos;
    pos += ctx->hdr.dt_size;
    pos += align_page_size<uint64_t>(pos, page_size);

    ctx->seg.entries_clear();

    ret = ctx->seg.entries_add(ENTRY_TYPE_KERNEL,
                               kernel_offset, ctx->hdr.kernel_size, false, bir);
    if (ret != RET_OK) return ret;

    ret = ctx->seg.entries_add(ENTRY_TYPE_RAMDISK,
                               ramdisk_offset, ctx->hdr.ramdisk_size, false,
                               bir);
    if (ret != RET_OK) return ret;

    if (ctx->hdr.second_size > 0) {
        ret = ctx->seg.entries_add(ENTRY_TYPE_SECONDBOOT,
                                   second_offset, ctx->hdr.second_size, false,
                                   bir);
        if (ret != RET_OK) return ret;
    }

    if (ctx->hdr.dt_size > 0) {
        ret = ctx->seg.entries_add(ENTRY_TYPE_DEVICE_TREE,
                                   dt_offset, ctx->hdr.dt_size,
                                   ctx->allow_truncated_dt, bir);
        if (ret != RET_OK) return ret;
    }

    return RET_OK;
}

int android_reader_read_entry(MbBiReader *bir, void *userdata, Entry &entry)
{
    AndroidReaderCtx *const ctx = static_cast<AndroidReaderCtx *>(userdata);

    return ctx->seg.read_entry(*bir->file, entry, bir);
}

int android_reader_go_to_entry(MbBiReader *bir, void *userdata, Entry &entry,
                               int entry_type)
{
    AndroidReaderCtx *const ctx = static_cast<AndroidReaderCtx *>(userdata);

    return ctx->seg.go_to_entry(*bir->file, entry, entry_type, bir);
}

int android_reader_read_data(MbBiReader *bir, void *userdata,
                             void *buf, size_t buf_size,
                             size_t &bytes_read)
{
    AndroidReaderCtx *const ctx = static_cast<AndroidReaderCtx *>(userdata);

    return ctx->seg.read_data(*bir->file, buf, buf_size, bytes_read, bir);
}

int android_reader_free(MbBiReader *bir, void *userdata)
{
    (void) bir;
    delete static_cast<AndroidReaderCtx *>(userdata);
    return RET_OK;
}

}

/*!
 * \brief Enable support for Android boot image format
 *
 * \param bir MbBiReader
 *
 * \return
 *   * #RET_OK if the format is successfully enabled
 *   * #RET_WARN if the format is already enabled
 *   * \<= #RET_FAILED if an error occurs
 */
int mb_bi_reader_enable_format_android(MbBiReader *bir)
{
    using namespace android;

    AndroidReaderCtx *const ctx = new AndroidReaderCtx();

    // Allow truncated dt image by default
    ctx->allow_truncated_dt = true;

    return _mb_bi_reader_register_format(bir,
                                         ctx,
                                         FORMAT_ANDROID,
                                         FORMAT_NAME_ANDROID,
                                         &android_reader_bid,
                                         &android_reader_set_option,
                                         &android_reader_read_header,
                                         &android_reader_read_entry,
                                         &android_reader_go_to_entry,
                                         &android_reader_read_data,
                                         &android_reader_free);
}

}
}
