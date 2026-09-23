#include "database.h"

#include "recovery.h"

namespace reldb {

    Database::Database(const std::string& data_path, const std::string& wal_path,
                        size_t pool_size)
        : disk_manager_(data_path), wal_manager_(wal_path) {
        // WALManager's constructor already ran by this point (member init
        // order above) and already truncated any torn tail from a prior crash
        // — ValidRecords() below only ever sees records that are individually
        // intact.
        RunRecovery(&disk_manager_, &wal_manager_);

        // Only now, after the data file reflects every durable mutation, does
        // anything start reading pages through the buffer pool. BTree's own
        // constructor (called next) reads the metadata page to find the root
        // — if that read happened before RunRecovery, it could see
        // pre-recovery bytes.
        buffer_pool_ = std::make_unique<BufferPool>(pool_size, &disk_manager_,
                                                     &wal_manager_);
        btree_ = std::make_unique<BTree>(buffer_pool_.get());
    }

}  // namespace reldb