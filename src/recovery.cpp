#include "recovery.h"

#include <unordered_map>

namespace reldb {

    void RunRecovery(DiskManager* disk_manager, WALManager* wal_manager) {
        // Reduce to "latest record per page_id" — a page can appear many times
        // across the log (every mutation to it logs a fresh full-image record),
        // and only the most recent one reflects its true final state. Earlier
        // records for the same page are superseded, not separate history to
        // replay in order; because each record carries the COMPLETE page
        // image, not a delta, applying only the latest one is sufficient and
        // correct — there's no intermediate state to reconstruct.
        std::unordered_map<page_id_t, const WALManager::RecoveredPage*> latest;
        for (const auto& record : wal_manager->ValidRecords()) {
            auto it = latest.find(record.page_id);
            if (it == latest.end() || record.lsn > it->second->lsn) {
                latest[record.page_id] = &record;
            }
        }

        for (auto& [page_id, record] : latest) {
            // WritePage recomputes the page's checksum before writing — correct
            // here too, since the recovered bytes are the authoritative
            // after-image regardless of where they came from.
            Page page_copy = record->page;
            disk_manager->WritePage(page_id, &page_copy);
        }

        disk_manager->Flush();

        // Must happen AFTER all recovery writes: those writes can extend the
        // data file past whatever page count DiskManager's constructor saw
        // (which predates recovery), so the allocation counter has to be
        // re-derived from the file's now-final size before anything calls
        // AllocatePage() again.
        disk_manager->RefreshNextPageIdAfterRecovery();
    }

}  // namespace reldb