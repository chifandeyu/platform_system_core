/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "fs_mgr_dm_linear.h"

#include <inttypes.h>
#include <linux/dm-ioctl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sstream>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <liblp/reader.h>

#include "fs_mgr_priv.h"

namespace android {
namespace fs_mgr {

using DeviceMapper = android::dm::DeviceMapper;
using DmTable = android::dm::DmTable;
using DmTarget = android::dm::DmTarget;
using DmTargetZero = android::dm::DmTargetZero;
using DmTargetLinear = android::dm::DmTargetLinear;

static bool CreateDmDeviceForPartition(DeviceMapper& dm, const LogicalPartition& partition) {
    DmTable table;
    for (const auto& extent : partition.extents) {
        table.AddTarget(std::make_unique<DmTargetLinear>(extent));
    }
    if (!dm.CreateDevice(partition.name, table)) {
        return false;
    }
    LINFO << "Created device-mapper device: " << partition.name;
    return true;
}

bool CreateLogicalPartitions(const LogicalPartitionTable& table) {
    DeviceMapper& dm = DeviceMapper::Instance();
    for (const auto& partition : table.partitions) {
        if (!CreateDmDeviceForPartition(dm, partition)) {
            LOG(ERROR) << "could not create dm-linear device for partition: " << partition.name;
            return false;
        }
    }
    return true;
}

std::unique_ptr<LogicalPartitionTable> LoadPartitionsFromDeviceTree() {
    return nullptr;
}

static bool CreateDmTable(const std::string& block_device, const LpMetadata& metadata,
                          const LpMetadataPartition& partition, DmTable* table) {
    uint64_t sector = 0;
    for (size_t i = 0; i < partition.num_extents; i++) {
        const auto& extent = metadata.extents[partition.first_extent_index + i];
        std::unique_ptr<DmTarget> target;
        switch (extent.target_type) {
            case LP_TARGET_TYPE_ZERO:
                target = std::make_unique<DmTargetZero>(sector, extent.num_sectors);
                break;
            case LP_TARGET_TYPE_LINEAR:
                target = std::make_unique<DmTargetLinear>(sector, extent.num_sectors, block_device,
                                                          extent.target_data);
                break;
            default:
                LOG(ERROR) << "Unknown target type in metadata: " << extent.target_type;
                return false;
        }
        if (!table->AddTarget(std::move(target))) {
            return false;
        }
        sector += extent.num_sectors;
    }
    if (partition.attributes & LP_PARTITION_ATTR_READONLY) {
        table->set_readonly(true);
    }
    return true;
}

bool CreateLogicalPartitions(const std::string& block_device) {
    uint32_t slot = SlotNumberForSlotSuffix(fs_mgr_get_slot_suffix());
    auto metadata = ReadMetadata(block_device.c_str(), slot);
    if (!metadata) {
        LOG(ERROR) << "Could not read partition table.";
        return true;
    }

    DeviceMapper& dm = DeviceMapper::Instance();
    for (const auto& partition : metadata->partitions) {
        DmTable table;
        if (!CreateDmTable(block_device, *metadata.get(), partition, &table)) {
            return false;
        }
        std::string name = GetPartitionName(partition);
        if (!dm.CreateDevice(name, table)) {
            return false;
        }
        std::string path;
        dm.GetDmDevicePathByName(partition.name, &path);
        LINFO << "Created logical partition " << name << " on device " << path;
    }
    return true;
}

}  // namespace fs_mgr
}  // namespace android
