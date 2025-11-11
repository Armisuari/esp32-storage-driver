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
 * @class storage_esp
 * @brief ESP32 filesystem storage implementation using native ESP-IDF APIs
 * 
 * This class provides a unified interface to ESP32 filesystem operations,
 * supporting both LittleFS and SPIFFS using native ESP-IDF APIs without
 * Arduino framework dependencies.
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
        bool _exists_impl(const std::string& key);
};