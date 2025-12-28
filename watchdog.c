#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include "logger.h"
#include <sys/file.h>  
#include <time.h>
#include "logger_custom.h"

#define CHECK_INTERVAL 10
#define RESPONSE_TIMEOUT 10
#define MAX_PROCESSES 50

typedef struct {
    pid_t pid;
    char name[100];
    int active;
} ProcessInfo;

ProcessInfo processes[MAX_PROCESSES];
int process_count = 0;
volatile sig_atomic_t response_received = 0;
volatile sig_atomic_t timeout_occurred = 0;
volatile sig_atomic_t terminate_flag = 0;

// Helper to log watchdog specific events
void log_watchdog(const char *message) {
    FILE *f = fopen("watchdog_log.log", "a");
    if (!f) return;
    if (flock(fileno(f), LOCK_EX) == -1) { fclose(f); return; }
    
    time_t now = time(NULL);
    char *timestamp = ctime(&now);
    timestamp[strlen(timestamp)-1] = '\0';
    fprintf(f, "[%s] %s\n", timestamp, message);
    fflush(f);
    flock(fileno(f), LOCK_UN);
    fclose(f);
}

// Handlers for signals
void response_handler(int signo) {
    if (signo == SIGUSR2) response_received = 1;
}

// Handler for timeout alarm
void timeout_handler(int signo) {
    if (signo == SIGALRM) timeout_occurred = 1;
}

// Handler for termination signal from Master Process
void terminate_handler(int signo) {
    if (signo == SIGTERM) {
        terminate_flag = 1;
        char msg[256];
        snprintf(msg, 256, "All Processes terminated by Master Process");
        log_watchdog(msg);
    }
}

// Load the process_log from file
// This allows for the watchdog to have all processes pids and names
int load_processes() {
    FILE *f = fopen("process_log.log", "r");
    if (!f) {
        LOG_ERROR("Watchdog", "Failed to open process_log.log");
        return 0;
    }

    // LOCK THE FILE
    if (flock(fileno(f), LOCK_EX) == -1) {
        fclose(f);
        return 0;
    }
    
    char line[256];
    process_count = 0; // WARNING: This resets your list every time!
    pid_t my_pid = getpid();
    
    while (fgets(line, sizeof(line), f) && process_count < MAX_PROCESSES) {
        char name[100];
        int pid;
        if (sscanf(line, "Process: %s | PID: %d", name, &pid) == 2) {
            if (pid == my_pid) continue;
            if (strcmp(name, "Master") == 0) continue;
            
            processes[process_count].pid = pid;
            strncpy(processes[process_count].name, name, 99);
            processes[process_count].active = 1; // Resets status to 1
            process_count++;
        }
    }
    
    flock(fileno(f), LOCK_UN); // Unlock
    fclose(f);
    return process_count;
}

// Core Logic: Ping -> Wait -> Kill if necessary
void check_process(int index) {
    ProcessInfo *proc = &processes[index];
    char msg[256];

    // Check if process still exists in OS
    if (kill(proc->pid, 0) == -1) {
        snprintf(msg, 256, "Process '%s' (PID=%d) no longer exists", proc->name, proc->pid);
        log_watchdog(msg);
        proc->active = 0;
        return;
    }
    
    response_received = 0;
    timeout_occurred = 0;
    
    // Send PING
    snprintf(msg, 256, "Checking '%s' (PID=%d)...", proc->name, proc->pid);
    log_watchdog(msg);
    kill(proc->pid, SIGUSR1);
    
    // Set Timeout Alarm
    alarm(RESPONSE_TIMEOUT);
    
    // Wait for PONG or ALARM
    while (!response_received && !timeout_occurred) {
        pause(); // Sleep until any signal arrives
    }
    
    alarm(0); // Disable alarm
    
    if (response_received) {
        snprintf(msg, 256, "✓ '%s' (PID=%d) is ALIVE", proc->name, proc->pid);
        log_watchdog(msg);
    } else {
        // TIMEOUT - TERMINATE
        snprintf(msg, 256, "✗ '%s' (PID=%d) TIMEOUT - TERMINATING", proc->name, proc->pid);
        log_watchdog(msg);
        kill(proc->pid, SIGKILL); // Force Kill
        proc->active = 0;
    }
}

// Main Watchdog Loop, sets up signal handlers and monitors child processes
int main() {
    // Setup Handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = response_handler;
    sigaction(SIGUSR2, &sa, NULL);
    
    sa.sa_handler = timeout_handler;
    sigaction(SIGALRM, &sa, NULL);

    sa.sa_handler = terminate_handler;
    sigaction(SIGTERM, &sa, NULL);

    // Initialize Logs
    FILE *f = fopen("watchdog_log.log", "w"); if (f) fclose(f);
    log_process("Watchdog", getpid());
    printf("Watchdog started (PID=%d)\n", getpid());
    
       
    printf("Loading processes from process_log.log...\n");
    while (load_processes() == 0) {
        sleep(1);
        printf("No processes to monitor!\n");
    }
    
    LOG_INFO("Watchdog","Monitoring %d processes. Check logs: tail -f watchdog_log.log\n", process_count);
    
    int cycle = 0;
    while (1) {
        if (terminate_flag) {
            log_watchdog("Watchdog received termination signal, exiting.");
            break;  // Exit if termination signal received
        }
        sleep(CHECK_INTERVAL);
        cycle++;
        load_processes();
        char msg[256];
        snprintf(msg, 256, "--- Health Check Cycle #%d ---", cycle);
        log_watchdog(msg);
        
        int alive = 0;
        for (int i = 0; i < process_count; i++) {
            if (processes[i].active) {
                check_process(i);
                if (processes[i].active) alive++;
            }
        }
        
        if (alive == 0) {
            LOG_INFO("Watchdog","\nAll processes dead. Watchdog exiting.\n");
            log_watchdog("All processes dead. Watchdog exiting.");
            break;
        }
    }
    return 0;
}