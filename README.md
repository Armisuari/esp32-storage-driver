# ESP32 Storage Driver Usage Guide

This guide shows how to use the enhanced ESP32 storage driver with native ESP-IDF APIs, full configuration support, and thread safety.

## Features

- **Native ESP-IDF APIs**: No Arduino framework dependencies
- **Multiple Filesystem Support**: LittleFS and SPIFFS (configurable)
- **Built-in Thread Safety**: FreeRTOS mutex protection for all operations (configurable)
- **Centralized Configuration**: All settings managed through `storage_config.h`
- **Comprehensive Error Handling**: Detailed logging and error reporting
- **Automatic Resource Management**: Proper cleanup and RAII principles
- **Advanced File Operations**: File allocation, renaming, integrity verification
- **Directory Management**: Recursive directory creation and listing

## Configuration

All storage driver settings are centralized in `storage_config.h`:

```cpp
// Filesystem availability (enable/disable filesystems)
#define STORAGE_SPIFFS_AVAILABLE
#define STORAGE_LITTLEFS_AVAILABLE

// Default settings
#define STORAGE_DEFAULT_TYPE STORAGE_TYPE_LITTLEFS
#define STORAGE_DEFAULT_PARTITION_LABEL "spiffs"
#define STORAGE_DEFAULT_BASE_PATH "/littlefs"

// Mount configuration
#define STORAGE_FORMAT_IF_MOUNT_FAILS false
#define STORAGE_MAX_FILES 10

// Base paths for different filesystems
#define STORAGE_SPIFFS_BASE_PATH "/spiffs"
#define STORAGE_LITTLEFS_BASE_PATH "/littlefs"

// Thread safety (can be disabled for single-threaded applications)
#define STORAGE_ENABLE_MUTEX_PROTECTION true
#define STORAGE_MUTEX_TIMEOUT_MS portMAX_DELAY

// Logging control
#define STORAGE_ENABLE_DEBUG_LOGGING true

// Directory permissions
#define STORAGE_DIR_PERMISSIONS 0755
```

## Basic Usage

### Creating Storage Instances

```cpp
#include "storage_esp.h"

// Default configuration (uses settings from storage_config.h)
storage_esp storage;

// Specific filesystem with default partition and mount point
storage_esp littlefs_storage(STORAGE_TYPE_LITTLEFS);

// Custom partition label
storage_esp custom_storage(STORAGE_TYPE_LITTLEFS, "my_partition");

// Full custom configuration
storage_esp full_custom(STORAGE_TYPE_SPIFFS, "spiffs_part", "/custom");
```

### Initializing and Using Storage

```cpp
// Initialize the filesystem
if (!storage.begin()) {
    ESP_LOGE("app", "Failed to mount filesystem");
    return;
}

// Check if mounted
if (!storage.get_is_mounted()) {
    ESP_LOGE("app", "Storage not ready");
    return;
}

// Basic file operations
std::string data = "Hello, World!";
if (storage.write_file("test.txt", data.c_str(), data.length())) {
    ESP_LOGI("app", "File written successfully");
    
    // Read it back
    char buffer[256];
    if (storage.read_file("test.txt", buffer, sizeof(buffer))) {
        ESP_LOGI("app", "File content: %s", buffer);
    }
}
```

## File Operations

### Writing Files with Directory Creation

```cpp
storage_esp storage;
storage.begin();

// Automatically creates nested directories
std::string config_data = "{\"version\":1,\"enabled\":true}";
if (!storage.write_file("config/device/settings.json", config_data.c_str(), config_data.length())) {
    ESP_LOGE("app", "Failed to write config file");
}
```

### Dynamic Memory Allocation for File Reading

```cpp
// Automatically allocates exact amount needed
uint8_t* file_data = nullptr;
size_t file_size = 0;

if (storage.read_file_alloc("large_data.bin", &file_data, &file_size)) {
    ESP_LOGI("app", "Read %d bytes from file", file_size);
    
    // Process the data...
    
    // Don't forget to free the allocated memory
    free(file_data);
}
```

### File Management Operations

```cpp
// Check if file exists
if (storage.exists("config.json")) {
    // Get file size
    size_t size = storage.file_size("config.json");
    ESP_LOGI("app", "Config file size: %d bytes", size);
    
    // Rename file
    if (storage.rename_file("config.json", "config_backup.json")) {
        ESP_LOGI("app", "File renamed successfully");
    }
    
    // Verify file integrity
    uint32_t expected_checksum = 0x12345678;
    if (storage.verify_file_integrity("config_backup.json", size, &expected_checksum)) {
        ESP_LOGI("app", "File integrity verified");
    }
    
    // Delete file
    storage.erase_file("config_backup.json");
}
```

## Directory Operations

```cpp
storage_esp storage;
storage.begin();

// Create directory structure
if (storage.create_directory("logs/2024/november")) {
    ESP_LOGI("app", "Directory structure created");
}

// List directory contents
std::vector<file_info_t> files;
if (storage.list_directory("logs", files)) {
    for (const auto& file : files) {
        ESP_LOGI("app", "%s: %s (%d bytes)", 
                 file.path.c_str(), 
                 file.is_directory ? "DIR" : "FILE",
                 file.size);
    }
}

// List all files in filesystem
std::vector<file_info_t> all_files;
if (storage.list_all_files(all_files)) {
    ESP_LOGI("app", "Total files: %d", all_files.size());
}
```

## Filesystem Information and Management

```cpp
storage_esp storage;
storage.begin();

// Get filesystem statistics
ESP_LOGI("app", "Total space: %zu bytes", storage.total_size());
ESP_LOGI("app", "Used space: %zu bytes", storage.used_size());
ESP_LOGI("app", "Free space: %zu bytes", 
         storage.total_size() - storage.used_size());

// Get storage configuration
ESP_LOGI("app", "Storage type: %s", 
         storage.get_storage_type() == STORAGE_TYPE_LITTLEFS ? "LittleFS" : "SPIFFS");
ESP_LOGI("app", "Base path: %s", storage.get_base_path().c_str());
ESP_LOGI("app", "Partition: %s", storage.get_partition_label().c_str());
```

## Advanced Mount Operations

```cpp
storage_esp storage(STORAGE_TYPE_LITTLEFS, "storage");

// Mount with specific options
if (!storage.mount(true)) {  // format_on_fail = true
    ESP_LOGE("app", "Failed to mount even with formatting");
    return;
}

// Manually format if needed
if (/* some condition */) {
    ESP_LOGW("app", "Formatting filesystem...");
    if (!storage.format()) {
        ESP_LOGE("app", "Format failed");
    }
}

// Proper cleanup
storage.unmount();
```

## Thread Safety

The storage driver provides **built-in thread safety** when `STORAGE_ENABLE_MUTEX_PROTECTION` is enabled in `storage_config.h`. All public methods are automatically protected:

```cpp
// Safe to use from multiple tasks simultaneously
storage_esp shared_storage;
shared_storage.begin();

// Task 1: Configuration updates
void config_task(void* pvParameters) {
    storage_esp* storage = (storage_esp*)pvParameters;
    
    while(1) {
        std::string config = get_current_config();
        storage->write_file("config.json", config.c_str(), config.length());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// Task 2: Data logging  
void log_task(void* pvParameters) {
    storage_esp* storage = (storage_esp*)pvParameters;
    
    while(1) {
        std::string log_entry = generate_log_entry();
        storage->write_file("system.log", log_entry.c_str(), log_entry.length());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Main application
void app_main() {
    // Both tasks can safely use the same storage instance
    xTaskCreate(config_task, "config", 4096, &shared_storage, 5, NULL);
    xTaskCreate(log_task, "log", 4096, &shared_storage, 5, NULL);
}
```

### Thread Safety Configuration

- **Enable/Disable**: Set `STORAGE_ENABLE_MUTEX_PROTECTION` in `storage_config.h`
- **Mutex Timeout**: Configure `STORAGE_MUTEX_TIMEOUT_MS` (default: `portMAX_DELAY`)
- **Performance**: Minimal overhead, file I/O is the bottleneck
- **RAII Protection**: Automatic mutex management using custom `mutex_guard` class

## Error Handling and Debugging

```cpp
storage_esp storage;

// Enable detailed logging via configuration
// #define STORAGE_ENABLE_DEBUG_LOGGING true

if (!storage.begin()) {
    ESP_LOGE("app", "Storage mount failed - check partition table and configuration");
    
    // Try different mount options
    if (!storage.mount(true)) {  // Allow formatting
        ESP_LOGE("app", "Storage completely failed - hardware issue?");
        return ESP_FAIL;
    }
}

// All file operations return bool for success/failure
if (!storage.write_file("critical_data.bin", data, size)) {
    ESP_LOGE("app", "Critical write failed - check disk space: %d free", 
             storage.total_size() - storage.used_size());
}
```

## Partition Table Configuration

Ensure your `partitions.csv` includes appropriate storage partitions:

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 1M,
storage,  data, spiffs,  0x110000, 1M,
```

For LittleFS, you can use either `spiffs` or `littlefs` as SubType (ESP-IDF 4.4+).

## Build Configuration

### CMakeLists.txt

```cmake
idf_component_register(
    SRCS "storage_esp.cpp" 
    INCLUDE_DIRS "." "../interface"
    REQUIRES "esp_littlefs" "spiffs"  # Include both for flexibility
)
```

### sdkconfig

```
# LittleFS Configuration
CONFIG_LITTLEFS_PAGE_SIZE=256
CONFIG_LITTLEFS_OBJ_NAME_LEN=64

# SPIFFS Configuration  
CONFIG_SPIFFS_PAGE_SIZE=256
CONFIG_SPIFFS_OBJ_NAME_LEN=64
```

## Migration from Previous Version

### Constructor Changes

```cpp
// Old way
storage_esp storage(STORAGE_TYPE_LITTLEFS, "partition");

// New way (same, but with more options)
storage_esp storage;  // Uses defaults from config
// OR
storage_esp storage(STORAGE_TYPE_LITTLEFS, "partition");  // Explicit
// OR  
storage_esp storage(STORAGE_TYPE_LITTLEFS, "partition", "/custom_mount");  // Full control
```

### Configuration Migration

Move hardcoded values to `storage_config.h`:

```cpp
// Old: hardcoded in source files
#define STORAGE_MAX_FILES 10

// New: in storage_config.h  
#define STORAGE_MAX_FILES 10
#define STORAGE_FORMAT_IF_MOUNT_FAILS false
#define STORAGE_DEFAULT_TYPE STORAGE_TYPE_LITTLEFS
```

## Performance Tips

1. **Minimize File Operations**: Batch writes when possible
2. **Use Appropriate Buffer Sizes**: Read/write in reasonable chunks
3. **Enable Debug Logging Conditionally**: Disable in production for better performance
4. **Consider Thread Safety Needs**: Disable mutex protection for single-threaded applications
5. **Choose Right Filesystem**: LittleFS generally better for power-loss protection

## Troubleshooting

### Common Issues

1. **Mount Failures**: Check partition table, partition label, and filesystem availability flags
2. **Thread Deadlocks**: Ensure `STORAGE_MUTEX_TIMEOUT_MS` is appropriate for your use case
3. **Memory Issues**: Use `read_file_alloc()` for dynamic allocation, remember to `free()` result
4. **Permission Errors**: Verify `STORAGE_DIR_PERMISSIONS` setting for directory creation

### Debug Steps

1. Enable debug logging: `#define STORAGE_ENABLE_DEBUG_LOGGING true`
2. Check filesystem info after mount: `total_size()` and `used_size()`
3. Verify partition exists: Use ESP-IDF partition tools
4. Test with formatting enabled: `mount(true)`