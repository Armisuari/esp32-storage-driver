#include "storage_esp.h"
#include "log.h"
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

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
    
    // Ensure directory exists
    if (!_ensure_directory_exists(full_path)) {
        ESP_LOGE(TAG, "Failed to create directory for file: %s", full_path.c_str());
        return false;
    }

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

bool storage_esp::exists(const std::string& key)
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
    
    struct stat st;
    bool file_exists = (stat(full_path.c_str(), &st) == 0) && S_ISREG(st.st_mode);
    
    ESP_LOGD(TAG, "File %s exists: %s", key.c_str(), file_exists ? "true" : "false");
    return file_exists;
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
