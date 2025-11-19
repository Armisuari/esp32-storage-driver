#pragma once

#include "storage_config.h"
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

#if STORAGE_ENABLE_MUTEX_PROTECTION
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

/**
 * @brief File version information structure
 */
struct file_version_info {
    uint32_t version;
    size_t size;
    bool is_current;
};

/**
 * @brief File versioning manager
 * 
 * Handles automatic versioning of files with configurable history depth.
 * Works through callback interface for storage operations.
 */
class file_versioning {
    public:
        /**
         * @brief Callback interface for storage operations
         */
        struct storage_callbacks {
            std::function<std::string(const std::string&)> get_full_path;
            std::function<bool(const std::string&, void*, size_t)> read_file;
            std::function<bool(const std::string&, const void*, size_t)> write_file;
            std::function<bool(const std::string&)> delete_file;
            std::function<size_t(const std::string&)> get_file_size;
            std::function<bool(const std::string&)> file_exists;
            std::function<bool()> is_mounted;
        };

        /**
         * @brief Construct versioning manager with storage callbacks
         * @param callbacks Storage operation callbacks
         */
        explicit file_versioning(const storage_callbacks& callbacks);

        ~file_versioning();

        // Version query methods
        uint32_t get_file_version(const std::string& key);
        bool get_file_version_info(const std::string& key, file_version_info& info);
        std::vector<file_version_info> list_file_versions(const std::string& key);

        // Version read/restore methods
        bool read_file_version(const std::string& key, uint32_t version, void* data, size_t data_size);
        bool restore_file_version(const std::string& key, uint32_t version);

        // Version management methods
        bool archive_current_version(const std::string& key);
        bool file_has_changed(const std::string& key, uint32_t last_known_version);
        uint32_t cleanup_old_versions(const std::string& key);

        // Hook to be called before file write
        bool on_before_write(const std::string& key, const void* data, size_t size);

    private:
        storage_callbacks storage_ops;

#if STORAGE_ENABLE_MUTEX_PROTECTION
        SemaphoreHandle_t versioning_mutex;

        class mutex_guard {
        public:
            explicit mutex_guard(SemaphoreHandle_t& mutex) : m_mutex(mutex) {
                if (mutex) {
                    xSemaphoreTake(m_mutex, STORAGE_MUTEX_TIMEOUT_MS);
                }
            }
            ~mutex_guard() {
                if (m_mutex) {
                    xSemaphoreGive(m_mutex);
                }
            }
        private:
            SemaphoreHandle_t& m_mutex;
        };
#endif

        // Internal metadata structure
        struct file_version_metadata {
            uint32_t current_version;
            uint32_t file_size;
            uint32_t checksum;
            uint32_t version_count;
            uint32_t versions[STORAGE_MAX_VERSION_HISTORY];

            file_version_metadata() : current_version(0), file_size(0), 
                                       checksum(0), version_count(0) {
                for (size_t i = 0; i < STORAGE_MAX_VERSION_HISTORY; i++) {
                    versions[i] = 0;
                }
            }
        };

        // Helper methods
        std::string get_metadata_path(const std::string& key) const;
        std::string get_version_path(const std::string& key, uint32_t version) const;
        bool load_metadata(const std::string& key, file_version_metadata& metadata);
        bool save_metadata(const std::string& key, const file_version_metadata& metadata);
        uint32_t calculate_crc32(const void* data, size_t length) const;
        bool cleanup_oldest_version(const std::string& key, file_version_metadata& metadata);
};