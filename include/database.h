#pragma once

#include <memory>
#include <string>

#include "btree.h"
#include "buffer_pool.h"
#include "disk_manager.h"
#include "wal.h"

namespace reldb {

    // Ties the whole stack together in the order that makes recovery correct:
    // open the data file, open the WAL (which truncates its own torn tail),
    // REPLAY the WAL onto the data file, THEN — only then — start handing out
    // pages through a WAL-backed buffer pool. Getting this order right is the
    // actual point of the class; everything it does, you could do by hand,
    // but doing it by hand is exactly how you'd accidentally start the
    // buffer pool before recovery finishes and silently lose the recovery
    // guarantee.
    class Database {
    public:
        Database(const std::string& data_path, const std::string& wal_path,
                 size_t pool_size);

        BTree& tree() { return *btree_; }
        BufferPool& buffer_pool() { return *buffer_pool_; }

    private:
        DiskManager disk_manager_;
        WALManager wal_manager_;
        // Constructed after recovery runs, so its initial reads see the
        // already-recovered data file — order of member destruction/
        // construction here matters and is enforced by declaration order.
        std::unique_ptr<BufferPool> buffer_pool_;
        std::unique_ptr<BTree> btree_;
    };

}  // namespace reldb