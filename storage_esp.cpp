#include "storage_esp.h"
#include <unistd.h>
#include <errno.h>
#include <algorithm>

static const char* TAG = "storage_esp";

storage_esp::storage_esp() : storage_esp(STORAGE_DEFAULT_TYPE, STORAGE_DEFAULT_PARTITION_LABEL, STORAGE_DEFAULT_BASE_PATH) {
}

storage_esp::storage_esp(storage_type_t type, const std::string& partition)
    : storage_type(type), partition_label(partition), is_mounted(false) {
    
    if (type == STORAGE_TYPE_SPIFFS) {
        base_path = STORAGE_SPIFFS_BASE_PATH;
    } else {
        base_path = STORAGE_LITTLEFS_BASE_PATH;
    }
    
    init_default_config();
}

storage_esp::storage_esp(storage_type_t type, const std::string& partition, const std::string& mount_point)
    : storage_type(type), partition_label(partition), base_path(mount_point), is_mounted(false) {
    
    init_default_config();
}

storage_esp::~storage_esp() {
    if (is_mounted) {
        unmount();
    }
    
#if STORAGE_ENABLE_MUTEX_PROTECTION
    if (storage_mutex != nullptr) {
        vSemaphoreDelete(storage_mutex);
    }
#endif
}

void storage_esp::init_default_config() {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    storage_mutex = xSemaphoreCreateMutex();
    if (storage_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create storage mutex");
    }
#endif

#if STORAGE_ENABLE_DEBUG_LOGGING
    ESP_LOGI(TAG, "Storage initialized: type=%s, partition=%s, base_path=%s",
             get_storage_type_name(), partition_label.c_str(), base_path.c_str());
#endif
}

std::string storage_esp::get_full_path(const std::string& relative_path) const {
    if (relative_path.empty()) {
        return base_path;
    }
    if (relative_path[0] != '/') {
        return base_path + "/" + relative_path;
    }
    return base_path + relative_path;
}

bool storage_esp::begin() {
    return mount(STORAGE_FORMAT_IF_MOUNT_FAILS);
}

bool storage_esp::mount(bool format_on_fail) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(storage_mutex);
#endif

    if (is_mounted) {
        ESP_LOGW(TAG, "Storage already mounted");
        return true;
    }
    
    esp_err_t ret = ESP_FAIL;
    
    if (storage_type == STORAGE_TYPE_SPIFFS) {
#ifdef STORAGE_SPIFFS_AVAILABLE
        esp_vfs_spiffs_conf_t conf = {
            .base_path = base_path.c_str(),
            .partition_label = partition_label.c_str(),
            .max_files = STORAGE_MAX_FILES,
            .format_if_mount_failed = format_on_fail
        };
        
        ret = esp_vfs_spiffs_register(&conf);
        if (ret == ESP_OK) {
#if STORAGE_ENABLE_DEBUG_LOGGING
            ESP_LOGI(TAG, "SPIFFS mounted successfully on %s", base_path.c_str());
#endif
            is_mounted = true;
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
            .base_path = base_path.c_str(),
            .partition_label = partition_label.c_str(),
            .format_if_mount_failed = format_on_fail,
            .dont_mount = false
        };
        
        ret = esp_vfs_littlefs_register(&conf);
        if (ret == ESP_OK) {
#if STORAGE_ENABLE_DEBUG_LOGGING
            ESP_LOGI(TAG, "LittleFS mounted successfully on %s", base_path.c_str());
#endif
            is_mounted = true;
            
            // Log filesystem info after successful mount
            size_t total = 0, used = 0;
            esp_littlefs_info(partition_label.c_str(), &total, &used);
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
    
    return ret == ESP_OK;
}

bool storage_esp::unmount() {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(storage_mutex);
#endif

    if (!is_mounted) {
        ESP_LOGW(TAG, "Storage not mounted");
        return true;
    }
    
    bool ret = false;
    
    if (storage_type == STORAGE_TYPE_SPIFFS) {
#ifdef STORAGE_SPIFFS_AVAILABLE
        ret = esp_vfs_spiffs_unregister(partition_label.c_str()) == ESP_OK;
#endif
    } else {
#ifdef STORAGE_LITTLEFS_AVAILABLE
        ret = esp_vfs_littlefs_unregister(partition_label.c_str()) == ESP_OK;
#endif
    }
    
    if (ret) {
#if STORAGE_ENABLE_DEBUG_LOGGING
        ESP_LOGI(TAG, "%s unmounted successfully", get_storage_type_name());
#endif
        is_mounted = false;
    } else {
        ESP_LOGE(TAG, "Failed to unmount %s", get_storage_type_name());
    }
    
    return ret;
}

bool storage_esp::format() {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(storage_mutex);
#endif

    if (!is_mounted) {
        ESP_LOGE(TAG, "Storage not mounted, cannot format");
        return false;
    }
    
    bool ret = false;
    
    if (storage_type == STORAGE_TYPE_SPIFFS) {
#ifdef STORAGE_SPIFFS_AVAILABLE
        ret = esp_spiffs_format(partition_label.c_str()) == ESP_OK;
#endif
    } else {
#ifdef STORAGE_LITTLEFS_AVAILABLE
        ret = esp_littlefs_format(partition_label.c_str()) == ESP_OK;
#endif
    }
    
    if (ret) {
#if STORAGE_ENABLE_DEBUG_LOGGING
        ESP_LOGI(TAG, "%s formatted successfully", get_storage_type_name());
#endif
    } else {
        ESP_LOGE(TAG, "Failed to format %s", get_storage_type_name());
    }
    
    return ret;
}

bool storage_esp::exists(const std::string& key) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(storage_mutex);
#endif

    if (!is_mounted) {
        return false;
    }
    
    std::string full_path = get_full_path(key);
    struct stat st;
    return stat(full_path.c_str(), &st) == 0;
}

size_t storage_esp::file_size(const std::string& key) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(storage_mutex);
#endif

    if (!is_mounted) {
        return 0;
    }
    
    std::string full_path = get_full_path(key);
    struct stat st;
    if (stat(full_path.c_str(), &st) == 0) {
        return st.st_size;
    }
    return 0;
}

bool storage_esp::read_file(const std::string& key, void* data, size_t data_size) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(storage_mutex);
#endif

    if (!is_mounted || !data) {
        return false;
    }
    
    std::string full_path = get_full_path(key);
    
    FILE* f = fopen(full_path.c_str(), "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", full_path.c_str());
        return false;
    }
    
    size_t bytes_read = fread(data, 1, data_size, f);
    fclose(f);
    
    if (bytes_read != data_size) {
        ESP_LOGE(TAG, "Read size mismatch: expected %d, got %d", data_size, bytes_read);
        return false;
    }
    
#if STORAGE_ENABLE_DEBUG_LOGGING
    ESP_LOGD(TAG, "Read %d bytes from %s", bytes_read, key.c_str());
#endif
    return true;
}

bool storage_esp::write_file(const std::string& key, const void* data, size_t data_size) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(storage_mutex);
#endif

    if (!is_mounted || !data) {
        return false;
    }
    
    std::string full_path = get_full_path(key);
    
    // Create parent directories if needed
    size_t last_slash = full_path.rfind('/');
    if (last_slash != std::string::npos && last_slash > base_path.length()) {
        std::string dir_path = full_path.substr(0, last_slash);
        create_directory_recursive(dir_path);
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

bool storage_esp::erase_file(const std::string& key) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(storage_mutex);
#endif

    if (!is_mounted) {
        return false;
    }
    
    std::string full_path = get_full_path(key);
    
    if (unlink(full_path.c_str()) == 0) {
#if STORAGE_ENABLE_DEBUG_LOGGING
        ESP_LOGD(TAG, "Deleted file: %s", key.c_str());
#endif
        return true;
    }
    
    ESP_LOGE(TAG, "Failed to delete file: %s", full_path.c_str());
    return false;
}

size_t storage_esp::total_size() {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(storage_mutex);
#endif

    if (!is_mounted) {
        return 0;
    }
    
    size_t total = 0, used = 0;
    
    if (storage_type == STORAGE_TYPE_SPIFFS) {
#ifdef STORAGE_SPIFFS_AVAILABLE
        esp_spiffs_info(partition_label.c_str(), &total, &used);
#endif
    } else {
#ifdef STORAGE_LITTLEFS_AVAILABLE
        esp_littlefs_info(partition_label.c_str(), &total, &used);
#endif
    }
    
    return total;
}

size_t storage_esp::used_size() {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(storage_mutex);
#endif

    if (!is_mounted) {
        return 0;
    }
    
    size_t total = 0, used = 0;
    
    if (storage_type == STORAGE_TYPE_SPIFFS) {
#ifdef STORAGE_SPIFFS_AVAILABLE
        esp_spiffs_info(partition_label.c_str(), &total, &used);
#endif
    } else {
#ifdef STORAGE_LITTLEFS_AVAILABLE
        esp_littlefs_info(partition_label.c_str(), &total, &used);
#endif
    }
    
    return used;
}

bool storage_esp::create_directory_recursive(const std::string& path) {
    // Skip if path is just the base path
    if (path == base_path || path.length() <= base_path.length()) {
        return true;
    }
    
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    
    // Create parent directory first
    size_t last_slash = path.rfind('/');
    if (last_slash != std::string::npos && last_slash > base_path.length()) {
        std::string parent = path.substr(0, last_slash);
        if (!create_directory_recursive(parent)) {
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

bool storage_esp::list_directory(const std::string& path, std::vector<file_info_t>& files) {
    if (!is_mounted) {
        return false;
    }
    
    std::string full_path = get_full_path(path);
    
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

// Advanced file operations
bool storage_esp::read_file_alloc(const std::string& key, uint8_t** data, size_t* size) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(storage_mutex);
#endif

    if (!is_mounted || !data || !size) {
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

    if (!read_file(key, *data, *size)) {
        free(*data);
        *data = nullptr;
        *size = 0;
        return false;
    }

    return true;
}

bool storage_esp::rename_file(const std::string& old_key, const std::string& new_key) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(storage_mutex);
#endif

    if (!is_mounted) {
        return false;
    }

    std::string old_path = get_full_path(old_key);
    std::string new_path = get_full_path(new_key);

    if (rename(old_path.c_str(), new_path.c_str()) == 0) {
#if STORAGE_ENABLE_DEBUG_LOGGING
        ESP_LOGD(TAG, "Renamed file: %s -> %s", old_key.c_str(), new_key.c_str());
#endif
        return true;
    }

    ESP_LOGE(TAG, "Failed to rename file: %s -> %s", old_key.c_str(), new_key.c_str());
    return false;
}

// Directory operations
bool storage_esp::create_directory(const std::string& path) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(storage_mutex);
#endif

    if (!is_mounted) {
        return false;
    }

    std::string full_path = get_full_path(path);
    return create_directory_recursive(full_path);
}

bool storage_esp::verify_file_integrity(const std::string& key, size_t expected_size, uint32_t* checksum) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(storage_mutex);
#endif

    if (!is_mounted) {
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

// ========== File Versioning Public Methods ==========

#if STORAGE_ENABLE_VERSIONING
uint32_t storage_esp::get_file_version(const std::string& key)
{
    mutex_guard guard(storage_mutex);
    
    if (!is_mounted || !exists(key)) {
        return 0;
    }
    
    file_version_metadata metadata;
    if (!_load_metadata(key, metadata)) {
        return 0;
    }
    
    return metadata.current_version;
}

bool storage_esp::get_file_version_info(const std::string& key, file_version_info& info)
{
    mutex_guard guard(storage_mutex);
    
    if (!is_mounted || !exists(key)) {
        return false;
    }
    
    file_version_metadata metadata;
    if (!_load_metadata(key, metadata)) {
        return false;
    }
    
    info.version = metadata.current_version;
    info.size = metadata.file_size;
    info.is_current = true;
    
    return true;
}

std::vector<file_version_info> storage_esp::list_file_versions(const std::string& key)
{
    std::vector<file_version_info> versions;
    mutex_guard guard(storage_mutex);
    
    if (!is_mounted || !exists(key)) {
        return versions;
    }
    
    file_version_metadata metadata;
    if (!_load_metadata(key, metadata)) {
        return versions;
    }
    
    // Add current version
    file_version_info current;
    current.version = metadata.current_version;
    current.size = metadata.file_size;
    current.is_current = true;
    versions.push_back(current);
    
    // Add historical versions
    for (uint32_t i = 0; i < metadata.version_count; i++) {
        uint32_t version_num = metadata.versions[i];
        if (version_num == 0) continue;
        
        std::string version_path = _get_version_path(key, version_num);
        struct stat st;
        if (stat(version_path.c_str(), &st) == 0) {
            file_version_info historical;
            historical.version = version_num;
            historical.size = st.st_size;
            historical.is_current = false;
            versions.push_back(historical);
        }
    }
    
    // Sort by version number (newest first)
    std::sort(versions.begin(), versions.end(), 
              [](const file_version_info& a, const file_version_info& b) {
                  return a.version > b.version;
              });
    
    return versions;
}

bool storage_esp::read_file_version(const std::string& key, uint32_t version, void* data, size_t dataSize)
{
    mutex_guard guard(storage_mutex);
    
    if (!is_mounted) {
        ESP_LOGE(TAG, "Filesystem not mounted");
        return false;
    }
    
    std::string file_path;
    if (version == 0) {
        // Read current version
        file_path = get_full_path(key);
    } else {
        // Read specific version
        file_path = _get_version_path(key, version);
    }
    
    FILE* file = fopen(file_path.c_str(), "rb");
    if (!file) {
        ESP_LOGW(TAG, "Failed to open file version %d for reading: %s", version, file_path.c_str());
        return false;
    }
    
    size_t bytes_read = fread(data, 1, dataSize, file);
    fclose(file);
    
    if (bytes_read != dataSize) {
        ESP_LOGW(TAG, "Read %zu bytes from %s version %d, expected %zu bytes", 
                 bytes_read, key.c_str(), version, dataSize);
        return false;
    }
    
    ESP_LOGD(TAG, "Successfully read %zu bytes from %s version %d", bytes_read, key.c_str(), version);
    return true;
}

bool storage_esp::file_has_changed(const std::string& key, uint32_t last_known_version)
{
    mutex_guard guard(storage_mutex);
    
    if (!is_mounted || !exists(key)) {
        return false;
    }
    
    file_version_metadata metadata;
    if (!_load_metadata(key, metadata)) {
        return false;
    }
    
    return metadata.current_version > last_known_version;
}

bool storage_esp::restore_file_version(const std::string& key, uint32_t version)
{
    mutex_guard guard(storage_mutex);
    
    if (!is_mounted) {
        ESP_LOGE(TAG, "Filesystem not mounted");
        return false;
    }
    
    std::string version_path = _get_version_path(key, version);
    std::string current_path = get_full_path(key);
    
    // Read the version data
    struct stat st;
    if (stat(version_path.c_str(), &st) != 0) {
        ESP_LOGE(TAG, "Version %d of %s does not exist", version, key.c_str());
        return false;
    }
    
    std::vector<uint8_t> version_data(st.st_size);
    FILE* version_file = fopen(version_path.c_str(), "rb");
    if (!version_file) {
        ESP_LOGE(TAG, "Failed to open version file: %s", version_path.c_str());
        return false;
    }
    
    size_t bytes_read = fread(version_data.data(), 1, st.st_size, version_file);
    fclose(version_file);
    
    if (bytes_read != (size_t)st.st_size) {
        ESP_LOGE(TAG, "Failed to read version data completely");
        return false;
    }
    
    // Write as current version (this will create a new version automatically)
    bool result = write_file(key, version_data.data(), version_data.size());
    
    if (result) {
        ESP_LOGI(TAG, "Successfully restored %s to version %d", key.c_str(), version);
    }
    
    return result;
}

uint32_t storage_esp::cleanup_old_versions(const std::string& key)
{
    mutex_guard guard(storage_mutex);
    uint32_t cleaned_count = 0;
    
    if (!is_mounted) {
        ESP_LOGE(TAG, "Filesystem not mounted");
        return 0;
    }
    
    if (key.empty()) {
        // Clean up all files - this would be a more complex implementation
        ESP_LOGW(TAG, "Global cleanup not implemented yet");
        return 0;
    }
    
    file_version_metadata metadata;
    if (!_load_metadata(key, metadata)) {
        return 0;
    }
    
    // Remove versions beyond the limit
    while (metadata.version_count > STORAGE_MAX_VERSION_HISTORY) {
        if (_cleanup_oldest_version(key, metadata)) {
            cleaned_count++;
        } else {
            break;
        }
    }
    
    if (cleaned_count > 0) {
        _save_metadata(key, metadata);
        ESP_LOGI(TAG, "Cleaned up %d old versions of %s", cleaned_count, key.c_str());
    }
    
    return cleaned_count;
}

// ========== Private Helper Methods ==========

std::string storage_esp::_get_metadata_path(const std::string& key) const
{
    return get_full_path(key) + STORAGE_VERSION_METADATA_EXT;
}

std::string storage_esp::_get_version_path(const std::string& key, uint32_t version) const
{
    char version_suffix[32];
    snprintf(version_suffix, sizeof(version_suffix), ".v%d", version);
    return get_full_path(key) + version_suffix;
}

bool storage_esp::_load_metadata(const std::string& key, file_version_metadata& metadata)
{
    std::string meta_path = _get_metadata_path(key);
    
    FILE* meta_file = fopen(meta_path.c_str(), "rb");
    if (!meta_file) {
        // No metadata file exists - initialize with defaults
        metadata = file_version_metadata();
        return true;
    }
    
    size_t bytes_read = fread(&metadata, sizeof(file_version_metadata), 1, meta_file);
    fclose(meta_file);
    
    if (bytes_read != 1) {
        ESP_LOGW(TAG, "Failed to read metadata for %s, using defaults", key.c_str());
        metadata = file_version_metadata();
        return true;
    }
    
    return true;
}

bool storage_esp::_save_metadata(const std::string& key, const file_version_metadata& metadata)
{
    std::string meta_path = _get_metadata_path(key);
    
    FILE* meta_file = fopen(meta_path.c_str(), "wb");
    if (!meta_file) {
        ESP_LOGE(TAG, "Failed to open metadata file for writing: %s", meta_path.c_str());
        return false;
    }
    
    size_t bytes_written = fwrite(&metadata, sizeof(file_version_metadata), 1, meta_file);
    fclose(meta_file);
    
    return bytes_written == 1;
}

uint32_t storage_esp::_calculate_crc32(const void* data, size_t length) const
{
    // Simple CRC32 implementation
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFF;
    
    // CRC32 polynomial (IEEE 802.3)
    const uint32_t polynomial = 0xEDB88320;
    
    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ polynomial;
            } else {
                crc = crc >> 1;
            }
        }
    }
    
    return crc ^ 0xFFFFFFFF;
}

bool storage_esp::_archive_current_version(const std::string& key, file_version_metadata& metadata)
{
    std::string current_path = get_full_path(key);
    std::string archive_path = _get_version_path(key, metadata.current_version);
    
    // Read current file
    struct stat st;
    if (stat(current_path.c_str(), &st) != 0) {
        return false;
    }
    
    std::vector<uint8_t> file_data(st.st_size);
    FILE* current_file = fopen(current_path.c_str(), "rb");
    if (!current_file) {
        return false;
    }
    
    size_t bytes_read = fread(file_data.data(), 1, st.st_size, current_file);
    fclose(current_file);
    
    if (bytes_read != (size_t)st.st_size) {
        return false;
    }
    
    // Write to archive
    FILE* archive_file = fopen(archive_path.c_str(), "wb");
    if (!archive_file) {
        return false;
    }
    
    size_t bytes_written = fwrite(file_data.data(), 1, file_data.size(), archive_file);
    fclose(archive_file);
    
    if (bytes_written != file_data.size()) {
        unlink(archive_path.c_str()); // Clean up failed archive
        return false;
    }
    
    // Add to version list if not already there
    bool version_exists = false;
    for (uint32_t i = 0; i < metadata.version_count; i++) {
        if (metadata.versions[i] == metadata.current_version) {
            version_exists = true;
            break;
        }
    }
    
    if (!version_exists && metadata.version_count < STORAGE_MAX_VERSION_HISTORY) {
        metadata.versions[metadata.version_count] = metadata.current_version;
        metadata.version_count++;
    } else if (!version_exists) {
        // Need to make room - remove oldest version
        _cleanup_oldest_version(key, metadata);
        metadata.versions[metadata.version_count] = metadata.current_version;
        metadata.version_count++;
    }
    
    ESP_LOGD(TAG, "Archived version %d of %s", metadata.current_version, key.c_str());
    return true;
}

bool storage_esp::_cleanup_oldest_version(const std::string& key, file_version_metadata& metadata)
{
    if (metadata.version_count == 0) {
        return false;
    }
    
    // Find oldest version
    uint32_t oldest_version = metadata.versions[0];
    uint32_t oldest_index = 0;
    
    for (uint32_t i = 1; i < metadata.version_count; i++) {
        if (metadata.versions[i] < oldest_version && metadata.versions[i] != 0) {
            oldest_version = metadata.versions[i];
            oldest_index = i;
        }
    }
    
    // Delete the oldest version file
    std::string oldest_path = _get_version_path(key, oldest_version);
    if (unlink(oldest_path.c_str()) == 0) {
        ESP_LOGD(TAG, "Deleted old version %d of %s", oldest_version, key.c_str());
        
        // Remove from version list by shifting array
        for (uint32_t i = oldest_index; i < metadata.version_count - 1; i++) {
            metadata.versions[i] = metadata.versions[i + 1];
        }
        metadata.versions[metadata.version_count - 1] = 0;
        metadata.version_count--;
        
        return true;
    }
    
    return false;
}

#endif // STORAGE_ENABLE_VERSIONING