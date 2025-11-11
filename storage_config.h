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
// #define STORAGE_SPIFFS_AVAILABLE
#define STORAGE_LITTLEFS_AVAILABLE

// Default filesystem selection
#ifdef STORAGE_LITTLEFS_AVAILABLE
    #define STORAGE_DEFAULT_TYPE storage_filesystem_t::LITTLEFS
    #define STORAGE_DEFAULT_PARTITION_LABEL "storage"
#elif defined(STORAGE_SPIFFS_AVAILABLE)
    #define STORAGE_DEFAULT_TYPE storage_filesystem_t::SPIFFS
    #define STORAGE_DEFAULT_PARTITION_LABEL "storage"
#else
    #error "No storage filesystem is available. Please define at least one filesystem type."
#endif

// Storage configuration
#define STORAGE_FORMAT_IF_MOUNT_FAILS true
#define STORAGE_MAX_FILES 10