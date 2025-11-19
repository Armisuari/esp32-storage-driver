#include "file_versioning.h"
#include "esp_log.h"
#include <algorithm>
#include <sys/stat.h>

static const char* TAG = "file_versioning";

file_versioning::file_versioning(const storage_callbacks& callbacks)
    : storage_ops(callbacks)
#if STORAGE_ENABLE_MUTEX_PROTECTION
    , versioning_mutex(nullptr)
#endif
{
#if STORAGE_ENABLE_MUTEX_PROTECTION
    versioning_mutex = xSemaphoreCreateMutex();
    if (versioning_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create versioning mutex");
    }
#endif

#if STORAGE_ENABLE_DEBUG_LOGGING
    ESP_LOGI(TAG, "File versioning initialized");
#endif
}

file_versioning::~file_versioning() {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    if (versioning_mutex != nullptr) {
        vSemaphoreDelete(versioning_mutex);
    }
#endif
}

// ========== Public Version Query Methods ==========

uint32_t file_versioning::get_file_version(const std::string& key) {
    // Note: This method should only be called from within a mutex-protected context
    // or from thread-safe contexts, so no additional mutex guard is needed here
    
    if (!storage_ops.is_mounted() || !storage_ops.file_exists(key)) {
        return 0;
    }
    
    file_version_metadata metadata;
    if (!load_metadata(key, metadata)) {
        return 0;
    }
    
    return metadata.current_version;
}

bool file_versioning::get_file_version_info(const std::string& key, file_version_info& info) {
    // Note: This method should only be called from within a mutex-protected context
    // or from thread-safe contexts, so no additional mutex guard is needed here
    
    if (!storage_ops.is_mounted() || !storage_ops.file_exists(key)) {
        return false;
    }
    
    file_version_metadata metadata;
    if (!load_metadata(key, metadata)) {
        return false;
    }
    
    info.version = metadata.current_version;
    info.size = metadata.file_size;
    info.is_current = true;
    
    return true;
}

std::vector<file_version_info> file_versioning::list_file_versions(const std::string& key) {
    std::vector<file_version_info> versions;
    
    // Note: This method should only be called from within a mutex-protected context
    // or from thread-safe contexts, so no additional mutex guard is needed here
    
    if (!storage_ops.is_mounted() || !storage_ops.file_exists(key)) {
        return versions;
    }
    
    file_version_metadata metadata;
    if (!load_metadata(key, metadata)) {
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
        
        std::string version_path = get_version_path(key, version_num);
        size_t version_size = storage_ops.get_file_size(version_path);
        
        if (version_size > 0) {
            file_version_info historical;
            historical.version = version_num;
            historical.size = version_size;
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

// ========== Public Version Read/Restore Methods ==========

bool file_versioning::read_file_version(const std::string& key, uint32_t version, 
                                       void* data, size_t data_size) {
    // Note: This method should only be called from within a mutex-protected context
    // or from thread-safe contexts, so no additional mutex guard is needed here
    
    if (!storage_ops.is_mounted()) {
        ESP_LOGE(TAG, "Storage not mounted");
        return false;
    }
    
    std::string file_path;
    if (version == 0) {
        // Read current version
        file_path = key;
    } else {
        // Read specific version
        file_path = get_version_path(key, version);
    }
    
    if (!storage_ops.read_file(file_path, data, data_size)) {
        ESP_LOGW(TAG, "Failed to read file version %d: %s", version, key.c_str());
        return false;
    }
    
#if STORAGE_ENABLE_DEBUG_LOGGING
    ESP_LOGD(TAG, "Successfully read %zu bytes from %s version %d", 
             data_size, key.c_str(), version);
#endif
    return true;
}

bool file_versioning::restore_file_version(const std::string& key, uint32_t version) {
    // Note: This method should only be called from within a mutex-protected context
    // or from thread-safe contexts, so no additional mutex guard is needed here
    
    if (!storage_ops.is_mounted()) {
        ESP_LOGE(TAG, "Storage not mounted");
        return false;
    }
    
    std::string version_path = get_version_path(key, version);
    
    // Check if version exists
    size_t version_size = storage_ops.get_file_size(version_path);
    if (version_size == 0) {
        ESP_LOGE(TAG, "Version %d of %s does not exist", version, key.c_str());
        return false;
    }
    
    // Read the version data
    std::vector<uint8_t> version_data(version_size);
    if (!storage_ops.read_file(version_path, version_data.data(), version_size)) {
        ESP_LOGE(TAG, "Failed to read version file: %s", version_path.c_str());
        return false;
    }
    
    // Write as current version (this will create a new version automatically via on_before_write)
    bool result = storage_ops.write_file(key, version_data.data(), version_data.size());
    
    if (result) {
        ESP_LOGI(TAG, "Successfully restored %s to version %d", key.c_str(), version);
    }
    
    return result;
}

// ========== Public Version Management Methods ==========

bool file_versioning::archive_current_version(const std::string& key) {
    // Note: This method should only be called from within a mutex-protected context
    // (e.g., from on_before_write), so no additional mutex guard is needed here
    
    if (!storage_ops.is_mounted() || !storage_ops.file_exists(key)) {
        return false;
    }
    
    file_version_metadata metadata;
    load_metadata(key, metadata);
    
    std::string current_path = key;
    std::string archive_path = get_version_path(key, metadata.current_version);
    
    // Read current file
    size_t file_size = storage_ops.get_file_size(current_path);
    if (file_size == 0) {
        return false;
    }
    
    std::vector<uint8_t> file_data(file_size);
    if (!storage_ops.read_file(current_path, file_data.data(), file_size)) {
        return false;
    }
    
    // Write to archive
    if (!storage_ops.write_file(archive_path, file_data.data(), file_data.size())) {
        ESP_LOGE(TAG, "Failed to write archive: %s", archive_path.c_str());
        return false;
    }
    
    // Update version list
    bool version_exists = false;
    for (uint32_t i = 0; i < metadata.version_count; i++) {
        if (metadata.versions[i] == metadata.current_version) {
            version_exists = true;
            break;
        }
    }
    
    if (!version_exists) {
        if (metadata.version_count < STORAGE_MAX_VERSION_HISTORY) {
            metadata.versions[metadata.version_count] = metadata.current_version;
            metadata.version_count++;
        } else {
            // Need to make room - remove oldest version
            cleanup_oldest_version(key, metadata);
            metadata.versions[metadata.version_count - 1] = metadata.current_version;
        }
        save_metadata(key, metadata);
    }
    
#if STORAGE_ENABLE_DEBUG_LOGGING
    ESP_LOGD(TAG, "Archived version %d of %s", metadata.current_version, key.c_str());
#endif
    return true;
}

bool file_versioning::file_has_changed(const std::string& key, uint32_t last_known_version) {
    // Note: This method should only be called from within a mutex-protected context
    // or from thread-safe contexts, so no additional mutex guard is needed here
    
    if (!storage_ops.is_mounted() || !storage_ops.file_exists(key)) {
        return false;
    }
    
    file_version_metadata metadata;
    if (!load_metadata(key, metadata)) {
        return false;
    }
    
    return metadata.current_version > last_known_version;
}

uint32_t file_versioning::cleanup_old_versions(const std::string& key) {
    // Note: This method should only be called from within a mutex-protected context
    // (e.g., from erase_file), so no additional mutex guard is needed here
    
    uint32_t cleaned_count = 0;
    
    if (!storage_ops.is_mounted() || key.empty()) {
        return 0;
    }
    
    file_version_metadata metadata;
    if (!load_metadata(key, metadata)) {
        return 0;
    }
    
    // Remove versions beyond the limit
    while (metadata.version_count > STORAGE_MAX_VERSION_HISTORY) {
        if (cleanup_oldest_version(key, metadata)) {
            cleaned_count++;
        } else {
            break;
        }
    }
    
    if (cleaned_count > 0) {
        save_metadata(key, metadata);
#if STORAGE_ENABLE_DEBUG_LOGGING
        ESP_LOGI(TAG, "Cleaned up %d old versions of %s", cleaned_count, key.c_str());
#endif
    }
    
    return cleaned_count;
}

bool file_versioning::on_before_write(const std::string& key, const void* data, size_t size) {
#if STORAGE_ENABLE_MUTEX_PROTECTION
    mutex_guard guard(versioning_mutex);
#endif
    
    if (!storage_ops.is_mounted()) {
        return true; // Allow write to proceed
    }
    
    file_version_metadata metadata;
    load_metadata(key, metadata);
    
    // Archive current version if file exists
    if (storage_ops.file_exists(key)) {
        archive_current_version(key);
    }
    
    // Update metadata for new version
    metadata.current_version++;
    metadata.file_size = size;
    metadata.checksum = calculate_crc32(data, size);
    
    save_metadata(key, metadata);
    
    return true;
}

// ========== Private Helper Methods ==========

std::string file_versioning::get_metadata_path(const std::string& key) const {
    return storage_ops.get_full_path(key) + STORAGE_VERSION_METADATA_EXT;
}

std::string file_versioning::get_version_path(const std::string& key, uint32_t version) const {
    char version_suffix[32];
    snprintf(version_suffix, sizeof(version_suffix), ".v%d", version);
    return storage_ops.get_full_path(key) + version_suffix;
}

bool file_versioning::load_metadata(const std::string& key, file_version_metadata& metadata) {
    std::string meta_path = get_metadata_path(key);
    
    size_t meta_size = storage_ops.get_file_size(meta_path);
    if (meta_size == 0 || meta_size != sizeof(file_version_metadata)) {
        // No metadata file exists - initialize with defaults
        metadata = file_version_metadata();
        return true;
    }
    
    if (!storage_ops.read_file(meta_path, &metadata, sizeof(file_version_metadata))) {
        ESP_LOGW(TAG, "Failed to read metadata for %s, using defaults", key.c_str());
        metadata = file_version_metadata();
        return true;
    }
    
    return true;
}

bool file_versioning::save_metadata(const std::string& key, const file_version_metadata& metadata) {
    std::string meta_path = get_metadata_path(key);
    
    if (!storage_ops.write_file(meta_path, &metadata, sizeof(file_version_metadata))) {
        ESP_LOGE(TAG, "Failed to save metadata for %s", key.c_str());
        return false;
    }
    
    return true;
}

uint32_t file_versioning::calculate_crc32(const void* data, size_t length) const {
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

bool file_versioning::cleanup_oldest_version(const std::string& key, 
                                             file_version_metadata& metadata) {
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
    std::string oldest_path = get_version_path(key, oldest_version);
    if (storage_ops.delete_file(oldest_path)) {
#if STORAGE_ENABLE_DEBUG_LOGGING
        ESP_LOGD(TAG, "Deleted old version %d of %s", oldest_version, key.c_str());
#endif
        
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