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

#include "mbbootimg/format/mtk_writer_p.h"

#include <algorithm>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <openssl/sha.h>

#include "mbcommon/endian.h"
#include "mbcommon/file.h"
#include "mbcommon/file_util.h"
#include "mbcommon/string.h"

#include "mbbootimg/entry.h"
#include "mbbootimg/header.h"
#include "mbbootimg/writer.h"
#include "mbbootimg/writer_p.h"


namespace mb
{
namespace bootimg
{
namespace mtk
{

static int _mtk_header_update_size(MbBiWriter *biw, File &file,
                                   uint64_t offset, uint32_t size)
{
    uint32_t le32_size = mb_htole32(size);
    size_t n;

    if (offset > SIZE_MAX - offsetof(MtkHeader, size)) {
        mb_bi_writer_set_error(biw, ERROR_INTERNAL_ERROR,
                               "MTK header offset too large");
        return RET_FATAL;
    }

    if (!file.seek(offset + offsetof(MtkHeader, size), SEEK_SET, nullptr)) {
        mb_bi_writer_set_error(biw, file.error().value() /* TODO */,
                               "Failed to seek to MTK size field: %s",
                               file.error_string().c_str());
        return file.is_fatal() ? RET_FATAL : RET_FAILED;
    }

    if (!file_write_fully(file, &le32_size, sizeof(le32_size), n)) {
        mb_bi_writer_set_error(biw, file.error().value() /* TODO */,
                               "Failed to write MTK size field: %s",
                               file.error_string().c_str());
        return file.is_fatal() ? RET_FATAL : RET_FAILED;
    } else if (n != sizeof(le32_size)) {
        mb_bi_writer_set_error(biw, ERROR_FILE_FORMAT,
                               "Unexpected EOF when writing MTK size field");
        return RET_FAILED;
    }

    return RET_OK;
}

static int _mtk_compute_sha1(MbBiWriter *biw, SegmentWriter &seg,
                             File &file,
                             unsigned char digest[SHA_DIGEST_LENGTH])
{
    SHA_CTX sha_ctx;
    char buf[10240];
    size_t n;

    uint32_t kernel_mtkhdr_size = 0;
    uint32_t ramdisk_mtkhdr_size = 0;

    if (!SHA1_Init(&sha_ctx)) {
        mb_bi_writer_set_error(biw, ERROR_INTERNAL_ERROR,
                               "Failed to initialize SHA_CTX");
        return RET_FAILED;
    }

    for (size_t i = 0; i < seg.entries_size(); ++i) {
        auto const *entry = seg.entries_get(i);
        uint64_t remain = entry->size;

        if (!file.seek(entry->offset, SEEK_SET, nullptr)) {
            mb_bi_writer_set_error(biw, file.error().value() /* TODO */,
                                   "Failed to seek to entry %" MB_PRIzu ": %s",
                                   i, file.error_string().c_str());
            return file.is_fatal() ? RET_FATAL : RET_FAILED;
        }

        // Update checksum with data
        while (remain > 0) {
            size_t to_read = std::min<uint64_t>(remain, sizeof(buf));

            if (!file_read_fully(file, buf, to_read, n)) {
                mb_bi_writer_set_error(biw, file.error().value() /* TODO */,
                                       "Failed to read entry %" MB_PRIzu ": %s",
                                       i, file.error_string().c_str());
                return file.is_fatal() ? RET_FATAL : RET_FAILED;
            } else if (n != to_read) {
                mb_bi_writer_set_error(biw, file.error().value() /* TODO */,
                                       "Unexpected EOF when reading entry");
                return RET_FAILED;
            }

            if (!SHA1_Update(&sha_ctx, buf, n)) {
                mb_bi_writer_set_error(biw, ERROR_INTERNAL_ERROR,
                                       "Failed to update SHA1 hash");
                return RET_FAILED;
            }

            remain -= to_read;
        }

        uint32_t le32_size;

        // Update checksum with size
        switch (entry->type) {
        case ENTRY_TYPE_MTK_KERNEL_HEADER:
            kernel_mtkhdr_size = entry->size;
            continue;
        case ENTRY_TYPE_MTK_RAMDISK_HEADER:
            ramdisk_mtkhdr_size = entry->size;
            continue;
        case ENTRY_TYPE_KERNEL:
            le32_size = mb_htole32(entry->size + kernel_mtkhdr_size);
            break;
        case ENTRY_TYPE_RAMDISK:
            le32_size = mb_htole32(entry->size + ramdisk_mtkhdr_size);
            break;
        case ENTRY_TYPE_SECONDBOOT:
            le32_size = mb_htole32(entry->size);
            break;
        case ENTRY_TYPE_DEVICE_TREE:
            if (entry->size == 0) {
                continue;
            }
            le32_size = mb_htole32(entry->size);
            break;
        default:
            continue;
        }

        if (!SHA1_Update(&sha_ctx, &le32_size, sizeof(le32_size))) {
            mb_bi_writer_set_error(biw, ERROR_INTERNAL_ERROR,
                                   "Failed to update SHA1 hash");
            return RET_FAILED;
        }
    }

    if (!SHA1_Final(digest, &sha_ctx)) {
        mb_bi_writer_set_error(biw, ERROR_INTERNAL_ERROR,
                               "Failed to finalize SHA1 hash");
        return RET_FATAL;
    }

    return RET_OK;
}

int mtk_writer_get_header(MbBiWriter *biw, void *userdata, Header &header)
{
    (void) biw;
    (void) userdata;

    header.set_supported_fields(SUPPORTED_FIELDS);

    return RET_OK;
}

int mtk_writer_write_header(MbBiWriter *biw, void *userdata,
                            const Header &header)
{
    MtkWriterCtx *const ctx = static_cast<MtkWriterCtx *>(userdata);
    int ret;

    // Construct header
    memset(&ctx->hdr, 0, sizeof(ctx->hdr));
    memcpy(ctx->hdr.magic, android::BOOT_MAGIC, android::BOOT_MAGIC_SIZE);

    if (auto address = header.kernel_address()) {
        ctx->hdr.kernel_addr = *address;
    }
    if (auto address = header.ramdisk_address()) {
        ctx->hdr.ramdisk_addr = *address;
    }
    if (auto address = header.secondboot_address()) {
        ctx->hdr.second_addr = *address;
    }
    if (auto address = header.kernel_tags_address()) {
        ctx->hdr.tags_addr = *address;
    }
    if (auto page_size = header.page_size()) {
        switch (*page_size) {
        case 2048:
        case 4096:
        case 8192:
        case 16384:
        case 32768:
        case 65536:
        case 131072:
            ctx->hdr.page_size = *page_size;
            break;
        default:
            mb_bi_writer_set_error(biw, ERROR_FILE_FORMAT,
                                   "Invalid page size: %" PRIu32, *page_size);
            return RET_FAILED;
        }
    } else {
        mb_bi_writer_set_error(biw, ERROR_FILE_FORMAT,
                               "Page size field is required");
        return RET_FAILED;
    }

    if (auto board_name = header.board_name()) {
        if (board_name->size() >= sizeof(ctx->hdr.name)) {
            mb_bi_writer_set_error(biw, ERROR_FILE_FORMAT,
                                   "Board name too long");
            return RET_FAILED;
        }

        strncpy(reinterpret_cast<char *>(ctx->hdr.name), board_name->c_str(),
                sizeof(ctx->hdr.name) - 1);
        ctx->hdr.name[sizeof(ctx->hdr.name) - 1] = '\0';
    }
    if (auto cmdline = header.kernel_cmdline()) {
        if (cmdline->size() >= sizeof(ctx->hdr.cmdline)) {
            mb_bi_writer_set_error(biw, ERROR_FILE_FORMAT,
                                   "Kernel cmdline too long");
            return RET_FAILED;
        }

        strncpy(reinterpret_cast<char *>(ctx->hdr.cmdline), cmdline->c_str(),
                sizeof(ctx->hdr.cmdline) - 1);
        ctx->hdr.cmdline[sizeof(ctx->hdr.cmdline) - 1] = '\0';
    }

    // TODO: UNUSED
    // TODO: ID

    // Clear existing entries (none should exist unless this function fails and
    // the user reattempts to call it)
    ctx->seg.entries_clear();

    ret = ctx->seg.entries_add(ENTRY_TYPE_MTK_KERNEL_HEADER,
                               0, false, 0, biw);
    if (ret != RET_OK) return ret;

    ret = ctx->seg.entries_add(ENTRY_TYPE_KERNEL,
                               0, false, ctx->hdr.page_size, biw);
    if (ret != RET_OK) return ret;

    ret = ctx->seg.entries_add(ENTRY_TYPE_MTK_RAMDISK_HEADER,
                               0, false, 0, biw);
    if (ret != RET_OK) return ret;

    ret = ctx->seg.entries_add(ENTRY_TYPE_RAMDISK,
                               0, false, ctx->hdr.page_size, biw);
    if (ret != RET_OK) return ret;

    ret = ctx->seg.entries_add(ENTRY_TYPE_SECONDBOOT,
                               0, false, ctx->hdr.page_size, biw);
    if (ret != RET_OK) return ret;

    ret = ctx->seg.entries_add(ENTRY_TYPE_DEVICE_TREE,
                               0, false, ctx->hdr.page_size, biw);
    if (ret != RET_OK) return ret;

    // Start writing after first page
    if (!biw->file->seek(ctx->hdr.page_size, SEEK_SET, nullptr)) {
        mb_bi_writer_set_error(biw, biw->file->error().value() /* TODO */,
                               "Failed to seek to first page: %s",
                               biw->file->error_string().c_str());
        return biw->file->is_fatal() ? RET_FATAL : RET_FAILED;
    }

    return RET_OK;
}

int mtk_writer_get_entry(MbBiWriter *biw, void *userdata,
                         Entry &entry)
{
    MtkWriterCtx *const ctx = static_cast<MtkWriterCtx *>(userdata);

    return ctx->seg.get_entry(*biw->file, entry, biw);
}

int mtk_writer_write_entry(MbBiWriter *biw, void *userdata,
                           const Entry &entry)
{
    MtkWriterCtx *const ctx = static_cast<MtkWriterCtx *>(userdata);

    return ctx->seg.write_entry(*biw->file, entry, biw);
}

int mtk_writer_write_data(MbBiWriter *biw, void *userdata,
                          const void *buf, size_t buf_size,
                          size_t &bytes_written)
{
    MtkWriterCtx *const ctx = static_cast<MtkWriterCtx *>(userdata);

    return ctx->seg.write_data(*biw->file, buf, buf_size, bytes_written, biw);
}

int mtk_writer_finish_entry(MbBiWriter *biw, void *userdata)
{
    MtkWriterCtx *const ctx = static_cast<MtkWriterCtx *>(userdata);
    int ret;

    ret = ctx->seg.finish_entry(*biw->file, biw);
    if (ret != RET_OK) {
        return ret;
    }

    auto const *swentry = ctx->seg.entry();

    if ((swentry->type == ENTRY_TYPE_KERNEL
            || swentry->type == ENTRY_TYPE_RAMDISK)
            && swentry->size == UINT32_MAX - sizeof(MtkHeader)) {
        mb_bi_writer_set_error(biw, ERROR_FILE_FORMAT,
                               "Entry size too large to accomodate MTK header");
        return RET_FATAL;
    } else if ((swentry->type == ENTRY_TYPE_MTK_KERNEL_HEADER
            || swentry->type == ENTRY_TYPE_MTK_RAMDISK_HEADER)
            && swentry->size != sizeof(MtkHeader)) {
        mb_bi_writer_set_error(biw, ERROR_FILE_FORMAT,
                               "Invalid size for MTK header entry");
        return RET_FATAL;
    }

    switch (swentry->type) {
    case ENTRY_TYPE_KERNEL:
        ctx->hdr.kernel_size = swentry->size + sizeof(MtkHeader);
        break;
    case ENTRY_TYPE_RAMDISK:
        ctx->hdr.ramdisk_size = swentry->size + sizeof(MtkHeader);
        break;
    case ENTRY_TYPE_SECONDBOOT:
        ctx->hdr.second_size = swentry->size;
        break;
    case ENTRY_TYPE_DEVICE_TREE:
        ctx->hdr.dt_size = swentry->size;
        break;
    }

    return RET_OK;
}

int mtk_writer_close(MbBiWriter *biw, void *userdata)
{
    MtkWriterCtx *const ctx = static_cast<MtkWriterCtx *>(userdata);
    int ret;
    size_t n;

    if (!ctx->have_file_size) {
        if (!biw->file->seek(0, SEEK_CUR, &ctx->file_size)) {
            mb_bi_writer_set_error(biw, biw->file->error().value() /* TODO */,
                                   "Failed to get file offset: %s",
                                   biw->file->error_string().c_str());
            return biw->file->is_fatal() ? RET_FATAL : RET_FAILED;
        }

        ctx->have_file_size = true;
    }

    auto const *swentry = ctx->seg.entry();

    // If successful, finish up the boot image
    if (!swentry) {
        // Truncate to set size
        if (!biw->file->truncate(ctx->file_size)) {
            mb_bi_writer_set_error(biw, biw->file->error().value() /* TODO */,
                                   "Failed to truncate file: %s",
                                   biw->file->error_string().c_str());
            return biw->file->is_fatal() ? RET_FATAL : RET_FAILED;
        }

        // Update MTK header sizes
        for (size_t i = 0; i < ctx->seg.entries_size(); ++i) {
            auto const *entry = ctx->seg.entries_get(i);
            switch (entry->type) {
            case ENTRY_TYPE_MTK_KERNEL_HEADER:
                ret = _mtk_header_update_size(biw, *biw->file, entry->offset,
                                              ctx->hdr.kernel_size
                                              - sizeof(MtkHeader));
                break;
            case ENTRY_TYPE_MTK_RAMDISK_HEADER:
                ret = _mtk_header_update_size(biw, *biw->file, entry->offset,
                                              ctx->hdr.ramdisk_size
                                              - sizeof(MtkHeader));
                break;
            default:
                continue;
            }

            if (ret != RET_OK) {
                return ret;
            }
        }

        // We need to take the performance hit and compute the SHA1 here.
        // We can't fill in the sizes in the MTK headers when we're writing
        // them. Thus, if we calculated the SHA1sum during write, it would be
        // incorrect.
        ret = _mtk_compute_sha1(biw, ctx->seg, *biw->file,
                                reinterpret_cast<unsigned char *>(ctx->hdr.id));
        if (ret != RET_OK) {
            return ret;
        }

        // Convert fields back to little-endian
        android::AndroidHeader hdr = ctx->hdr;
        android_fix_header_byte_order(hdr);

        // Seek back to beginning to write header
        if (!biw->file->seek(0, SEEK_SET, nullptr)) {
            mb_bi_writer_set_error(biw, biw->file->error().value() /* TODO */,
                                   "Failed to seek to beginning: %s",
                                   biw->file->error_string().c_str());
            return biw->file->is_fatal() ? RET_FATAL : RET_FAILED;
        }

        // Write header
        if (!file_write_fully(*biw->file, &hdr, sizeof(hdr), n)
                || n != sizeof(hdr)) {
            mb_bi_writer_set_error(biw, biw->file->error().value() /* TODO */,
                                   "Failed to write header: %s",
                                   biw->file->error_string().c_str());
            return biw->file->is_fatal() ? RET_FATAL : RET_FAILED;
        }
    }

    return RET_OK;
}

int mtk_writer_free(MbBiWriter *bir, void *userdata)
{
    (void) bir;
    delete static_cast<MtkWriterCtx *>(userdata);
    return RET_OK;
}

}

/*!
 * \brief Set MTK boot image output format
 *
 * \param biw MbBiWriter
 *
 * \return
 *   * #RET_OK if the format is successfully enabled
 *   * #RET_WARN if the format is already enabled
 *   * \<= #RET_FAILED if an error occurs
 */
int mb_bi_writer_set_format_mtk(MbBiWriter *biw)
{
    using namespace mtk;

    MtkWriterCtx *const ctx = new MtkWriterCtx();

    return _mb_bi_writer_register_format(biw,
                                         ctx,
                                         FORMAT_MTK,
                                         FORMAT_NAME_MTK,
                                         nullptr,
                                         &mtk_writer_get_header,
                                         &mtk_writer_write_header,
                                         &mtk_writer_get_entry,
                                         &mtk_writer_write_entry,
                                         &mtk_writer_write_data,
                                         &mtk_writer_finish_entry,
                                         &mtk_writer_close,
                                         &mtk_writer_free);
}

}
}
