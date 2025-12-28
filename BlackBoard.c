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
#include <errno.h>
#include <sys/file.h>
#include "logger.h"
#include <signal.h>
#include "logger_custom.h"
#define MAX_ITEMS 20
typedef struct {
    int x;
    int y;
} Point;

// Global variables and parameters
int window_width ;
int window_height;
int rph_intial;
double eta_intial;
int force_intial ;
int mass;        
int k_intial ;
int working_area;
int t_intial;  
int H = 0, W = 0;
int wh = 0, ww = 0;
bool running = true;
bool skip_drone_update = false;
bool repulsion_sent = false;
bool colors_enabled = false;
Point obstacles[MAX_ITEMS];
int obs_head = 0;
int obs_count = 0;

Point targets[MAX_ITEMS];
int tar_head = 0;
int tar_count = 0;

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

// Function to read parameter file
void Parameter_File() {
    FILE* file = fopen("Parameter_File.txt", "r");
    if (file == NULL) {
        perror("Error opening Parameter_File.txt");
        return;
    }

    char line[256];
    int line_number = 0;

    // Reading file line by line
    while (fgets(line, sizeof(line), file)) {
        line_number++;

        // Converting lines from parameter file into arrays of words
        // The words are separated by a token defined in the parameter file.
        char* tokens[10]; 
        int token_count = 0;
        char* token = strtok(line, "_");

        while (token != NULL && token_count < 10) {
            tokens[token_count] = token;
            token_count++;
            token = strtok(NULL, "_"); 
        }

        // Assign the respective values to the global parameters
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

static void layout_and_draw(WINDOW *win) {
    
    getmaxyx(stdscr, H, W);

    // Window with fixed margin
    wh = (H > 6) ? H - 6 : H;
    ww = (W > 10) ? W - 10 : W;
    if (wh < 3) wh = 3;
    if (ww < 3) ww = 3;

    // Resize and recenter window
    wresize(win, wh, ww);
    mvwin(win, (H - wh) / 2, (W - ww) / 2);

    // Clean up and draw again
    werase(stdscr);
    werase(win);
    if (colors_enabled) {
        wattron(win, COLOR_PAIR(4));
        box(win, 0, 0);
        wattroff(win, COLOR_PAIR(4));
    } else {
        box(win, 0, 0);
    }
   

    refresh();
    wrefresh(win);
}
  
int main(int argc, char *argv[]) {

    // Setup signal handling FIRST
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGUSR1, &sa, NULL);
    
    // LOG SELF immediately
    log_process("BlackBoard", getpid());
    logger_init("system.log");
    LOG_INFO("BlackBoard", "Starting BlackBoard Process (PID=%d)", getpid());
    
    pid_t watchdog_pid = -1;
    int retries = 0;
    while (watchdog_pid == -1 && retries < 10) {
        sleep(1);
        watchdog_pid = get_pid_by_name("Watchdog");
        retries++;
    }
    
    if (watchdog_pid == -1) {
        LOG_WARNING("BlackBoard","Could not find Watchdog! Exiting.\n");
        return 1;
    }
    Parameter_File();
    
    setlocale(LC_ALL, "");

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    (void)curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN, -1);  // drone
        init_pair(2, COLOR_GREEN, -1); // target
        init_pair(3, COLOR_RED, -1);   // obstacle
        init_pair(4, COLOR_BLUE, -1);  // border
        colors_enabled = true;
    }
    
    int term_h, term_w;
    getmaxyx(stdscr, term_h, term_w);
    // Parameter file values clamped to terminal size 
    int win_h = (window_height > 0) ? window_height : term_h;
    int win_w = (window_width  > 0) ? window_width  : term_w;
    if (win_h > term_h) win_h = term_h;
    if (win_w > term_w) win_w = term_w;

    WINDOW *win = newwin(win_w, win_h, 0, 0);
    layout_and_draw(win);
    // Allow window to report keys / KEY_RESIZE without blocking
    keypad(win, TRUE);
    wtimeout(win, 50); // wait up to 50 ms in wgetch, then continue to select()

    // Standardized exit codes
    #define USAGE_ERROR 64
    #define OPEN_FAIL 66
    #define EXEC_FAIL 127
    #define RUNTIME_ERROR 70

    if (argc < 7) 
    {
        fprintf(stderr, "Usage: %s <fd>\n", argv[0]);
        endwin();
        exit(USAGE_ERROR);
    }

    // Convert the argument to an integer file descriptor
    int fdToBB = atoi(argv[1]);   
    int fdFromBB = atoi(argv[2]);   
    int fdOb = atoi(argv[3]);    
    int fdTa = atoi(argv[4]);    
    char *path_bb = argv[5];
    int fdIn_BB = open(path_bb, O_RDONLY | O_NONBLOCK);
    if (fdIn_BB == -1) { LOG_ERRNO("BlackBoard","Failed to open In_BB Pipe"); return OPEN_FAIL; }
    int fdRepul =atoi(argv[6]);

    struct timeval tv;
    int retval;
    char strOb[100], strTa[100], strIn[100]; 
    char sToBB[135],sFromBB[135],sIn[10],sRepul[40];
    char format_stringOb[100] = "%d,%d";
    char format_stringTa[100] = "%d,%d";  
    
    float dx,dy;
    float distance;

    fd_set readfds;
    int maxfd = fdToBB;
    if (fdOb > maxfd) maxfd = fdOb;
    if (fdTa > maxfd) maxfd = fdTa;
    if (fdIn_BB > maxfd) maxfd = fdIn_BB;

    // Persistent Coordinates (Initialize off-screen or valid default)
    // Removed single coordinates in favor of arrays
                   

    float x_curr = ww / 2.0;

    float y_curr = wh / 2.0;
    

    // Initial handshake with drone to get starting position
    snprintf(sFromBB, sizeof(sFromBB), "%.0f,%.0f", x_curr, y_curr);
    write(fdFromBB, sFromBB, strlen(sFromBB) + 1);

    if(running == false){
        exit(0);
    }

    while (running) {

        if (should_exit) {
            LOG_INFO("BlackBoard","Termination signal received. Exiting main loop.\n");
            break;
        }    
        
        int ch = wgetch(win); // poll window for keys (returns KEY_RESIZE)
        sIn[0]='\0';
        repulsion_sent = false;

        if (ch == KEY_RESIZE) {
            // Store old window dimensions for scaling
            int old_ww = ww;
            int old_wh = wh;

            // Update ncurses internal structures for new dimensions
            resize_term(0, 0);
            layout_and_draw(win);

            // Recalculate and reproportionate obstacles
            for (int i = 0; i < obs_count; i++) {
                mvwprintw(win, obstacles[i].y, obstacles[i].x, " ");
                obstacles[i].x = (int)(((float)obstacles[i].x * ww) / old_ww);
                obstacles[i].y = (int)(((float)obstacles[i].y * wh) / old_wh);
                wattron(win, COLOR_PAIR(3));
                mvwprintw(win, obstacles[i].y, obstacles[i].x, "O");
                wattroff(win, COLOR_PAIR(3));
                // Clamp to window dimensions to prevent vanishing
                if (obstacles[i].x >= ww - 1) obstacles[i].x = ww - 2;
                if (obstacles[i].y >= wh - 1) obstacles[i].y = wh - 2;
            }

            // Recalculate and reproportionate targets
            for (int i = 0; i < tar_count; i++) {
                mvwprintw(win, targets[i].y, targets[i].x, " ");
                targets[i].x = (int)(((float)targets[i].x * ww) / old_ww);
                targets[i].y = (int)(((float)targets[i].y * wh) / old_wh);
                wattron(win, COLOR_PAIR(2));
                mvwprintw(win, targets[i].y, targets[i].x, "T");
                wattroff(win, COLOR_PAIR(2));
                // Clamp to window dimensions to prevent vanishing
                if (targets[i].x >= ww - 1) targets[i].x = ww - 2;
                if (targets[i].y >= wh - 1) targets[i].y = wh - 2;
            }

            // Clamp current position to new window bounds (in case it's off-screen now)
            // Clamp to window dimensions to prevent vanishing
            x_curr = ww / 2;
            y_curr = wh / 2;
            snprintf(sFromBB, sizeof(sFromBB), "%.0f,%.0f", x_curr, y_curr);
            write(fdFromBB, sFromBB, strlen(sFromBB) + 1);
            skip_drone_update = true;
        }
        

        // Clear window for new frame
        werase(win);
        if (colors_enabled) {
            wattron(win, COLOR_PAIR(4));
            box(win, 0, 0);
            wattroff(win, COLOR_PAIR(4));
        } else {
            box(win, 0, 0);
        }

        FD_ZERO(&readfds);
        FD_SET(fdToBB, &readfds);
        FD_SET(fdOb, &readfds);
        FD_SET(fdTa, &readfds);
        FD_SET(fdIn_BB, &readfds);

        // Small timeout so loop stays responsive
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        retval = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (retval == -1) break;
        else if (retval > 0) {
            
            if (FD_ISSET(fdIn_BB, &readfds)) {
                // Clear strIn before reading new data
                memset(strIn, 0, sizeof(strIn)); 
                ssize_t bytes = read(fdIn_BB, strIn, sizeof(strIn)-1);
                
                if (bytes > 0) {
                    sscanf(strIn, "%1s", sIn); 
                    LOG_INFO("BlackBoard","Received input command: %s", sIn);
                } 
                else { 
                    LOG_ERROR("BlackBoard", "Input pipe closed unexpectedly");
                    running = false; } // Pipe closed
            }

            // Receiving coordinates from drone pipe
            if (FD_ISSET(fdToBB, &readfds)) {
                ssize_t bytes = read(fdToBB, sToBB, sizeof(sToBB)-1);
                if (bytes > 0) {
                    if (skip_drone_update) {
                        skip_drone_update = false;
                    } 
                    else {
                        sToBB[bytes] = '\0';
                        sscanf(sToBB, "%f,%f", &x_curr, &y_curr);
                        LOG_INFO("BlackBoard","Received drone coordinates:");
                    }   
                }
                else { 
                    LOG_ERROR("BlackBoard", "Drone pipe closed unexpectedly");
                    running = false; }
            }
            

            // Receiving coordinates from obstacle pipe
            if (FD_ISSET(fdOb, &readfds)) {
                ssize_t bytes = read(fdOb, strOb, sizeof(strOb)-1);
                if (bytes > 0) {
                    strOb[bytes] = '\0';
                    int new_x, new_y;
                    sscanf(strOb, format_stringOb, &new_x, &new_y);
                    LOG_INFO("BlackBoard","Received obstacle coordinates:");

                    //apply to current window size
                    float ratio_x = (float)new_x / (float)window_width;
                    float ratio_y = (float)new_y / (float)window_height;
                    
                    //Aplies to current window size
                    new_x=ratio_x*ww;
                    new_y=ratio_y*wh;

                    // Clamp to window dimensions to prevent vanishing
                    if (new_x >= ww - 1) new_x = ww - 2;
                    if (new_y >= wh - 1) new_y = wh - 2;
                    
                    // Store in array
                    obstacles[obs_head].x = new_x;
                    obstacles[obs_head].y = new_y;
                    obs_head = (obs_head + 1) % MAX_ITEMS;
                    if (obs_count < MAX_ITEMS) obs_count++;
                }
                else { 
                    LOG_ERROR("BlackBoard", "Obstacle pipe closed unexpectedly");
                    running = false; }
            }

            // Reading coordinates from target pipe
            if (FD_ISSET(fdTa, &readfds)) {
                ssize_t bytes = read(fdTa, strTa, sizeof(strTa)-1);
                if (bytes > 0) {
                    strTa[bytes] = '\0';
                    int new_x, new_y;
                    sscanf(strTa, format_stringTa, &new_x, &new_y);
                    LOG_INFO("BlackBoard","Received target coordinates:");

                    //apply to current window size
                    float ratio_x = (float)new_x / (float)window_width;
                    float ratio_y = (float)new_y / (float)window_height;
                    
                    //Aplies to current window size
                    new_x=ratio_x*ww;
                    new_y=ratio_y*wh;
                    
                    // Clamp to window dimensions to prevent vanishing
                    if (new_x >= ww - 1) new_x = ww - 2;
                    if (new_y >= wh - 1) new_y = wh - 2;

                    // Store in array
                    if (tar_count < MAX_ITEMS) {
                        targets[tar_count].x = new_x;
                        targets[tar_count].y = new_y;
                        tar_count++;
                    } else {
                        // Drop the oldest target to make room
                        memmove(&targets[0], &targets[1], sizeof(targets[0]) * (MAX_ITEMS - 1));
                        targets[MAX_ITEMS - 1].x = new_x;
                        targets[MAX_ITEMS - 1].y = new_y;
                    }
                }
                else { 
                    LOG_ERROR("BlackBoard", "Target pipe closed unexpectedly");
                    running = false; }
            }
        }                

        char input_key= sIn[0];
        // Quit the game
        if (input_key=='q'){
            running = false;
        }

        // If drone overlaps a target, remove that target
        {
            int drone_x = (int)x_curr;
            int drone_y = (int)y_curr;
            for (int i = 0; i < tar_count; ) {
                if (targets[i].x == drone_x && targets[i].y == drone_y) {
                    if (i < tar_count - 1) {
                        memmove(&targets[i], &targets[i + 1], sizeof(targets[0]) * (tar_count - i - 1));
                    }
                    tar_count--;
                    continue;
                }
                i++;
            }
        }

        // Reset button - recentre drone
        if (input_key == 'a'){
            mvwprintw(win, y_curr, x_curr, " " );
            x_curr=ww/2;
            y_curr=wh/2;

            snprintf(sFromBB, sizeof(sFromBB), "%.0f,%.0f", x_curr, y_curr);     
            write(fdFromBB, sFromBB, strlen(sFromBB) + 1);
            wrefresh(win);
            LOG_INFO("BlackBoard","Drone recentred to");
        }

        // Pause the game, wait for 'u' to unpause
        if (input_key == 'p') {
            mvwprintw(win, 0, 0, "Game Paused, Press 'u' to Resume");
            wrefresh(win);

            fd_set pause_fds;
            struct timeval pause_tv;
            int pause_ret;
            
            

            while (running) {
                FD_ZERO(&pause_fds);
                FD_SET(fdIn_BB, &pause_fds);
                int maxfd_pause = fdIn_BB;
                pause_tv.tv_sec = 0;
                pause_tv.tv_usec = 100 * 1000; 

                pause_ret = select(maxfd_pause + 1, &pause_fds, NULL, NULL, &pause_tv);
                if (pause_ret > 0) {
                    // If input pipe has data, read and update sIn
                    if (FD_ISSET(fdIn_BB, &pause_fds)) {
                        ssize_t bytes = read(fdIn_BB, strIn, sizeof(strIn) - 1);
                        if (bytes > 0) {
                            strIn[bytes] = '\0';
                            sscanf(strIn, "%s", sIn);
                            if (sIn[0] == 'u') { 
                                break;
                            }
                            if (sIn[0] == 'q'){
                                running=false;
                                break;
                            }
                            } else {
                                running = false; //pipe broken
                                break;
                            }
                    }
                }
            }

            input_key=' ';
            werase(win);
            if (colors_enabled) {
                wattron(win, COLOR_PAIR(4));
                box(win, 0, 0);
                wattroff(win, COLOR_PAIR(4));
            } else {
                box(win, 0, 0);
            }
            mvwprintw(win, 0, 0, "                       ");    

        }
        
        //clamping drone to window size
        if (x_curr >= ww - 1) {
            x_curr = ww - 1;
            
            snprintf(sFromBB, sizeof(sFromBB), "%.0f,%.0f", x_curr, y_curr);        
            write(fdFromBB, sFromBB, strlen(sFromBB) + 1);
        } else if (x_curr <= 0) {
            x_curr = 0;
            snprintf(sFromBB, sizeof(sFromBB), "%.0f,%.0f", x_curr, y_curr);       
            write(fdFromBB, sFromBB, strlen(sFromBB) + 1);
        }

        if (y_curr >= wh - 1) {
            y_curr = wh - 1;
            snprintf(sFromBB, sizeof(sFromBB), "%.0f,%.0f", x_curr, y_curr);       
            write(fdFromBB, sFromBB, strlen(sFromBB) + 1);
            
        } else if (y_curr <= 0) {
            y_curr = 0;
            snprintf(sFromBB, sizeof(sFromBB), "%.0f,%.0f", x_curr, y_curr);      
            write(fdFromBB, sFromBB, strlen(sFromBB) + 1);
            
        }

        // Draw Obstacles while checking if we having an closeness of a drone
        for(int i=0; i<obs_count; i++) {
            if (obstacles[i].x > 0 && obstacles[i].y > 0){ 
                wattron(win, COLOR_PAIR(3));
                mvwprintw(win, obstacles[i].y, obstacles[i].x, "O");
                wattroff(win, COLOR_PAIR(3));
            }
            
            dx =  x_curr - obstacles[i].x;
            dy =  y_curr - obstacles[i].y;
            distance = sqrt(pow(dx, 2) + pow(dy, 2));
            
            if (!repulsion_sent) { 
                if(distance < rph_intial ){
    
                    if (distance <= 1.0) {distance = 1.0;}
    
                    snprintf(sRepul, sizeof(sRepul), "%.2f,%.2f,%.2f",distance, dx, dy);
                    write(fdRepul, sRepul, strlen(sRepul) + 1);
                    //it recieves that it has reached the obstacle
                    //dprintf(STDERR_FILENO, "BB: Obstacle repulsion - dist=%.2f\n", distance);
                    repulsion_sent = true;
                } 
            }
        }            

        // Check for boundary repulsion
        // Only check boundaries if NO obstacle repulsion was sent
        if (!repulsion_sent) {
            // Left boundary
            if (x_curr < rph_intial) {
                distance = x_curr;
                if (distance < 1.0) distance = 1.0;
                dx = distance;  
                dy = 0;
                
                snprintf(sRepul, sizeof(sRepul), "%.2f,%.2f,%.2f", distance, dx, dy);
                write(fdRepul, sRepul, strlen(sRepul) + 1);
                repulsion_sent = true;
                
                //dprintf(STDERR_FILENO, "BB: Left boundary repulsion - dist=%.2f\n", distance);
            }
            // Right boundary
            else if (x_curr > (ww - rph_intial)) {
                distance = ww - x_curr;
                if (distance < 1.0) distance = 1.0;
                dx = -distance;  
                dy = 0;
                
                snprintf(sRepul, sizeof(sRepul), "%.2f,%.2f,%.2f", distance, dx, dy);
                write(fdRepul, sRepul, strlen(sRepul) + 1);
                repulsion_sent = true;
                
                //dprintf(STDERR_FILENO, "BB: Right boundary repulsion - dist=%.2f\n", distance);
            }
            
            // Top boundary
            if (y_curr < rph_intial && !repulsion_sent) {
                distance = y_curr;
                if (distance < 1.0) distance = 1.0;
                dx = 0;
                dy = distance;  

                snprintf(sRepul, sizeof(sRepul), "%.2f,%.2f,%.2f", distance, dx, dy);
                write(fdRepul, sRepul, strlen(sRepul) + 1);
                repulsion_sent = true;
                
                //dprintf(STDERR_FILENO, "BB: Top boundary repulsion - dist=%.2f\n", distance);
            }
            // Bottom boundary
            else if (y_curr > (wh - rph_intial) && !repulsion_sent) {
                distance = wh - y_curr;
                if (distance < 1.0) distance = 1.0;
                dx = 0;
                dy = -distance;  
                
                snprintf(sRepul, sizeof(sRepul), "%.2f,%.2f,%.2f", distance, dx, dy);
                write(fdRepul, sRepul, strlen(sRepul) + 1);
                repulsion_sent = true;
                
                //dprintf(STDERR_FILENO, "BB: Bottom boundary repulsion - dist=%.2f\n", distance);
            }
        }
        
        // Draw Targets
        for(int i=0; i<tar_count; i++) {
             if (targets[i].x > 0 && targets[i].y > 0) {
                wattron(win, COLOR_PAIR(2));
                mvwprintw(win, targets[i].y, targets[i].x, "T");
                wattroff(win, COLOR_PAIR(2));
             }
        }

        // Draw the drone 
        wattron(win, COLOR_PAIR(1));
        mvwprintw(win, (int)y_curr, (int)x_curr, "+");
        wattroff(win, COLOR_PAIR(1));
        wrefresh(win);

        
        // Checking alive signal
        if (health_check) {
            health_check = 0; // Reset flag
            kill(watchdog_pid, SIGUSR2); // Send signal back to watchdog
        }
        usleep(10000); 
        
    }
    
    // Cleanup
    close(fdToBB);
    close(fdFromBB);
    close(fdOb);
    close(fdTa);
    close(fdRepul);
    close(fdIn_BB);

    delwin(win);
    endwin();
    logger_close();
    return 0;

}





