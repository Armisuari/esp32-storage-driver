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