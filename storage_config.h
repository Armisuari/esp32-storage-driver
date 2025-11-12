#pragma once

/**
 * @file storage_config.h
 * @brief Storage filesystem configuration for ESP32 platform
 * 
 * This file defines the available filesystem options and selects the default
 * filesystem to use for storage operations.
 */

// Available filesystem types
enum class storage_filesystem_t {
    SPIFFS,
    LITTLEFS
};

// Filesystem availability flags
#define STORAGE_SPIFFS_AVAILABLE
#define STORAGE_LITTLEFS_AVAILABLE

// Default filesystem selection
#ifdef STORAGE_LITTLEFS_AVAILABLE
    #define STORAGE_DEFAULT_TYPE storage_filesystem_t::LITTLEFS
    #define STORAGE_DEFAULT_PARTITION_LABEL "spiffs"
#elif defined(STORAGE_SPIFFS_AVAILABLE)
    #define STORAGE_DEFAULT_TYPE storage_filesystem_t::SPIFFS
    #define STORAGE_DEFAULT_PARTITION_LABEL "spiffs"
#else
    #error "No storage filesystem is available. Please define at least one filesystem type."
#endif

// Storage configuration
#define STORAGE_FORMAT_IF_MOUNT_FAILS true
#define STORAGE_MAX_FILES 10

// File versioning configuration
#define STORAGE_ENABLE_VERSIONING true
#define STORAGE_MAX_VERSION_HISTORY 5  // Keep last N versions of each file
#define STORAGE_VERSION_METADATA_EXT ".meta"  // Extension for metadata files