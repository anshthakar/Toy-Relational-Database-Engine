#pragma once

#include "disk_manager.h"
#include "wal.h"

namespace reldb {

    // Replays the WAL onto the data file: for every page_id that appears in
    // WALManager::ValidRecords(), applies only its LATEST record (the one
    // with the highest LSN) as a direct write to the data file, then
    // refreshes DiskManager's page-id counter so subsequent AllocatePage()
    // calls don't collide with anything recovery just wrote.

    // Call this once, right after constructing DiskManager and WALManager
    // and before constructing BTree or doing anything else with either.
    void RunRecovery(DiskManager* disk_manager, WALManager* wal_manager);

}  // namespace reldb