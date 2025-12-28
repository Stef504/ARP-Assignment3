#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

// Function to log process name and PID to a file with file locking
void log_process(const char *process_name, int pid) {
    // 1. OPEN FILE (Append Mode)
    FILE *f = fopen("process_log.log", "a");
    if (f == NULL) {
        perror("Failed to open log file");
        return;
    }

    // 2. LOCK FILE (Exclusive Lock)
    // LOCK_EX = Exclusive Lock (Write Access)
    // This call blocks until the lock is acquired.
    if (flock(fileno(f), LOCK_EX) == -1) {
        perror("Failed to lock log file");
        fclose(f);
        return;
    }

    // 3. WRITE
    fprintf(f, "Process: %s | PID: %d\n", process_name, pid);

    // 4. FLUSH
    // Force write to disk immediately
    fflush(f);

    // 5. RELEASE LOCK & CLOSE
    // LOCK_UN = Unlock
    flock(fileno(f), LOCK_UN);
    fclose(f);
}

// Read the log file to find a PID by name
//this is used by the process to send a signal to the watchdog
pid_t get_pid_by_name(const char *process_name) {
    FILE *f = fopen("process_log.log", "r");
    if (f == NULL) return -1;
    
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char name[100];
        int pid;
        if (sscanf(line, "Process: %s | PID: %d", name, &pid) == 2) {
            if (strcmp(name, process_name) == 0) {
                fclose(f);
                return pid;
            }
        }
    }
    
    fclose(f);
    return -1;
}