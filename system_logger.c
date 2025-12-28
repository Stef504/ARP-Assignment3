#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/file.h>
#include "logger_custom.h"

// Store the log file PATH instead of FILE pointer
static char log_file_path[256] = {0};

static const char* log_level_to_string(LogLevel level) {
    switch(level) {
        case LOG_DEBUG:    return "DEBUG";
        case LOG_INFO:     return "INFO";
        case LOG_WARNING:  return "WARNING";
        case LOG_ERROR:    return "ERROR";
        case LOG_CRITICAL: return "CRITICAL";
        default:           return "UNKNOWN";
    }
}

int logger_init(const char* path) {
    // Just store the path - don't open the file yet
    strncpy(log_file_path, path, sizeof(log_file_path) - 1);
    log_file_path[sizeof(log_file_path) - 1] = '\0';
    
    // Test that we CAN open it
    FILE* test = fopen(log_file_path, "a");
    if (test == NULL) {
        perror("Failed to open log file");
        return -1;
    }
    fclose(test);
    
    // Reset the log file at start
    FILE* reset = fopen(log_file_path, "w");
    if (reset) fclose(reset);
    
    return 0;
}

void logger_close(void) {
    // Nothing to do - we open/close per log
    log_file_path[0] = '\0';
}

void logger_log(LogLevel level, const char* process_name, const char* file, 
                int line, const char* function, const char* format, ...) {
    
    if (log_file_path[0] == '\0') {
        return;  // Logger not initialized
    }
    
    // OPEN the file each time (just like your log_watchdog)
    FILE* log_file = fopen(log_file_path, "a");
    if (log_file == NULL) {
        return;
    }
    
    // LOCK: Acquire exclusive lock
    int fd = fileno(log_file);
    if (flock(fd, LOCK_EX) == -1) {
        fclose(log_file);
        return;
    }
    
    // Get current time
    time_t now;
    time(&now);
    char time_str[26];
    struct tm* tm_info = localtime(&now);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    
    // Get process ID
    pid_t pid = getpid();
    
    // WRITE: Write log header
    fprintf(log_file, "[%s] [PID:%d] [%s] [%s] %s:%d (%s) - ",
            time_str, pid, log_level_to_string(level), 
            process_name, file, line, function);
    
    // WRITE: Write the actual log message
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    
    fprintf(log_file, "\n");
    
    // FLUSH: Force write to disk immediately
    fflush(log_file);
    
    // UNLOCK: Release the lock
    flock(fd, LOCK_UN);
    
    // CLOSE: Close the file
    fclose(log_file);
}