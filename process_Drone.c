#include <stdio.h>
#include <string.h> 
#include <fcntl.h> 
#include <sys/stat.h> 
#include <sys/types.h> 
#include <unistd.h> 
#include <stdlib.h>
#define _XOPEN_SOURCE_EXTENDED
#include <locale.h>
#include <ncurses.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>
#include <signal.h>
#include <errno.h>
#include <sys/file.h>
#include "logger.h"
#include "logger_custom.h"


int window_width;
int window_height;
float rph_intial;
double eta_intial;
int force_intial;
int mass;        
int k_intial;
int working_area ;
int t_intial ;  
bool running = true;
bool repul =false;

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


void log_coordinates(const char *message) {
    FILE *f = fopen("coordinates_log.log", "a");
    if (!f) {
        LOG_ERRNO("Drone","Failed to open coordinates_log.log");
        return;
    }
    if (flock(fileno(f), LOCK_EX) == -1) { fclose(f); return; }
    
    time_t now = time(NULL);
    char *timestamp = ctime(&now);
    timestamp[strlen(timestamp)-1] = '\0';
    fprintf(f, "[%s] %s\n", timestamp, message);
    fflush(f);
    flock(fileno(f), LOCK_UN);
    fclose(f);
}

// Function to identify opposite keys
char get_opposite_key(char key) {
    switch (key) {
        case 'w': return 'v'; // Up-Left  vs Down-Right
        case 'e': return 'c'; // Up       vs Down
        case 'r': return 'x'; // Up-Right vs Down-Left
        case 's': return 'f'; // Left     vs Right
        case 'f': return 's'; // Right    vs Left
        case 'x': return 'r'; // Down-Left vs Up-Right
        case 'c': return 'e'; // Down     vs Up
        case 'v': return 'w'; // Down-Right vs Up-Left
        default: return 0;
    }
}

void Parameter_File() {
    FILE* file = fopen("Parameter_File.txt", "r");
    if (file == NULL) {
        LOG_ERRNO("Drone","Error opening Parameter_File.txt");
        return;
    }

    char line[256];
    int line_number = 0;

    while (fgets(line, sizeof(line), file)) {
        line_number++;

        char* tokens[10]; 
        int token_count = 0;
        char* token = strtok(line, "_");

        while (token != NULL && token_count < 10) {
            tokens[token_count] = token; // Add token to our array
            token_count++;
            token = strtok(NULL, "_"); // Get next token
        }

        switch (line_number) {
            case 1:
                if (token_count > 2) window_width = atoi(tokens[2]);
                break;
            case 2:
                if (token_count > 2) window_height = atoi(tokens[2]);
                break;
            case 3:
                if (token_count > 2) rph_intial = atoi(tokens[2]);
                break;
            case 4:
                if (token_count > 2) eta_intial = atof(tokens[2]); // Use atof() for doubles
                break;
            case 5:
                if (token_count > 2) force_intial = atoi(tokens[2]);
                break;
            case 6:
                if (token_count > 1) mass = atoi(tokens[1]); // You used index [1] here
                break;
            case 7:
                if (token_count > 2) k_intial = atoi(tokens[2]);
                break;
            case 8:
                if (token_count > 2) working_area = atoi(tokens[2]);
                break;
            case 9:
                if (token_count > 2) t_intial = atoi(tokens[2]);
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

    // LOG SELF immediately
    log_process("Drone", getpid());
    logger_init("system.log");
    LOG_INFO("Drone", "Starting Drone Process (PID=%d)", getpid());
    
    // Reset coordinates log at start
    FILE *reset_f = fopen("coordinates_log.log", "w");
    if (reset_f) fclose(reset_f);
    
    pid_t watchdog_pid = -1;
    int retries = 0;
    while (watchdog_pid == -1 && retries < 10) {
        sleep(1);
        watchdog_pid = get_pid_by_name("Watchdog");
        retries++;
    }
    
    if (watchdog_pid == -1) {
        LOG_WARNING("Drone","Could not find Watchdog! Exiting.\n");
        return 1;
    }
    

    // Standardized exit codes
    #define USAGE_ERROR 64
    #define OPEN_FAIL 66
    #define EXEC_FAIL 127
    #define RUNTIME_ERROR 70

    Parameter_File();

    if (argc < 5) {
        fprintf(stderr, "Usage: %s <fd>\n", argv[0]);
        exit(USAGE_ERROR);
    }

    // Convert the argument to an integer file descriptor
    int fdIn = atoi(argv[1]);
    int fdFromBB= atoi(argv[2]);
    int fdToBB = atoi(argv[3]);
    int fdRepul = atoi(argv[4]);

    // Avoid process termination on broken pipe and print FD debug
    signal(SIGPIPE, SIG_IGN);
    dprintf(STDERR_FILENO, "DRONE: start fds fdIn=%d fdFromBB=%d fdToBB=%d\n",fdIn, fdFromBB, fdToBB);
    
    struct timeval tv={0,0};
    int retval;
    char strIn[135],sOut[135],strFromBB[100], strRepul[50]; 
    char sIn[10];

    float distance=0;
    float dx=0,dy=0;

    float x_curr = 0, y_curr = 0;
    float x_prev = 0, y_prev = 0;
    float x_prev2 = 0, y_prev2 = 0;
    float x_update =0 , y_update =0;

    ssize_t bytes = 0;
    while (bytes <= 0) {
        bytes = read(fdFromBB, strFromBB, sizeof(strFromBB)-1);
    }
    strFromBB[bytes] = '\0';

    sscanf(strFromBB, "%f,%f",&x_curr, &y_curr);
    
    x_prev = x_curr;
    x_prev2 = x_curr;
    y_prev = y_curr;
    y_prev2 = y_curr;

    fd_set readfds;

    int maxfd = fdIn;
    if (fdFromBB > maxfd) maxfd = fdFromBB;
    if (fdRepul > maxfd) maxfd = fdRepul;
 
    float diag_force = (float)force_intial * M_SQRT1_2;
    float T= t_intial / 1000.0; // Convert ms to seconds

    char active_key = ' '; // The key currently driving the physics
    int boost_level = 0;   // 0 = 0%, 1 = 20%, 2 = 40% (Max)

    while(running){
 
        if (should_exit) {
            LOG_INFO("Drone","Termination signal received. Exiting main loop.\n");
            break;
        }

        FD_ZERO(&readfds);
        FD_SET(fdIn, &readfds);
        FD_SET(fdFromBB, &readfds);
        FD_SET(fdRepul, &readfds);

        tv.tv_sec = 0;
        tv.tv_usec = 0; // Use a zero-timeout for select to poll

        retval = select(maxfd + 1, &readfds, NULL, NULL, &tv);

        if (retval == -1) {
            break;
        } 
        else if (retval > 0) {

            // Read from keyboard
            if (FD_ISSET(fdIn, &readfds)) {
                ssize_t bytes = read(fdIn, strIn, sizeof(strIn)-1);
                if (bytes > 0) {
                    strIn[bytes] = '\0';
                    sscanf(strIn, "%s", sIn);
                    LOG_INFO("Drone", "Received key inputs");
                } else { 
                    LOG_ERROR("Drone", "Input pipe closed unexpectedly");
                    running = false; } // Pipe closed
            }

            // Read from black board PIPE
            if (FD_ISSET(fdFromBB, &readfds)) {
                ssize_t bytes = read(fdFromBB, strFromBB, sizeof(strFromBB)-1);
                if (bytes > 0) {
                    strFromBB[bytes] = '\0';
                    sscanf(strFromBB, "%f,%f",&x_update, &y_update);
                    x_prev = x_update;
                    x_prev2 = x_update;
                    y_prev = y_update;
                    y_prev2 = y_update;
                    LOG_INFO("Drone", "Received key inputs");
                }else { 
                    LOG_ERROR("Drone", "Input pipe closed unexpectedly");
                    running = false; }
            }

            // Read repulsion keys
            if (FD_ISSET(fdRepul,&readfds)){
                 ssize_t bytes = read(fdRepul, strRepul, sizeof(strRepul)-1);
                if (bytes > 0) {
                    strRepul[bytes] = '\0';
                    sscanf(strRepul, "%f,%f,%f",&distance,&dx,&dy);
                    repul=true;
                    LOG_INFO("Drone", "Received repulsion inputs");
                } else { 
                    if (bytes == 0) { // Pipe closed
                        LOG_ERROR("Drone", "Input pipe closed unexpectedly");
                        running = false;
                    }
                }
            }
        }                
        
        char input_key = sIn[0];
        //char repul_key =sRepul[0];

        if (input_key != ' ' && input_key != 0) {

            // Case A: Quit
            if (input_key == 'q') {
                running = false;
            }
            // Case B: Brake (Stop Engine)
            else if (input_key == 'd') {
                boost_level = 0;
                active_key = ' ';
            }
            // Case C: Pause Logic
            else if (input_key == 'p') {
                // Enter Blocking Wait for 'u'
                while(1) {
                    ssize_t b = 0;
                    while (b <= 0) b = read(fdIn, strIn, sizeof(strIn)-1);
                    strIn[b] = '\0';
                    sscanf(strIn, "%s", sIn);
                    if (sIn[0] == 'u') break;
                }
            }
            // Case E: Same Direction -> Increase Speed
            else if (input_key == active_key) {
                if (boost_level < 2) boost_level++; 
            }
            // Case F: Opposite Direction -> Decrease Speed
            else if (input_key == get_opposite_key(active_key)) {
                boost_level--;
                if (boost_level < 0) {
                    // Crossed the threshold: Reverse Direction
                    boost_level = 0;
                    active_key = input_key;
                }
            }
            // Case G: Intializes the first key pressed and if New Direction (Orthogonal) -> Switch immediately
            else {
                boost_level = 0;
                active_key = input_key;
            }
        }
        

        // Clear physical input so keys don't "stick" in the buffer,
        // BUT active_key persists, so the engine keeps running.
        sIn[0] = ' '; 
        
        // Note: We no longer reset boost_level and active_key when repulsion occurs.
        // This allows both user input force and repulsion force to be applied simultaneously.

        float multiplier = 1.0 + (boost_level * 0.2);
        float cur_force = force_intial * multiplier;
        float cur_diag = diag_force * multiplier;

        float Fx = 0, Fy = 0;
        float total_fx=0, total_fy=0;
       

         switch (active_key) {
            case 'e': Fy = -cur_force; break; // Up
            case 'c': Fy =  cur_force; break; // Down
            case 's': Fx = -cur_force; break; // Left
            case 'f': Fx =  cur_force; break; // Right
            case 'w': Fx = -cur_diag; Fy = -cur_diag; break;
            case 'r': Fx =  cur_diag; Fy = -cur_diag; break;
            case 'x': Fx = -cur_diag; Fy =  cur_diag; break;
            case 'v': Fx =  cur_diag; Fy =  cur_diag; break;
        }

        total_fx= Fx;
        total_fy= Fy;
        float MAX_REPULSION = 10.0;

        if (repul){

            float dist_f = distance;

            //this is the bridge between physics and pixels 
            float scale_factor = 200;

            float term_rph = (1.0 / rph_intial);
            float norm_dx = dx/dist_f;
            float norm_dy = dy/dist_f;
                        
            float repulsion_force= scale_factor * eta_intial * 1/pow(dist_f,2) * (1/dist_f - term_rph);

            if (repulsion_force > MAX_REPULSION) repulsion_force = MAX_REPULSION;
            float repul_x = repulsion_force * norm_dx;
            float repul_y = repulsion_force * norm_dy;

            // Add repulsion (Push away), it has its own sign to be repul
            total_fx += repul_x;
            total_fy += repul_y;

            char msg[256];
            snprintf(msg, 256, "DRONE: Repulsion - dist=%.2f, Fmag=%.4f, Fx=%.4f, Fy=%.4f", dist_f, repulsion_force, repul_x, repul_y);
            log_coordinates(msg);
            repul=false;
        }
    
        float denom = mass + (k_intial * T);
        float history_factor = (2 * mass) + (k_intial * T);

        float num_x = (total_fx * T * T) + (x_prev * history_factor) - (mass * x_prev2);
        float x_new = num_x / denom;

        float num_y = (total_fy * T * T) + (y_prev * history_factor) - (mass * y_prev2);
        float y_new = num_y / denom;
        
        // Update history
        x_prev2 = x_prev; x_prev = x_new;
        y_prev2 = y_prev; y_prev = y_new;
        x_curr = x_new;
        y_curr = y_new;
        
        // Sends the current position back to bb
        snprintf(sOut, sizeof(sOut), "%d,%d", (int)(x_curr), (int)(y_curr));
        ssize_t w = write(fdToBB, sOut, strlen(sOut) + 1);
        char msg[256];
        // Log coordinates with timestamp
        if (w > 0) {
            snprintf(msg, 256, "Drone: coordinates %d,%d - write successful", (int)x_curr, (int)y_curr);
            log_coordinates(msg);
        } else {
            snprintf(msg, 256, "Drone: coordinates %d,%d - write failed: %s", (int)x_curr, (int)y_curr, strerror(errno));
            log_coordinates(msg);
            running = false;
        }
           
        
        // Checking alive signal
        if (health_check) {
            health_check = 0; // Reset flag
            kill(watchdog_pid, SIGUSR2); // Send signal back to watchdog
        }
        
        

    usleep(10000);
}

    // Close all file descriptors to signal EOF to parent
    close(fdIn);
    close(fdFromBB);
    close(fdToBB);
    close(fdRepul);
    logger_close();
   

    return 0;
}