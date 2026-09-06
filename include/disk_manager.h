#pragma once

#include <atomic>
#include <string>

#include "page.h"

namespace reldb {

    class DiskManager {
    public:
        explicit DiskManager(const std::string& db_file);
        ~DiskManager();

        DiskManager(const DiskManager&) = delete;
        DiskManager& operator=(const DiskManager&) = delete;

        void ReadPage(page_id_t page_id, Page* page);
        void WritePage(page_id_t page_id, Page* page);
        page_id_t AllocatePage();
        void Flush();
        size_t NumPages() const;

    private:
        int fd_;
        std::atomic<page_id_t> next_page_id_;
    };

}  // namespace reldb