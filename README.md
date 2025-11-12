# ESP32 Storage Driver Usage Guide

This guide shows how to use the refactored ESP32 storage driver with native ESP-IDF APIs.

## Features

- **Native ESP-IDF APIs**: No Arduino framework dependencies
- **Multiple Filesystem Support**: LittleFS and SPIFFS
- **Built-in Thread Safety**: FreeRTOS mutex protection for all operations
- **Simple Configuration**: Easy filesystem selection through configuration
- **Comprehensive Error Handling**: Detailed logging and error reporting
- **Automatic Resource Management**: Proper cleanup and RAII principles

## Basic Usage

### Creating and Using Storage

```cpp
#include "storage_esp.h"

// Create storage instance with default configuration (LittleFS)
storage_esp storage;

// Or specify filesystem type and partition
storage_esp storage(storage_filesystem_t::LITTLEFS, "storage", "/data");

// Initialize the filesystem
if (!storage.begin()) {
    ESP_LOGE("app", "Failed to mount filesystem");
    return;
}

// Use the storage
std::string data = "Hello, World!";
if (storage.write_file("test.txt", data.c_str(), data.length())) {
    ESP_LOGI("app", "File written successfully");
}
```

### Different Storage Configurations

```cpp
#include "storage_esp.h"

// Default configuration (uses STORAGE_DEFAULT_TYPE from config)
storage_esp default_storage;

// Specific LittleFS configuration
storage_esp littlefs_storage(
    storage_filesystem_t::LITTLEFS,
    "storage",           // partition label
    "/data"              // mount point
);

// Specific SPIFFS configuration (if enabled)
storage_esp spiffs_storage(
    storage_filesystem_t::SPIFFS,
    "spiffs_partition",  // partition label
    "/spiffs"            // mount point
);
```

## File Operations

### Writing Files

```cpp
storage_esp storage;
storage.begin();

std::string config_data = "{\"version\":1,\"enabled\":true}";
if (!storage.write_file("config.json", config_data.c_str(), config_data.length())) {
    ESP_LOGE("app", "Failed to write config file");
}
```

### Reading Files

```cpp
// First, get the file size
size_t file_size = storage.file_size("config.json");
if (file_size > 0) {
    std::vector<char> buffer(file_size + 1);  // +1 for null terminator
    if (storage.read_file("config.json", buffer.data(), file_size)) {
        buffer[file_size] = '\0';  // Null terminate
        ESP_LOGI("app", "Config content: %s", buffer.data());
    }
}
```

### Checking File Existence

```cpp
if (storage.exists("config.json")) {
    ESP_LOGI("app", "Config file exists");
} else {
    ESP_LOGI("app", "Config file does not exist");
}
```

### Deleting Files

```cpp
if (storage.erase_file("old_data.txt")) {
    ESP_LOGI("app", "File deleted successfully");
}
```

## Filesystem Information

```cpp
storage_esp storage;
storage.begin();

ESP_LOGI("app", "Total space: %zu bytes", storage.total_size());
ESP_LOGI("app", "Used space: %zu bytes", storage.used_size());
ESP_LOGI("app", "Free space: %zu bytes", 
         storage.total_size() - storage.used_size());
```

## Configuration

### storage_config.h

```cpp
// Available filesystem types
enum class storage_filesystem_t {
    SPIFFS,
    LITTLEFS
};

// Enable the filesystems you want to use
#define STORAGE_LITTLEFS_AVAILABLE
// #define STORAGE_SPIFFS_AVAILABLE

// Configuration
#define STORAGE_DEFAULT_TYPE storage_filesystem_t::LITTLEFS
#define STORAGE_DEFAULT_PARTITION_LABEL "storage"
#define STORAGE_FORMAT_IF_MOUNT_FAILS true
#define STORAGE_MAX_FILES 10
```

## Partition Table Requirements

Make sure your partition table includes a storage partition. Example `partitions.csv`:

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 1M,
storage,  data, spiffs,  0x110000, 1M,
```

For LittleFS, you can use either `spiffs` or `littlefs` as SubType in ESP-IDF 4.4+.

## Error Handling

The storage driver provides comprehensive error handling:

```cpp
storage_esp storage;

if (!storage.begin()) {
    ESP_LOGE("app", "Storage mount failed - check partition table and configuration");
    return ESP_FAIL;
}

// All file operations return bool for success/failure
if (!storage.write_file("data.bin", data, size)) {
    ESP_LOGE("app", "Write failed - check disk space and permissions");
}
```

## Migration from Arduino Framework

If migrating from Arduino SPIFFS/LittleFS libraries:

### Before (Arduino)
```cpp
#include <SPIFFS.h>

SPIFFS.begin();
File file = SPIFFS.open("/config.txt", "w");
file.write(data, size);
file.close();
```

### After (ESP-IDF)
```cpp
#include "storage_esp.h"

storage_esp storage;
storage.begin();
storage.write_file("config.txt", data, size);
```

## Thread Safety

The storage driver provides **built-in thread safety** using FreeRTOS mutexes. All public methods are automatically protected, so you can safely use the storage from multiple tasks without additional synchronization:

```cpp
// Task 1
void config_task(void* pvParameters) {
    storage_esp* storage = (storage_esp*)pvParameters;
    
    while(1) {
        std::string config = get_current_config();
        storage->write_file("config.json", config.c_str(), config.length());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// Task 2
void log_task(void* pvParameters) {
    storage_esp* storage = (storage_esp*)pvParameters;
    
    while(1) {
        std::string log_entry = get_log_entry();
        storage->write_file("system.log", log_entry.c_str(), log_entry.length());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Main application
void app_main() {
    storage_esp storage;
    storage.begin();
    
    // Both tasks can safely use the same storage instance
    xTaskCreate(config_task, "config", 4096, &storage, 5, NULL);
    xTaskCreate(log_task, "log", 4096, &storage, 5, NULL);
}
```

### Thread Safety Implementation

- **FreeRTOS Mutexes**: Uses `xSemaphoreCreateMutex()` for efficient blocking
- **RAII Lock Guard**: Automatic mutex acquisition/release using custom `mutex_guard` class
- **Atomic Operations**: Each public method is fully protected
- **Deadlock Prevention**: Mutexes are always acquired in the same order

### Performance Considerations

- **Minimal Overhead**: Mutex operations are very fast on ESP32
- **No Busy Waiting**: Tasks block efficiently when waiting for mutex
- **File Operations**: The actual I/O operations are the bottleneck, not the mutex
- **Configurable Timeout**: Uses `portMAX_DELAY` for reliability (can be customized if needed)

## Build Configuration

Add these to your component's `CMakeLists.txt` or `component.mk`:

```cmake
# For ESP-IDF CMake
idf_component_register(
    SRCS "storage_esp.cpp" "storage_factory.cpp"
    INCLUDE_DIRS "."
    REQUIRES "esp_littlefs"  # or "spiffs" for SPIFFS
)
```

Enable the filesystem in `sdkconfig`:
```
CONFIG_LITTLEFS_PAGE_SIZE=256
CONFIG_LITTLEFS_OBJ_NAME_LEN=64
```

## File Versioning Features

### Overview

The enhanced storage driver now includes comprehensive file versioning capabilities:

- **Automatic Version Tracking**: Every file write automatically increments the version number
- **Version History**: Configurable number of previous versions kept (default: 5)
- **Metadata Storage**: Each file has associated metadata with version, timestamp, size, and checksum
- **Change Detection**: Easy detection of file modifications since last known version
- **Version Restoration**: Ability to restore files to previous versions
- **Thread-Safe Operations**: All versioning operations are protected by the same mutex system

### Configuration

Enable versioning in `storage_config.h`:

```cpp
// File versioning configuration
#define STORAGE_ENABLE_VERSIONING true
#define STORAGE_MAX_VERSION_HISTORY 5  // Keep last N versions of each file
#define STORAGE_VERSION_METADATA_EXT ".meta"  // Extension for metadata files
```

### Basic Versioning Usage

```cpp
storage_esp storage;
storage.begin();

// Write initial version (automatically becomes version 1)
std::string config_v1 = R"({"timeout": 5000})";
storage.write_file("config.json", config_v1.c_str(), config_v1.length());

// Write updated version (automatically becomes version 2)
std::string config_v2 = R"({"timeout": 3000, "retries": 3})";
storage.write_file("config.json", config_v2.c_str(), config_v2.length());

// Check current version
uint32_t current_version = storage.get_file_version("config.json");
ESP_LOGI("app", "Current version: %d", current_version); // Prints: 2

// Check if file has changed since version 1
if (storage.file_has_changed("config.json", 1)) {
    ESP_LOGI("app", "File has been modified!");
}
```

### Version Information and History

```cpp
// Get detailed version information
file_version_info info;
if (storage.get_file_version_info("config.json", info)) {
    ESP_LOGI("app", "Version %d: %zu bytes, timestamp: %d", 
             info.version, info.size, info.timestamp);
}

// List all available versions
auto versions = storage.list_file_versions("config.json");
for (const auto& ver : versions) {
    ESP_LOGI("app", "Version %d: %zu bytes, current: %s", 
             ver.version, ver.size, ver.is_current ? "yes" : "no");
}
```

### Reading Specific Versions

```cpp
// Read current version (same as normal read_file)
char current_data[256];
storage.read_file("config.json", current_data, sizeof(current_data));

// Read a specific version
char version_1_data[256];
storage.read_file_version("config.json", 1, version_1_data, sizeof(version_1_data));
```

### Version Restoration

```cpp
// Restore file to a previous version
if (storage.restore_file_version("config.json", 1)) {
    ESP_LOGI("app", "Successfully restored to version 1");
    // This creates a new version with the content from version 1
}
```

### Change Detection for IoT Applications

```cpp
// Typical IoT scenario: check for configuration updates
class ConfigManager {
private:
    storage_esp* storage;
    uint32_t last_known_config_version = 0;

public:
    void check_for_config_updates() {
        if (storage->file_has_changed("device_config.json", last_known_config_version)) {
            ESP_LOGI("config", "Configuration updated, reloading...");
            load_config();
            last_known_config_version = storage->get_file_version("device_config.json");
        }
    }
    
    void load_config() {
        size_t config_size = storage->file_size("device_config.json");
        std::vector<char> config_data(config_size);
        if (storage->read_file("device_config.json", config_data.data(), config_size)) {
            // Parse and apply configuration
            parse_json_config(config_data.data());
        }
    }
};
```

### Maintenance and Cleanup

```cpp
// Clean up old versions beyond the configured limit
uint32_t cleaned = storage.cleanup_old_versions("large_log_file.txt");
ESP_LOGI("app", "Cleaned up %d old versions", cleaned);

// Clean up all files (if key is empty)
storage.cleanup_old_versions("");
```

### File Structure

With versioning enabled, the filesystem structure looks like:

```
/storage/
├── config.json           # Current version
├── config.json.meta      # Metadata (version info, checksums, etc.)
├── config.json.v1        # Version 1 backup
├── config.json.v2        # Version 2 backup
├── sensor_data.csv       # Current version
├── sensor_data.csv.meta  # Metadata
└── sensor_data.csv.v1    # Version 1 backup
```

### Performance Considerations

- **Write Performance**: Each write operation now involves metadata updates and potentially archiving the current version
- **Storage Usage**: Each versioned file uses approximately `(history_count + 1) * average_file_size` storage
- **Memory Usage**: Metadata structures are small (~100 bytes per file)
- **Thread Safety**: All versioning operations are atomic and thread-safe

### Migration from Non-Versioned Files

Existing files without version metadata will automatically start versioning from version 1 when first written to with the enhanced driver.

### Error Handling

```cpp
// Version-related operations have specific error handling
uint32_t version = storage.get_file_version("nonexistent.txt");
if (version == 0) {
    ESP_LOGW("app", "File doesn't exist or has no version info");
}

// Check if specific version exists before reading
auto versions = storage.list_file_versions("config.json");
bool version_3_exists = std::any_of(versions.begin(), versions.end(),
                                   [](const auto& v) { return v.version == 3; });
if (version_3_exists) {
    // Safe to read version 3
    storage.read_file_version("config.json", 3, buffer, size);
}
```