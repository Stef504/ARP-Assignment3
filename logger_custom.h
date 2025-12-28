// logger_custom.h
#ifndef LOGGER_CUSTOM_H
#define LOGGER_CUSTOM_H

#include <stdio.h>
#include <errno.h>
#include <string.h>

// Log levels
typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR,
    LOG_CRITICAL
} LogLevel;

// Function prototypes
int logger_init(const char* log_file_path);
void logger_close(void);
void logger_log(LogLevel level, const char* process_name, const char* file, 
                int line, const char* function, const char* format, ...);

// Macros to auto-fill file/line/function details
#define LOG_INFO(process, ...) \
    logger_log(LOG_INFO, process, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define LOG_WARNING(process, ...) \
    logger_log(LOG_WARNING, process, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define LOG_ERROR(process, ...) \
    logger_log(LOG_ERROR, process, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define LOG_CRITICAL(process, ...) \
    logger_log(LOG_CRITICAL, process, __FILE__, __LINE__, __func__, __VA_ARGS__)

// Special macro for System Errors (like "File not found")
#define LOG_ERRNO(process, msg) \
    logger_log(LOG_ERROR, process, __FILE__, __LINE__, __func__, \
               "%s: %s", msg, strerror(errno))

#endif