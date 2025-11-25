#include "storage_esp.h"
#include <unistd.h>
#include <errno.h>
#include <algorithm>

static const char* TAG = "storage_esp";

// ========== Constructors and Destructor ==========

storage_esp::storage_esp() : storage_esp(STORAGE_DEFAULT_TYPE, STORAGE_DEFAULT_PARTITION_LABEL, STORAGE_DEFAULT_BASE_PATH) {
}

storage_esp::storage_esp(storage_type_t type, const std::string& partition)
    : _storage_type(type), _partition_label(partition), _is_mounted(false) {
    
    if (type == STORAGE_TYPE_SPIFFS) {
        _base_path = STORAGE_SPIFFS_BASE_PATH;
    } else {
        _base_path = STORAGE_LITTLEFS_BASE_PATH;
    }
    
    _init_default_config();
}

storage_esp::storage_esp(storage_type_t type, const std::string& partition, const std::string& mount_point)
    : _storage_type(type), _partition_label(partition), _base_path(mount_point), _is_mounted(false) {
    
    _init_default_config();
}

storage_esp::~storage_esp() {
    if (_is_mounted) {
        unmount();
    }
    
#if STORAGE_ENABLE_MUTEX_PROTECTION
    if (_storage_mutex != nullptr) {
        vSemaphoreDelete(_storage_mutex);
    }
#endif
}

void storage_esp::_init_default_config() {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    _storage_mutex = xSemaphoreCreateMutex();
    if (_storage_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create storage mutex");
    }
#endif

#if STORAGE_ENABLE_DEBUG_LOGGING
    ESP_LOGI(TAG, "Storage initialized: type=%s, partition=%s, base_path=%s",
             _get_storage_type_name(), _partition_label.c_str(), _base_path.c_str());
#endif
}

// ========== Versioning Initialization ==========

#if STORAGE_ENABLE_VERSIONING
void storage_esp::_init_versioning() {
    if (_versioning) {
        return; // Already initialized
    }
    
    // Create versioning with callback interface
    file_versioning::storage_callbacks callbacks;
    
    callbacks.get_full_path = [this](const std::string& key) -> std::string {
        return this->_get_full_path(key);
    };
    
    callbacks.read_file = [this](const std::string& key, void* data, size_t size) -> bool {
        return this->_read_file_no_mutex(key, data, size);
    };
    
    callbacks.write_file = [this](const std::string& key, const void* data, size_t size) -> bool {
        return this->_write_file_no_mutex(key, data, size);
    };
    
    callbacks.delete_file = [this](const std::string& key) -> bool {
        std::string full_path = this->_get_full_path(key);
        return unlink(full_path.c_str()) == 0;
    };
    
    callbacks.get_file_size = [this](const std::string& key) -> size_t {
        std::string full_path = this->_get_full_path(key);
        struct stat st;
        if (stat(full_path.c_str(), &st) == 0) {
            return st.st_size;
        }
        return 0;
    };
    
    callbacks.file_exists = [this](const std::string& key) -> bool {
        std::string full_path = this->_get_full_path(key);
        struct stat st;
        return stat(full_path.c_str(), &st) == 0;
    };
    
    callbacks.is_mounted = [this]() -> bool {
        return this->_is_mounted;
    };
    
    _versioning = std::make_unique<file_versioning>(callbacks);
    
#if STORAGE_ENABLE_DEBUG_LOGGING
    ESP_LOGI(TAG, "File versioning initialized");
#endif
}
#endif

// ========== Helper Methods ==========

std::string storage_esp::_get_full_path(const std::string& relative_path) const {
    if (relative_path.empty()) {
        return _base_path;
    }
    if (relative_path[0] != '/') {
        return _base_path + "/" + relative_path;
    }
    return _base_path + relative_path;
}

// ========== Public Interface Methods ==========

bool storage_esp::begin() {
    return mount(STORAGE_FORMAT_IF_MOUNT_FAILS);
}

bool storage_esp::mount(bool format_on_fail) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(_storage_mutex);
#endif

    if (_is_mounted) {
        ESP_LOGW(TAG, "Storage already mounted");
        return true;
    }
    
    esp_err_t ret = ESP_FAIL;
    
    if (_storage_type == STORAGE_TYPE_SPIFFS) {
#ifdef STORAGE_SPIFFS_AVAILABLE
        esp_vfs_spiffs_conf_t conf = {
            .base_path = _base_path.c_str(),
            .partition_label = _partition_label.c_str(),
            .max_files = STORAGE_MAX_FILES,
            .format_if_mount_failed = format_on_fail
        };
        
        ret = esp_vfs_spiffs_register(&conf);
        if (ret == ESP_OK) {
#if STORAGE_ENABLE_DEBUG_LOGGING
            ESP_LOGI(TAG, "SPIFFS mounted successfully on %s", _base_path.c_str());
#endif
            _is_mounted = true;
        } else {
            ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(ret));
            if (!format_on_fail) {
                ESP_LOGW(TAG, "Try mounting with format_on_fail=true if needed");
            }
        }
#else
        ESP_LOGE(TAG, "SPIFFS not available - check storage_config.h");
        return false;
#endif
    } else {
#ifdef STORAGE_LITTLEFS_AVAILABLE
        esp_vfs_littlefs_conf_t conf = {
            .base_path = _base_path.c_str(),
            .partition_label = _partition_label.c_str(),
            .format_if_mount_failed = format_on_fail,
            .dont_mount = false
        };
        
        ret = esp_vfs_littlefs_register(&conf);
        if (ret == ESP_OK) {
#if STORAGE_ENABLE_DEBUG_LOGGING
            ESP_LOGI(TAG, "LittleFS mounted successfully on %s", _base_path.c_str());
#endif
            _is_mounted = true;
            
            // Log filesystem info after successful mount
            size_t total = 0, used = 0;
            esp_littlefs_info(_partition_label.c_str(), &total, &used);
#if STORAGE_ENABLE_DEBUG_LOGGING
            ESP_LOGI(TAG, "LittleFS info - Total: %d bytes, Used: %d bytes", total, used);
#endif
        } else {
            ESP_LOGE(TAG, "Failed to mount LittleFS: %s", esp_err_to_name(ret));
            if (!format_on_fail) {
                ESP_LOGW(TAG, "Try mounting with format_on_fail=true if needed");
            }
        }
#else
        ESP_LOGE(TAG, "LittleFS not available - check storage_config.h");
        return false;
#endif
    }
    
#if STORAGE_ENABLE_VERSIONING
    if (_is_mounted) {
        _init_versioning();
    }
#endif
    
    return ret == ESP_OK;
}

bool storage_esp::unmount() {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(_storage_mutex);
#endif

    if (!_is_mounted) {
        ESP_LOGW(TAG, "Storage not mounted");
        return true;
    }
    
    bool ret = false;
    
    if (_storage_type == STORAGE_TYPE_SPIFFS) {
#ifdef STORAGE_SPIFFS_AVAILABLE
        ret = esp_vfs_spiffs_unregister(_partition_label.c_str()) == ESP_OK;
#endif
    } else {
#ifdef STORAGE_LITTLEFS_AVAILABLE
        ret = esp_vfs_littlefs_unregister(_partition_label.c_str()) == ESP_OK;
#endif
    }
    
    if (ret) {
#if STORAGE_ENABLE_DEBUG_LOGGING
        ESP_LOGI(TAG, "%s unmounted successfully", _get_storage_type_name());
#endif
        _is_mounted = false;
    } else {
        ESP_LOGE(TAG, "Failed to unmount %s", _get_storage_type_name());
    }
    
    return ret;
}

bool storage_esp::format() {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(_storage_mutex);
#endif

    if (!_is_mounted) {
        ESP_LOGE(TAG, "Storage not mounted, cannot format");
        return false;
    }
    
    bool ret = false;
    
    if (_storage_type == STORAGE_TYPE_SPIFFS) {
#ifdef STORAGE_SPIFFS_AVAILABLE
        ret = esp_spiffs_format(_partition_label.c_str()) == ESP_OK;
#endif
    } else {
#ifdef STORAGE_LITTLEFS_AVAILABLE
        ret = esp_littlefs_format(_partition_label.c_str()) == ESP_OK;
#endif
    }
    
    if (ret) {
#if STORAGE_ENABLE_DEBUG_LOGGING
        ESP_LOGI(TAG, "%s formatted successfully", _get_storage_type_name());
#endif
    } else {
        ESP_LOGE(TAG, "Failed to format %s", _get_storage_type_name());
    }
    
    return ret;
}

bool storage_esp::exists(const std::string& key) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(_storage_mutex);
#endif

    if (!_is_mounted) {
        return false;
    }
    
    std::string full_path = _get_full_path(key);
    struct stat st;
    return stat(full_path.c_str(), &st) == 0;
}

size_t storage_esp::file_size(const std::string& key) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(_storage_mutex);
#endif

    if (!_is_mounted) {
        return 0;
    }
    
    std::string full_path = _get_full_path(key);
    struct stat st;
    if (stat(full_path.c_str(), &st) == 0) {
        return st.st_size;
    }
    return 0;
}

bool storage_esp::read_file(const std::string& key, void* data, size_t data_size) {
    return _read_file_internal(key, data, data_size);
}

bool storage_esp::write_file(const std::string& key, const void* data, size_t data_size) {
#if STORAGE_ENABLE_VERSIONING
    // Notify versioning before write
    if (_versioning) {
        _versioning->on_before_write(key, data, data_size);
    }
#endif
    
    return _write_file_internal(key, data, data_size);
}

bool storage_esp::erase_file(const std::string& key) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(_storage_mutex);
#endif

    if (!_is_mounted) {
        return false;
    }
    
    std::string full_path = _get_full_path(key);
    
    if (unlink(full_path.c_str()) == 0) {
#if STORAGE_ENABLE_DEBUG_LOGGING
        ESP_LOGD(TAG, "Deleted file: %s", key.c_str());
#endif
        
#if STORAGE_ENABLE_VERSIONING
        // Also delete version metadata and version files
        if (_versioning) {
            _versioning->cleanup_old_versions(key);
            std::string meta_path = full_path + STORAGE_VERSION_METADATA_EXT;
            unlink(meta_path.c_str());
        }
#endif
        return true;
    }
    
    ESP_LOGE(TAG, "Failed to delete file: %s", full_path.c_str());
    return false;
}

size_t storage_esp::total_size() {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(_storage_mutex);
#endif

    if (!_is_mounted) {
        return 0;
    }
    
    size_t total = 0, used = 0;
    
    if (_storage_type == STORAGE_TYPE_SPIFFS) {
#ifdef STORAGE_SPIFFS_AVAILABLE
        esp_spiffs_info(_partition_label.c_str(), &total, &used);
#endif
    } else {
#ifdef STORAGE_LITTLEFS_AVAILABLE
        esp_littlefs_info(_partition_label.c_str(), &total, &used);
#endif
    }
    
    return total;
}

size_t storage_esp::used_size() {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(_storage_mutex);
#endif

    if (!_is_mounted) {
        return 0;
    }
    
    size_t total = 0, used = 0;
    
    if (_storage_type == STORAGE_TYPE_SPIFFS) {
#ifdef STORAGE_SPIFFS_AVAILABLE
        esp_spiffs_info(_partition_label.c_str(), &total, &used);
#endif
    } else {
#ifdef STORAGE_LITTLEFS_AVAILABLE
        esp_littlefs_info(_partition_label.c_str(), &total, &used);
#endif
    }
    
    return used;
}

// ========== Internal File Operations ==========

bool storage_esp::_read_file_internal(const std::string& key, void* data, size_t data_size) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(_storage_mutex);
#endif

    if (!_is_mounted || !data) {
        return false;
    }
    
    std::string full_path = _get_full_path(key);
    
    FILE* f = fopen(full_path.c_str(), "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", full_path.c_str());
        return false;
    }
    
    size_t bytes_read = fread(data, 1, data_size, f);
    fclose(f);
    
    // Note: It's normal for files to be smaller than the buffer size
    // Only error if we read 0 bytes and the file should exist
    if (bytes_read == 0 && data_size > 0) {
        ESP_LOGE(TAG, "Failed to read any data from %s", key.c_str());
        return false;
    }
    
#if STORAGE_ENABLE_DEBUG_LOGGING
    ESP_LOGD(TAG, "Read %zu bytes from %s (requested %zu)", bytes_read, key.c_str(), data_size);
#endif
    return true;
}

bool storage_esp::_write_file_internal(const std::string& key, const void* data, size_t data_size) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(_storage_mutex);
#endif

    if (!_is_mounted || !data) {
        return false;
    }
    
    std::string full_path = _get_full_path(key);
    
    // Create parent directories if needed
    size_t last_slash = full_path.rfind('/');
    if (last_slash != std::string::npos && last_slash > _base_path.length()) {
        std::string dir_path = full_path.substr(0, last_slash);
        _create_directory_recursive(dir_path);
    }
    
    FILE* f = fopen(full_path.c_str(), "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", full_path.c_str());
        return false;
    }
    
    size_t bytes_written = fwrite(data, 1, data_size, f);
    fclose(f);
    
    if (bytes_written != data_size) {
        ESP_LOGE(TAG, "Write size mismatch: expected %d, got %d", data_size, bytes_written);
        return false;
    }
    
#if STORAGE_ENABLE_DEBUG_LOGGING
    ESP_LOGD(TAG, "Wrote %d bytes to %s", bytes_written, key.c_str());
#endif
    return true;
}

// Mutex-free version for internal callbacks to avoid deadlock
bool storage_esp::_write_file_no_mutex(const std::string& key, const void* data, size_t data_size) {
    if (!_is_mounted || !data) {
        return false;
    }
    
    std::string full_path = _get_full_path(key);
    
    // Create parent directories if needed
    size_t last_slash = full_path.rfind('/');
    if (last_slash != std::string::npos && last_slash > _base_path.length()) {
        std::string dir_path = full_path.substr(0, last_slash);
        _create_directory_recursive(dir_path);
    }
    
    FILE* f = fopen(full_path.c_str(), "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", full_path.c_str());
        return false;
    }
    
    size_t bytes_written = fwrite(data, 1, data_size, f);
    fclose(f);
    
    if (bytes_written != data_size) {
        ESP_LOGE(TAG, "Write size mismatch: expected %zu, got %zu", data_size, bytes_written);
        return false;
    }
    
#if STORAGE_ENABLE_DEBUG_LOGGING
    ESP_LOGD(TAG, "Wrote %zu bytes to %s", bytes_written, key.c_str());
#endif
    return true;
}

// Mutex-free version for internal callbacks to avoid deadlock
bool storage_esp::_read_file_no_mutex(const std::string& key, void* data, size_t data_size) {
    if (!_is_mounted || !data) {
        return false;
    }
    
    std::string full_path = _get_full_path(key);
    
    FILE* f = fopen(full_path.c_str(), "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", full_path.c_str());
        return false;
    }
    
    size_t bytes_read = fread(data, 1, data_size, f);
    fclose(f);
    
    // Note: It's normal for files to be smaller than the buffer size
    // Only error if we read 0 bytes and the file should exist
    if (bytes_read == 0 && data_size > 0) {
        ESP_LOGE(TAG, "Failed to read any data from %s", key.c_str());
        return false;
    }
    
#if STORAGE_ENABLE_DEBUG_LOGGING
    ESP_LOGD(TAG, "Read %zu bytes from %s (requested %zu)", bytes_read, key.c_str(), data_size);
#endif
    return true;
}

// ========== Directory Operations ==========

bool storage_esp::_create_directory_recursive(const std::string& path) {
    // Skip if path is just the base path
    if (path == _base_path || path.length() <= _base_path.length()) {
        return true;
    }
    
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    
    // Create parent directory first
    size_t last_slash = path.rfind('/');
    if (last_slash != std::string::npos && last_slash > _base_path.length()) {
        std::string parent = path.substr(0, last_slash);
        if (!_create_directory_recursive(parent)) {
            return false;
        }
    }
    
    // Create this directory
    if (mkdir(path.c_str(), STORAGE_DIR_PERMISSIONS) == 0 || errno == EEXIST) {
        return true;
    }
    
    ESP_LOGE(TAG, "Failed to create directory: %s", path.c_str());
    return false;
}

bool storage_esp::create_directory(const std::string& path) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(_storage_mutex);
#endif

    if (!_is_mounted) {
        return false;
    }

    std::string full_path = _get_full_path(path);
    return _create_directory_recursive(full_path);
}

bool storage_esp::list_directory(const std::string& path, std::vector<file_info_t>& files) {
    if (!_is_mounted) {
        return false;
    }
    
    std::string full_path = _get_full_path(path);
    
    DIR* dir = opendir(full_path.c_str());
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", full_path.c_str());
        return false;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        std::string entry_path = full_path + "/" + entry->d_name;
        struct stat st;
        
        if (stat(entry_path.c_str(), &st) == 0) {
            file_info_t info;
            info.path = path + "/" + entry->d_name;
            info.size = st.st_size;
            info.is_directory = S_ISDIR(st.st_mode);
            files.push_back(info);
        }
    }
    
    closedir(dir);
    return true;
}

bool storage_esp::list_all_files(std::vector<file_info_t>& files) {
    std::vector<std::string> dirs_to_scan;
    dirs_to_scan.push_back("/");
    
    while (!dirs_to_scan.empty()) {
        std::string current_dir = dirs_to_scan.back();
        dirs_to_scan.pop_back();
        
        std::vector<file_info_t> dir_contents;
        if (!list_directory(current_dir, dir_contents)) {
            continue;
        }
        
        for (const auto& item : dir_contents) {
            if (item.is_directory) {
                dirs_to_scan.push_back(item.path);
            } else {
                files.push_back(item);
            }
        }
    }
    
    return true;
}

// ========== Advanced File Operations ==========

bool storage_esp::read_file_alloc(const std::string& key, uint8_t** data, size_t* size) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(_storage_mutex);
#endif

    if (!_is_mounted || !data || !size) {
        return false;
    }

    *size = file_size(key);
    if (*size == 0) {
        return false;
    }

    *data = (uint8_t*)malloc(*size);
    if (!*data) {
        ESP_LOGE(TAG, "Failed to allocate memory for file: %s", key.c_str());
        return false;
    }

    if (!_read_file_internal(key, *data, *size)) {
        free(*data);
        *data = nullptr;
        *size = 0;
        return false;
    }

    return true;
}

bool storage_esp::rename_file(const std::string& old_key, const std::string& new_key) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(_storage_mutex);
#endif

    if (!_is_mounted) {
        return false;
    }

    std::string old_path = _get_full_path(old_key);
    std::string new_path = _get_full_path(new_key);

    if (rename(old_path.c_str(), new_path.c_str()) == 0) {
#if STORAGE_ENABLE_DEBUG_LOGGING
        ESP_LOGD(TAG, "Renamed file: %s -> %s", old_key.c_str(), new_key.c_str());
#endif
        return true;
    }

    ESP_LOGE(TAG, "Failed to rename file: %s -> %s", old_key.c_str(), new_key.c_str());
    return false;
}

bool storage_esp::verify_file_integrity(const std::string& key, size_t expected_size, uint32_t* checksum) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(_storage_mutex);
#endif

    if (!_is_mounted) {
        return false;
    }

    // Check if file exists and has expected size
    size_t actual_size = file_size(key);
    if (actual_size != expected_size) {
        ESP_LOGE(TAG, "File size mismatch for %s: expected %d, actual %d", 
                 key.c_str(), expected_size, actual_size);
        return false;
    }

    // If checksum verification is requested
    if (checksum != nullptr) {
        // Read file and calculate checksum (simple implementation)
        uint8_t* data = nullptr;
        size_t size = 0;
        
        if (!read_file_alloc(key, &data, &size)) {
            return false;
        }

        uint32_t calculated_checksum = 0;
        for (size_t i = 0; i < size; i++) {
            calculated_checksum += data[i];
        }

        free(data);

        if (calculated_checksum != *checksum) {
            ESP_LOGE(TAG, "Checksum mismatch for %s: expected 0x%08X, calculated 0x%08X",
                     key.c_str(), *checksum, calculated_checksum);
            return false;
        }
    }

    return true;
}