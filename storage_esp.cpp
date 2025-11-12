#include "storage_esp.h"
#include "log.h"
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#if STORAGE_ENABLE_VERSIONING
#include <vector>
#include <algorithm>
#include <ctime>
#endif

static const char* TAG = "storage_esp";

storage_esp::storage_esp(storage_filesystem_t filesystem_type, 
                        const char* partition_label,
                        const char* base_path) 
    : _fs_type(filesystem_type)
    , _partition_label(partition_label)
    , _base_path(base_path)
    , _is_mounted(false)
    , _mutex(nullptr)
{
    // Create FreeRTOS mutex
    _mutex = xSemaphoreCreateMutex();
    if (_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create mutex for storage");
    }

    ESP_LOGD(TAG, "Storage ESP instance created with filesystem: %s, partition: %s, base_path: %s",
             (_fs_type == storage_filesystem_t::LITTLEFS) ? "LittleFS" : "SPIFFS",
             _partition_label, _base_path);
}

storage_esp::~storage_esp()
{
    if (_is_mounted) {
        _deinit_filesystem();
    }
    
    // Delete FreeRTOS mutex
    if (_mutex != nullptr) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
    
    ESP_LOGD(TAG, "Storage ESP instance destroyed");
}

bool storage_esp::begin()
{
    mutex_guard lock(_mutex);
    
    if (_is_mounted) {
        ESP_LOGW(TAG, "Filesystem already mounted");
        return true;
    }

    esp_err_t ret = ESP_FAIL;
    
    switch (_fs_type) {
#ifdef STORAGE_LITTLEFS_SUPPORTED
        case storage_filesystem_t::LITTLEFS:
            ret = _init_littlefs();
            break;
#endif
#ifdef STORAGE_SPIFFS_SUPPORTED
        case storage_filesystem_t::SPIFFS:
            ret = _init_spiffs();
            break;
#endif
        default:
            ESP_LOGE(TAG, "Unsupported filesystem type");
            return false;
    }

    if (ret == ESP_OK) {
        _is_mounted = true;
        ESP_LOGI(TAG, "Filesystem mounted successfully at %s", _base_path);
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to mount filesystem: %s", esp_err_to_name(ret));
        return false;
    }
}

bool storage_esp::read_file(const std::string& key, void* data, size_t dataSize)
{
    mutex_guard lock(_mutex);
    
    if (!_is_mounted) {
        ESP_LOGE(TAG, "Filesystem not mounted");
        return false;
    }

    if (!_is_valid_path(key)) {
        ESP_LOGE(TAG, "Invalid file path: %s", key.c_str());
        return false;
    }

    std::string full_path = get_full_path(key);
    
    FILE* file = fopen(full_path.c_str(), "rb");
    if (!file) {
        ESP_LOGW(TAG, "Failed to open file for reading: %s (%s)", full_path.c_str(), strerror(errno));
        return false;
    }

    size_t bytes_read = fread(data, 1, dataSize, file);
    fclose(file);

    if (bytes_read != dataSize) {
        ESP_LOGW(TAG, "Read %zu bytes from %s, expected %zu bytes", bytes_read, key.c_str(), dataSize);
        return false;
    }

    ESP_LOGD(TAG, "Successfully read %zu bytes from %s", bytes_read, key.c_str());
    return true;
}

bool storage_esp::write_file(const std::string& key, const void* data, size_t dataSize)
{
    ESP_LOGI(TAG, "write_file called for: %s", key.c_str());
    
    mutex_guard lock(_mutex);
    ESP_LOGI(TAG, "Mutex acquired");
    
    if (!_is_mounted) {
        ESP_LOGE(TAG, "Filesystem not mounted");
        return false;
    }

    if (!_is_valid_path(key)) {
        ESP_LOGE(TAG, "Invalid file path: %s", key.c_str());
        return false;
    }

    std::string full_path = get_full_path(key);
    ESP_LOGI(TAG, "Full path: %s", full_path.c_str());
    
    // Ensure directory exists
    ESP_LOGI(TAG, "Ensuring directory exists...");
    if (!_ensure_directory_exists(full_path)) {
        ESP_LOGE(TAG, "Failed to create directory for file: %s", full_path.c_str());
        return false;
    }

#if STORAGE_ENABLE_VERSIONING
    ESP_LOGI(TAG, "Versioning enabled, loading metadata...");
    // Load existing metadata (if any)
    file_version_metadata metadata;
    _load_metadata(key, metadata);
    ESP_LOGI(TAG, "Metadata loaded");
    
    // Archive current version if file exists
    ESP_LOGI(TAG, "Checking if file exists...");
    if (_exists_internal(key)) {
        ESP_LOGI(TAG, "File exists, archiving current version...");
        if (!_archive_current_version(key, metadata)) {
            ESP_LOGW(TAG, "Failed to archive current version of %s", key.c_str());
        }
        ESP_LOGI(TAG, "Archiving completed");
    } else {
        ESP_LOGI(TAG, "File does not exist, no archiving needed");
    }
    
    // Update metadata for new version
    metadata.current_version++;
    metadata.timestamp = _get_timestamp();
    metadata.file_size = dataSize;
    metadata.checksum = _calculate_crc32(data, dataSize);
    
    ESP_LOGD(TAG, "Writing file %s version %d (size: %zu, crc: 0x%08x)", 
             key.c_str(), metadata.current_version, dataSize, metadata.checksum);
#endif

    // Write the file
    FILE* file = fopen(full_path.c_str(), "wb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s (%s)", full_path.c_str(), strerror(errno));
        return false;
    }

    size_t bytes_written = fwrite(data, 1, dataSize, file);
    fclose(file);

    if (bytes_written != dataSize) {
        ESP_LOGE(TAG, "Wrote %zu bytes to %s, expected %zu bytes", bytes_written, key.c_str(), dataSize);
        return false;
    }

#if STORAGE_ENABLE_VERSIONING
    // Save updated metadata
    if (!_save_metadata(key, metadata)) {
        ESP_LOGW(TAG, "Failed to save metadata for %s", key.c_str());
    }
#endif

    ESP_LOGD(TAG, "Successfully wrote %zu bytes to %s", bytes_written, key.c_str());
    return true;
}

bool storage_esp::erase_file(const std::string& key)
{
    mutex_guard lock(_mutex);
    
    if (!_is_mounted) {
        ESP_LOGE(TAG, "Filesystem not mounted");
        return false;
    }

    if (!_is_valid_path(key)) {
        ESP_LOGE(TAG, "Invalid file path: %s", key.c_str());
        return false;
    }

    std::string full_path = get_full_path(key);
    
    if (unlink(full_path.c_str()) != 0) {
        if (errno == ENOENT) {
            ESP_LOGW(TAG, "File does not exist: %s", key.c_str());
        } else {
            ESP_LOGE(TAG, "Failed to delete file %s: %s", key.c_str(), strerror(errno));
        }
        return false;
    }

    ESP_LOGD(TAG, "Successfully deleted file: %s", key.c_str());
    return true;
}

size_t storage_esp::file_size(const std::string& key)
{
    mutex_guard lock(_mutex);
    
    if (!_is_mounted) {
        ESP_LOGE(TAG, "Filesystem not mounted");
        return 0;
    }

    if (!_is_valid_path(key)) {
        ESP_LOGE(TAG, "Invalid file path: %s", key.c_str());
        return 0;
    }

    std::string full_path = get_full_path(key);
    
    struct stat st;
    if (stat(full_path.c_str(), &st) != 0) {
        ESP_LOGD(TAG, "File does not exist or stat failed: %s", key.c_str());
        return 0;
    }

    ESP_LOGD(TAG, "File %s size: %ld bytes", key.c_str(), st.st_size);
    return st.st_size;
}

bool storage_esp::_exists_internal(const std::string& key)
{
    if (!_is_mounted) {
        ESP_LOGE(TAG, "Filesystem not mounted");
        return false;
    }

    if (!_is_valid_path(key)) {
        ESP_LOGE(TAG, "Invalid file path: %s", key.c_str());
        return false;
    }

    std::string full_path = get_full_path(key);
    
    struct stat st;
    bool file_exists = (stat(full_path.c_str(), &st) == 0) && S_ISREG(st.st_mode);
    
    ESP_LOGD(TAG, "File %s exists: %s", key.c_str(), file_exists ? "true" : "false");
    return file_exists;
}

bool storage_esp::exists(const std::string& key)
{
    mutex_guard lock(_mutex);
    return _exists_internal(key);
}

size_t storage_esp::total_size() const
{
    mutex_guard lock(_mutex);
    
    if (!_is_mounted) {
        ESP_LOGE(TAG, "Filesystem not mounted");
        return 0;
    }

    size_t total = 0, used = 0;
    esp_err_t ret = ESP_FAIL;

    switch (_fs_type) {
#ifdef STORAGE_LITTLEFS_SUPPORTED
        case storage_filesystem_t::LITTLEFS:
            ret = esp_littlefs_info(_partition_label, &total, &used);
            break;
#endif
#ifdef STORAGE_SPIFFS_SUPPORTED
        case storage_filesystem_t::SPIFFS:
            ret = esp_spiffs_info(_partition_label, &total, &used);
            break;
#endif
        default:
            ESP_LOGE(TAG, "Unsupported filesystem type for size query");
            return 0;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get filesystem info: %s", esp_err_to_name(ret));
        return 0;
    }

    return total;
}

size_t storage_esp::used_size() const
{
    mutex_guard lock(_mutex);
    
    if (!_is_mounted) {
        ESP_LOGE(TAG, "Filesystem not mounted");
        return 0;
    }

    size_t total = 0, used = 0;
    esp_err_t ret = ESP_FAIL;

    switch (_fs_type) {
#ifdef STORAGE_LITTLEFS_SUPPORTED
        case storage_filesystem_t::LITTLEFS:
            ret = esp_littlefs_info(_partition_label, &total, &used);
            break;
#endif
#ifdef STORAGE_SPIFFS_SUPPORTED
        case storage_filesystem_t::SPIFFS:
            ret = esp_spiffs_info(_partition_label, &total, &used);
            break;
#endif
        default:
            ESP_LOGE(TAG, "Unsupported filesystem type for size query");
            return 0;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get filesystem info: %s", esp_err_to_name(ret));
        return 0;
    }

    return used;
}

bool storage_esp::format()
{
    mutex_guard lock(_mutex);
    
    if (!_is_mounted) {
        ESP_LOGE(TAG, "Filesystem not mounted - cannot format");
        return false;
    }

    ESP_LOGW(TAG, "Formatting filesystem - THIS WILL ERASE ALL DATA!");
    
    esp_err_t ret = ESP_FAIL;

    switch (_fs_type) {
#ifdef STORAGE_LITTLEFS_SUPPORTED
        case storage_filesystem_t::LITTLEFS:
            ret = esp_littlefs_format(_partition_label);
            break;
#endif
#ifdef STORAGE_SPIFFS_SUPPORTED
        case storage_filesystem_t::SPIFFS:
            ret = esp_spiffs_format(_partition_label);
            break;
#endif
        default:
            ESP_LOGE(TAG, "Unsupported filesystem type for format");
            return false;
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Filesystem formatted successfully");
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to format filesystem: %s", esp_err_to_name(ret));
        return false;
    }
}

std::string storage_esp::get_full_path(const std::string& key) const
{
    std::string full_path = _base_path;
    
    // Ensure base path ends with slash
    if (full_path.back() != '/') {
        full_path += '/';
    }
    
    // Remove leading slash from key if present
    std::string clean_key = key;
    if (!clean_key.empty() && clean_key[0] == '/') {
        clean_key = clean_key.substr(1);
    }
    
    full_path += clean_key;
    return full_path;
}

#ifdef STORAGE_LITTLEFS_SUPPORTED
esp_err_t storage_esp::_init_littlefs()
{
    ESP_LOGI(TAG, "Initializing LittleFS on partition '%s'", _partition_label);
    
    // Configure LittleFS
    memset(&_fs_conf.littlefs_conf, 0, sizeof(esp_vfs_littlefs_conf_t));
    _fs_conf.littlefs_conf.base_path = _base_path;
    _fs_conf.littlefs_conf.partition_label = _partition_label;
    _fs_conf.littlefs_conf.format_if_mount_failed = STORAGE_FORMAT_IF_MOUNT_FAILS;
    _fs_conf.littlefs_conf.dont_mount = false;

    esp_err_t ret = esp_vfs_littlefs_register(&_fs_conf.littlefs_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LittleFS: %s", esp_err_to_name(ret));
    }
    
    return ret;
}
#endif

#ifdef STORAGE_SPIFFS_SUPPORTED
esp_err_t storage_esp::_init_spiffs()
{
    ESP_LOGI(TAG, "Initializing SPIFFS on partition '%s'", _partition_label);
    
    // Configure SPIFFS
    memset(&_fs_conf.spiffs_conf, 0, sizeof(esp_vfs_spiffs_conf_t));
    _fs_conf.spiffs_conf.base_path = _base_path;
    _fs_conf.spiffs_conf.partition_label = _partition_label;
    _fs_conf.spiffs_conf.max_files = STORAGE_MAX_FILES;
    _fs_conf.spiffs_conf.format_if_mount_failed = STORAGE_FORMAT_IF_MOUNT_FAILS;

    esp_err_t ret = esp_vfs_spiffs_register(&_fs_conf.spiffs_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPIFFS: %s", esp_err_to_name(ret));
    }
    
    return ret;
}
#endif

void storage_esp::_deinit_filesystem()
{
    if (!_is_mounted) {
        return;
    }

    esp_err_t ret = ESP_FAIL;
    
    switch (_fs_type) {
#ifdef STORAGE_LITTLEFS_SUPPORTED
        case storage_filesystem_t::LITTLEFS:
            ret = esp_vfs_littlefs_unregister(_partition_label);
            break;
#endif
#ifdef STORAGE_SPIFFS_SUPPORTED
        case storage_filesystem_t::SPIFFS:
            ret = esp_vfs_spiffs_unregister(_partition_label);
            break;
#endif
        default:
            ESP_LOGE(TAG, "Unsupported filesystem type for unmount");
            return;
    }

    if (ret == ESP_OK) {
        _is_mounted = false;
        ESP_LOGI(TAG, "Filesystem unmounted successfully");
    } else {
        ESP_LOGE(TAG, "Failed to unmount filesystem: %s", esp_err_to_name(ret));
    }
}

bool storage_esp::_is_valid_path(const std::string& key) const
{
    // Check for empty path
    if (key.empty()) {
        return false;
    }

    // Check for invalid characters (basic validation)
    if (key.find('\0') != std::string::npos) {
        return false;
    }

    // Check for relative path components that could escape the filesystem
    if (key.find("..") != std::string::npos) {
        return false;
    }

    // Path length check (reasonable limit)
    if (key.length() > 255) {
        return false;
    }

    return true;
}

bool storage_esp::_ensure_directory_exists(const std::string& filepath) const
{
    // SPIFFS doesn't support directories - all files are in a flat namespace
    // File paths with '/' are treated as part of the filename, not directory structure
    if (_fs_type == storage_filesystem_t::SPIFFS) {
        return true;  // No directory creation needed for SPIFFS
    }
    
    size_t last_slash = filepath.find_last_of('/');
    if (last_slash == std::string::npos) {
        // No directory component
        return true;
    }

    std::string dir_path = filepath.substr(0, last_slash);
    
    struct stat st;
    if (stat(dir_path.c_str(), &st) == 0) {
        // Path exists, check if it's a directory
        return S_ISDIR(st.st_mode);
    }

    // Directory doesn't exist, try to create it
    // Note: This is a simple implementation. For production, you might want
    // to create parent directories recursively
    if (mkdir(dir_path.c_str(), 0755) == 0) {
        ESP_LOGD(TAG, "Created directory: %s", dir_path.c_str());
        return true;
    } else if (errno == EEXIST) {
        // Directory was created by another thread/process
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to create directory %s: %s", dir_path.c_str(), strerror(errno));
        return false;
    }
}

#if STORAGE_ENABLE_VERSIONING

// ========== File Versioning Public Methods ==========

uint32_t storage_esp::get_file_version(const std::string& key)
{
    mutex_guard lock(_mutex);
    
    if (!_is_mounted || !_exists_internal(key)) {
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
    mutex_guard lock(_mutex);
    
    if (!_is_mounted || !_exists_internal(key)) {
        return false;
    }
    
    file_version_metadata metadata;
    if (!_load_metadata(key, metadata)) {
        return false;
    }
    
    info.version = metadata.current_version;
    info.timestamp = metadata.timestamp;
    info.size = metadata.file_size;
    info.is_current = true;
    
    return true;
}

std::vector<file_version_info> storage_esp::list_file_versions(const std::string& key)
{
    std::vector<file_version_info> versions;
    mutex_guard lock(_mutex);
    
    if (!_is_mounted || !_exists_internal(key)) {
        return versions;
    }
    
    file_version_metadata metadata;
    if (!_load_metadata(key, metadata)) {
        return versions;
    }
    
    // Add current version
    file_version_info current;
    current.version = metadata.current_version;
    current.timestamp = metadata.timestamp;
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
            historical.timestamp = st.st_mtime;
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
    mutex_guard lock(_mutex);
    
    if (!_is_mounted) {
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
    mutex_guard lock(_mutex);
    
    if (!_is_mounted || !_exists_internal(key)) {
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
    mutex_guard lock(_mutex);
    
    if (!_is_mounted) {
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
    mutex_guard lock(_mutex);
    uint32_t cleaned_count = 0;
    
    if (!_is_mounted) {
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

uint32_t storage_esp::_get_timestamp() const
{
    return (uint32_t)time(nullptr);
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
