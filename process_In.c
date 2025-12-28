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
#include <termios.h>
#include <signal.h>
#include <sys/file.h>
#include "logger.h"
#include "logger_custom.h"


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
    log_process("Input", getpid());
    logger_init("system.log");
    LOG_INFO("Input", "Starting Input Process (PID=%d)", getpid());
    
    pid_t watchdog_pid = -1;
    int retries = 0;
    while (watchdog_pid == -1 && retries < 10) {
        sleep(1);
        watchdog_pid = get_pid_by_name("Watchdog");
        retries++;
    }
    
    if (watchdog_pid == -1) {
        LOG_ERROR("Input", "Could not find Watchdog! Exiting.");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    // Standardized exit codes
    #define USAGE_ERROR 64
    #define OPEN_FAIL 66
    #define EXEC_FAIL 127
    #define RUNTIME_ERROR 70

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <fd>\n", argv[0]);
        return USAGE_ERROR;
    }

    //Convert the argument to an integer file descriptor
    int fdIn = atoi(argv[1]);
    char *path_bb = argv[2];
    int fdIn_BB = open(path_bb, O_WRONLY);
    if (fdIn_BB == -1) { 
        LOG_ERRNO("Input", "Failed to open In_BB Pipe");
        return OPEN_FAIL; 
    }

    // Setting up the terminal to read single characters without waiting for Enter
    struct termios old_tio, new_tio;
    tcgetattr(STDIN_FILENO, &old_tio); 
    new_tio = old_tio;                 

    // Disable canonical mode (waiting for Enter) and echo
    new_tio.c_lflag &= ~(ICANON); 
    new_tio.c_cc[VMIN] = 0;  
    new_tio.c_cc[VTIME] = 0; 
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio); 

    printf("BEGIN GAME!:D\n");
    printf("Controls: 'w,e,r,f,v,c,x,s' - movement, 'a' - reset position, 'p' - pause, 'u' - unpause, 'q' - quit\n"); 

    // Characters to be written
    // One is the typed character, the other is the "automatic Enter"
    char c;
    char newline = '\n'; 

    while (1) 
    {
        if (should_exit) {
            LOG_INFO("Input", "Termination signal received. Exiting main loop.");
            break;
        }

        if (read(STDIN_FILENO, &c, 1) > 0) 
        {
            write(fdIn, &c, 1);

            if (c == 'q' || c == 'a' || c == 'p' || c == 'u') {
                
                
                write(fdIn_BB, &c, 1); 
                write(fdIn_BB, &newline, 1);     
                
                if (c == 'q') {
                // --- 3. RESTORE TERMINAL ---
                // This is critical, or the terminal will be "broken" after
                tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
                close(fdIn_BB);
                close(fdIn);
                exit(EXIT_SUCCESS);
            }
        }
            // 2. Write the "automatic Enter"
            write(fdIn, &newline, 1);
               
        }
        
        // Checking alive signal
        if (health_check) {
            health_check = 0; // Reset flag
            kill(watchdog_pid, SIGUSR2); // Send signal back to watchdog
        }
        
        usleep(10000);
    }

    // Restore the old terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
    logger_close();
    return 0;
}

