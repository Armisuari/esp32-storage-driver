#pragma once

/**
 * @file storage_config.h
 * @brief Storage filesystem configuration for ESP32 platform
 * 
 * This file defines the available filesystem options, default configurations,
 * and all configurable parameters for the storage driver.
 */

// Filesystem availability flags
#define STORAGE_SPIFFS_AVAILABLE
#define STORAGE_LITTLEFS_AVAILABLE

// Default filesystem selection
#ifdef STORAGE_LITTLEFS_AVAILABLE
    #define STORAGE_DEFAULT_TYPE STORAGE_TYPE_LITTLEFS
    #define STORAGE_DEFAULT_PARTITION_LABEL "spiffs"
    #define STORAGE_DEFAULT_BASE_PATH "/littlefs"
#elif defined(STORAGE_SPIFFS_AVAILABLE)
    #define STORAGE_DEFAULT_TYPE STORAGE_TYPE_SPIFFS
    #define STORAGE_DEFAULT_PARTITION_LABEL "spiffs"
    #define STORAGE_DEFAULT_BASE_PATH "/spiffs"
#else
    #error "No storage filesystem is available. Please define at least one filesystem type."
#endif

// Mount configuration
#define STORAGE_FORMAT_IF_MOUNT_FAILS true
#define STORAGE_MAX_FILES 10

// Default mount points for different filesystems
#define STORAGE_SPIFFS_BASE_PATH "/spiffs"
#define STORAGE_LITTLEFS_BASE_PATH "/littlefs"

// Directory permissions
#define STORAGE_DIR_PERMISSIONS 0755

// File versioning configuration
#define STORAGE_ENABLE_VERSIONING true  // Disabled by default for now
#define STORAGE_MAX_VERSION_HISTORY 5    // Keep last N versions of each file
#define STORAGE_VERSION_METADATA_EXT ".meta"  // Extension for metadata files

// Thread safety configuration
#define STORAGE_ENABLE_MUTEX_PROTECTION true
#define STORAGE_MUTEX_TIMEOUT_MS portMAX_DELAY

// Logging configuration
#define STORAGE_ENABLE_DEBUG_LOGGING true