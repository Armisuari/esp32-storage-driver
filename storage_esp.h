/**
 * @file storage_esp.h
 * @brief ESP32 filesystem storage implementation using native ESP-IDF APIs.
 * @author Armisuari
 */
#pragma once

#include <interface/storage_interface.h>
#include "storage_config.h"

// Native ESP-IDF includes
#include "esp_vfs.h"
#include "esp_err.h"

// FreeRTOS includes for thread safety
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Include ESP-IDF filesystem headers based on availability
#if defined(CONFIG_LITTLEFS_PAGE_SIZE) || defined(STORAGE_LITTLEFS_AVAILABLE)
    #include "esp_littlefs.h"
    #define STORAGE_LITTLEFS_SUPPORTED
#endif

#if defined(CONFIG_SPIFFS_PAGE_SIZE) || defined(STORAGE_SPIFFS_AVAILABLE)
    #include "esp_spiffs.h"
    #define STORAGE_SPIFFS_SUPPORTED
#endif

#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

#if STORAGE_ENABLE_VERSIONING
#include <vector>
#include <algorithm>
#endif

/**
 * @class mutex_guard
 * @brief RAII wrapper for FreeRTOS mutex
 * 
 * Provides automatic mutex locking/unlocking similar to std::lock_guard
 */
class mutex_guard
{
public:
    explicit mutex_guard(SemaphoreHandle_t mutex) : _mutex(mutex) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
    }
    
    ~mutex_guard() {
        xSemaphoreGive(_mutex);
    }
    
    // Non-copyable and non-movable
    mutex_guard(const mutex_guard&) = delete;
    mutex_guard& operator=(const mutex_guard&) = delete;
    mutex_guard(mutex_guard&&) = delete;
    mutex_guard& operator=(mutex_guard&&) = delete;

private:
    SemaphoreHandle_t _mutex;
};

/**
 * @struct file_version_metadata
 * @brief Structure to store file version information
 */
struct file_version_metadata {
    uint32_t current_version;     ///< Current version number (increments on each write)
    uint32_t timestamp;           ///< Last modification timestamp (epoch seconds)
    uint32_t file_size;           ///< Size of the current version
    uint32_t checksum;            ///< Simple CRC32 checksum of file content
    uint32_t version_count;       ///< Number of historical versions stored
    uint32_t versions[STORAGE_MAX_VERSION_HISTORY]; ///< List of available version numbers
    
    // Default constructor
    file_version_metadata() : current_version(0), timestamp(0), file_size(0), 
                             checksum(0), version_count(0) {
        for (int i = 0; i < STORAGE_MAX_VERSION_HISTORY; i++) {
            versions[i] = 0;
        }
    }
};

/**
 * @struct file_version_info
 * @brief Public interface for file version information
 */
struct file_version_info {
    uint32_t version;
    uint32_t timestamp;
    size_t size;
    bool is_current;
};

/**
 * @class storage_esp
 * @brief ESP32 filesystem storage implementation using native ESP-IDF APIs
 * 
 * This class provides a unified interface to ESP32 filesystem operations,
 * supporting both LittleFS and SPIFFS using native ESP-IDF APIs without
 * Arduino framework dependencies. Now includes file versioning capabilities.
 */
class storage_esp : public storage_interface
{
    public:
        /**
         * @brief Constructor
         * @param filesystem_type Optional filesystem type override
         * @param partition_label Optional partition label override
         * @param base_path Optional VFS mount base path override
         */
        explicit storage_esp(storage_filesystem_t filesystem_type = STORAGE_DEFAULT_TYPE, 
                           const char* partition_label = STORAGE_DEFAULT_PARTITION_LABEL,
                           const char* base_path = "/storage");
        
        /**
         * @brief Destructor - unmounts the filesystem
         */
        ~storage_esp();

        /**
         * @brief Initialize and mount the filesystem
         * @return true if successfully mounted, false otherwise
         */
        bool begin() override;

        /**
         * @brief Read data from a file
         * @param key File path/key
         * @param data Buffer to store read data
         * @param dataSize Size of data to read
         * @return true if successfully read, false otherwise
         */
        bool read_file(const std::string& key, void* data, size_t dataSize) override;

        /**
         * @brief Write data to a file
         * @param key File path/key
         * @param data Data to write
         * @param dataSize Size of data to write
         * @return true if successfully written, false otherwise
         */
        bool write_file(const std::string& key, const void* data, size_t dataSize) override;

        /**
         * @brief Delete a file
         * @param key File path/key
         * @return true if successfully deleted, false otherwise
         */
        bool erase_file(const std::string& key) override;

        /**
         * @brief Get file size
         * @param key File path/key
         * @return File size in bytes, 0 if file doesn't exist or error
         */
        size_t file_size(const std::string& key) override;

        /**
         * @brief Check if file exists
         * @param key File path/key
         * @return true if file exists, false otherwise
         */
        bool exists(const std::string& key) override;

        /**
         * @brief Get total filesystem size
         * @return Total size in bytes
         */
        size_t total_size() const;

        /**
         * @brief Get used filesystem space
         * @return Used size in bytes
         */
        size_t used_size() const;

        /**
         * @brief Format the filesystem (DANGEROUS - erases all data)
         * @return true if successfully formatted, false otherwise
         */
        bool format();

        /**
         * @brief Get the full filesystem path for a given key
         * @param key File key
         * @return Full path string
         */
        std::string get_full_path(const std::string& key) const;

        // ========== File Versioning Methods ==========
        
        /**
         * @brief Get current version number of a file
         * @param key File path/key
         * @return Current version number, 0 if file doesn't exist
         */
        uint32_t get_file_version(const std::string& key);
        
        /**
         * @brief Get comprehensive version information for a file
         * @param key File path/key
         * @param[out] info Version information structure
         * @return true if file exists and info retrieved
         */
        bool get_file_version_info(const std::string& key, file_version_info& info);
        
        /**
         * @brief List all available versions of a file
         * @param key File path/key
         * @return Vector of version information for all stored versions
         */
        std::vector<file_version_info> list_file_versions(const std::string& key);
        
        /**
         * @brief Read a specific version of a file
         * @param key File path/key
         * @param version Version number to read (0 = current version)
         * @param data Buffer to store read data
         * @param dataSize Size of data to read
         * @return true if successfully read
         */
        bool read_file_version(const std::string& key, uint32_t version, void* data, size_t dataSize);
        
        /**
         * @brief Check if a file has been modified since a specific version
         * @param key File path/key
         * @param last_known_version Last known version number
         * @return true if file has been modified (version is newer)
         */
        bool file_has_changed(const std::string& key, uint32_t last_known_version);
        
        /**
         * @brief Restore a file to a previous version
         * @param key File path/key
         * @param version Version number to restore to
         * @return true if successfully restored
         */
        bool restore_file_version(const std::string& key, uint32_t version);
        
        /**
         * @brief Clean up old versions beyond the configured history limit
         * @param key File path/key (if empty, cleans all files)
         * @return Number of versions cleaned up
         */
        uint32_t cleanup_old_versions(const std::string& key = "");

    private:
        storage_filesystem_t _fs_type;        ///< Selected filesystem type
        const char* _partition_label;         ///< Partition label
        const char* _base_path;               ///< VFS mount path
        bool _is_mounted;                     ///< Mount status flag
        mutable SemaphoreHandle_t _mutex;     ///< FreeRTOS mutex for thread safety
        
        // ESP-IDF configuration handles
        union {
#ifdef STORAGE_LITTLEFS_SUPPORTED
            esp_vfs_littlefs_conf_t littlefs_conf;
#endif
#ifdef STORAGE_SPIFFS_SUPPORTED
            esp_vfs_spiffs_conf_t spiffs_conf;
#endif
        } _fs_conf;

        /**
         * @brief Initialize LittleFS filesystem
         * @return ESP_OK if successful
         */
        esp_err_t _init_littlefs();

        /**
         * @brief Initialize SPIFFS filesystem
         * @return ESP_OK if successful
         */
        esp_err_t _init_spiffs();

        /**
         * @brief Unmount and deinitialize filesystem
         */
        void _deinit_filesystem();

        /**
         * @brief Validate file path
         * @param key File path to validate
         * @return true if valid, false otherwise
         */
        bool _is_valid_path(const std::string& key) const;

        /**
         * @brief Ensure directory exists for file path
         * @param filepath Full file path
         * @return true if directory exists or was created
         */
        bool _ensure_directory_exists(const std::string& filepath) const;

        /**
         * @brief Internal implementation of read_file without locking
         * @param key File path/key
         * @param data Buffer to store read data
         * @param dataSize Size of data to read
         * @return true if successfully read, false otherwise
         */
        bool _read_file_impl(const std::string& key, void* data, size_t dataSize);

        /**
         * @brief Internal implementation of write_file without locking
         * @param key File path/key
         * @param data Data to write
         * @param dataSize Size of data to write
         * @return true if successfully written, false otherwise
         */
        bool _write_file_impl(const std::string& key, const void* data, size_t dataSize);

        /**
         * @brief Internal implementation of erase_file without locking
         * @param key File path/key
         * @return true if successfully deleted, false otherwise
         */
        bool _erase_file_impl(const std::string& key);

        /**
         * @brief Internal implementation of file_size without locking
         * @param key File path/key
         * @return File size in bytes, 0 if file doesn't exist or error
         */
        size_t _file_size_impl(const std::string& key);

        /**
         * @brief Internal implementation of exists without locking
         * @param key File path/key
         * @return true if file exists, false otherwise
         */
        bool _exists_internal(const std::string& key);

        /**
         * @brief Internal implementation of exists without locking
         * @param key File path/key
         * @return true if file exists, false otherwise
         */
        bool _exists_impl(const std::string& key);

        // ========== Private Versioning Helper Methods ==========
        
        /**
         * @brief Get metadata file path for a given file key
         * @param key File path/key
         * @return Metadata file path
         */
        std::string _get_metadata_path(const std::string& key) const;
        
        /**
         * @brief Get versioned file path for a given file key and version
         * @param key File path/key
         * @param version Version number
         * @return Versioned file path
         */
        std::string _get_version_path(const std::string& key, uint32_t version) const;
        
        /**
         * @brief Load metadata for a file
         * @param key File path/key
         * @param[out] metadata Metadata structure to fill
         * @return true if metadata loaded successfully
         */
        bool _load_metadata(const std::string& key, file_version_metadata& metadata);
        
        /**
         * @brief Save metadata for a file
         * @param key File path/key
         * @param metadata Metadata structure to save
         * @return true if metadata saved successfully
         */
        bool _save_metadata(const std::string& key, const file_version_metadata& metadata);
        
        /**
         * @brief Calculate CRC32 checksum of data
         * @param data Data to checksum
         * @param length Data length
         * @return CRC32 checksum
         */
        uint32_t _calculate_crc32(const void* data, size_t length) const;
        
        /**
         * @brief Get current timestamp in epoch seconds
         * @return Current timestamp
         */
        uint32_t _get_timestamp() const;
        
        /**
         * @brief Archive current file as a versioned backup
         * @param key File path/key
         * @param metadata Current metadata
         * @return true if archiving successful
         */
        bool _archive_current_version(const std::string& key, file_version_metadata& metadata);
        
        /**
         * @brief Remove oldest version to make room for new ones
         * @param key File path/key
         * @param metadata Current metadata (will be updated)
         * @return true if cleanup successful
         */
        bool _cleanup_oldest_version(const std::string& key, file_version_metadata& metadata);
};