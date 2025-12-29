#include <stdio.h>
#include <string.h> 
#include <fcntl.h> 
#include <sys/stat.h> 
#include <sys/types.h> 
#include <unistd.h> 
#include <stdlib.h>
#include <sys/wait.h>
#include <curses.h>
#include <sys/time.h>
#include <time.h>
#include <sys/file.h>
#include "logger.h"
#include "logger_custom.h"

int window_width;
int window_height;
int x_coord_Ta, y_coord_Ta;

// sig_atomic_t ensures atomic access during signal handling
volatile sig_atomic_t health_check = 0;
volatile sig_atomic_t should_exit = 0;

// Handler for health check signal from watchdog
void handle_signal(int signo) {
    if (signo == SIGUSR1) {
        health_check = 1; // Mark that we received a ping
    }
}

//termination handler from master process
void handle_terminate(int signo) {
    if (signo == SIGTERM) {
        should_exit = 1;
    }
}

static long current_millis() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}


// Same function as in BlackBoard, but read only the first two lines of the parameter file
void Parameter_File() {
    
    // Standardized exit codes
    #define USAGE_ERROR 64
    #define OPEN_FAIL 66
    #define EXEC_FAIL 127
    #define RUNTIME_ERROR 70

    FILE* file = fopen("Parameter_File.txt", "r");
    if (file == NULL) {
        LOG_ERRNO("Targets", "Error opening Parameter_File.txt");
        exit(OPEN_FAIL);
    }

    char line[256];
    int line_number = 0;

    while (fgets(line, sizeof(line), file)) {
        line_number++;

        char* tokens[10]; 
        int token_count = 0;
        char* token = strtok(line, "_");

        while (token != NULL && token_count < 10) {
            tokens[token_count] = token; 
            token_count++;
            token = strtok(NULL, "_"); 
        }

        switch (line_number) {
            case 1:
                if (token_count > 2) window_width = atoi(tokens[2]);
                break;
            case 2:
                if (token_count > 2) window_height = atoi(tokens[2]);
                break;
        }
    }
    fclose(file);
}


int main(int argc, char *argv[]) 
{

     // Setup signal handling FIRST
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGUSR1, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_terminate;
    sigaction(SIGTERM, &sa, NULL);
    
     // 1. LOG SELF immediately
    log_process("Targets", getpid());
    logger_init("system.log");
    LOG_INFO("Targets", "Starting Targets Process (PID=%d)", getpid());

    pid_t watchdog_pid = -1;
    int retries = 0;
    while (watchdog_pid == -1 && retries < 10) {
        sleep(1);
        watchdog_pid = get_pid_by_name("Watchdog");
        retries++;
    }
    
    if (watchdog_pid == -1) {
        LOG_ERROR("Targets", "Could not find Watchdog! Exiting.");
        return 1;
    }
    // Parameter file reading
    Parameter_File();
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <fd>\n", argv[0]);
        LOG_CRITICAL("Targets", "Insufficient arguments provided.");
        exit(USAGE_ERROR);
    }
    
    // Convert the argument to an integer file descriptor
    int fdTa = atoi(argv[1]);
    char buffer[100];

    // Target generation every 7 seconds
    const long target_interval_ms = 7000; 
    long last_target_ms = current_millis();
    srand(time(NULL) + getpid());

    while(1){
        if (should_exit) {
            LOG_INFO("Targets", "Termination signal received. Exiting main loop.");
            break;
        }
        
        // Checking alive signal
        if (health_check) {
            health_check = 0; // Reset flag
            kill(watchdog_pid, SIGUSR2); // Send signal back to watchdog
        }
        
        long now_ms = current_millis();
        if (now_ms - last_target_ms >= target_interval_ms) {
            x_coord_Ta = 1 + rand() % (window_width - 10);
            y_coord_Ta = 1 + rand() % (window_height - 10);
            last_target_ms = now_ms;
            sprintf(buffer, "%d,%d", x_coord_Ta, y_coord_Ta);
            write(fdTa, buffer, strlen(buffer)+1);
            LOG_INFO("Targets", "Generated new target at (%d, %d)", x_coord_Ta, y_coord_Ta);
        }
        usleep(100000); // Sleep 100ms to avoid busy-waiting
    }

    //clean up
    close(fdTa);
    logger_close();
    return 0;
}