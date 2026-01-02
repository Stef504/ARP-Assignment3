#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <sys/file.h>
#include "logger.h"
#include "logger_custom.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Transformation parameters, sets values
#define VIRTUAL_X0 0.0
#define VIRTUAL_Y0 0.0
#define VIRTUAL_ALFA 0.0 // Angle in radians


//sig_atomic_t ensures atomic access during signal handling
volatile sig_atomic_t should_exit = 0;

//termination handler from master process
void handle_terminate(int signo) {
    if (signo == SIGTERM) {
        should_exit = 1;
        // Direct write to console to prove we got the signal
        const char *msg = "\n[DEBUG] CommServer received SIGTERM\n";
        write(STDOUT_FILENO, msg, strlen(msg));
    }
}

// Virtual coordinate conversion functions
typedef struct {
    float x;
    float y;
} Coord;

// Convert local to virtual (standard: top-left origin)
// Formula:
// x1 = x0 + x cos(alfa) - y sin(alfa)
// y1 = y0 + x sin(alfa) + y cos(alfa)
// TO BE TESTED: alfa = 0, pi/2, pi
Coord local_to_virtual(Coord local, int window_width, int window_height) {
    Coord v;
    float x = local.x;
    float y = local.y;
    
    v.x = VIRTUAL_X0 + x * cos(VIRTUAL_ALFA) - y * sin(VIRTUAL_ALFA);
    v.y = VIRTUAL_Y0 + x * sin(VIRTUAL_ALFA) + y * cos(VIRTUAL_ALFA);
    
    return v;
}

// Convert virtual to local
// Inverse Formula:
// x = (x1 - x0) cos(alfa) + (y1 - y0) sin(alfa)
// y = -(x1 - x0) sin(alfa) + (y1 - y0) cos(alfa)
Coord virtual_to_local(Coord virt, int window_width, int window_height) {
    Coord l;
    float dx = virt.x - VIRTUAL_X0;
    float dy = virt.y - VIRTUAL_Y0;
    
    l.x = dx * cos(VIRTUAL_ALFA) + dy * sin(VIRTUAL_ALFA);
    l.y = -dx * sin(VIRTUAL_ALFA) + dy * cos(VIRTUAL_ALFA);
    
    return l;
}

// Helper functions for socket protocol
int read_line(int sockfd, char *buffer, int max_len) {
    int i = 0;
    char c;
    while (i < max_len - 1) {
        int n = read(sockfd, &c, 1);
        if (n <= 0) return -1;
        if (c == '\n') break;
        buffer[i++] = c;
    }
    buffer[i] = '\0';
    return i;
}

int write_line(int sockfd, const char *message) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s\n", message);
    return write(sockfd, buffer, strlen(buffer));
}

int main(int argc, char *argv[]) {

    // Setup signal handling FIRST
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_terminate;
    sa.sa_flags = 0; // Restart interrupted syscalls
    sigaction(SIGTERM, &sa, NULL);
    
    log_process("CommServer", getpid());
    logger_init("system.log",0);
    LOG_INFO("CommServer", "Starting Communication Server Process (PID=%d)", getpid());
          
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <sockfd> <fdComm_FromBB> <fdComm_ToBB> <width> <height>\n", argv[0]);
        return 1;
    }
    
    int listen_sockfd = atoi(argv[1]);
    int fdComm_FromBB = atoi(argv[2]);  // Read MY drone position from BB
    int fdComm_ToBB = atoi(argv[3]);    // Write CLIENT's position to BB
    int window_width = atoi(argv[4]);
    int window_height = atoi(argv[5]);

    LOG_INFO("CommServer", "Window size: %dx%d", window_width, window_height);
    LOG_INFO("CommServer", "Waiting for client connection...");
    //printf("Waiting for client connection...\n");
    
    struct sockaddr_in cli_addr;
    socklen_t clilen = sizeof(cli_addr);
    int newsockfd = -1;

    // --- Loop that waits for connection OR exit signal ---
    //Either we want to exit before client accepts or we get a client connection
    //using a select to check
    while (!should_exit) {
        fd_set readfds;
        struct timeval tv;

        FD_ZERO(&readfds);
        FD_SET(listen_sockfd, &readfds);

        tv.tv_sec = 1; // Check for 'q' every 1 second
        tv.tv_usec = 0;

        // Wait to see if a client is knocking
        int activity = select(listen_sockfd + 1, &readfds, NULL, NULL, &tv);

        if (activity > 0) {
            // A client is waiting! Safe to call accept() now without blocking.
            newsockfd = accept(listen_sockfd, (struct sockaddr *)&cli_addr, &clilen);
            break; // Exit the wait loop and proceed to handshake
        } else if (activity == -1) {
            // Error (likely interrupted by signal 'q')
            if (errno != EINTR) LOG_ERRNO("CommServer", "Select error");
        }
        // If activity == 0 (Timeout), loop repeats and checks 'should_exit'
    }

    // --- CHECK: Did we exit the loop because of 'q'? ---
    if (should_exit) {
        LOG_INFO("CommServer", "Exiting while waiting for client.");
        close(listen_sockfd); // Clean up
        logger_close();
        return 0; // Terminate immediately
    }

    if (newsockfd < 0) {
        LOG_ERRNO("CommServer", "ERROR on accept");
        return 1;
    }

    
    LOG_INFO("CommServer", "Client connected!");
    char buffer[256];
    // --- SET TIMEOUT ---
    struct timeval tv;
    tv.tv_sec = 1;  // 1 Second timeout
    tv.tv_usec = 0;
    setsockopt(newsockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
 
        
    // PROTOCOL: Initial handshake
    // 1. Send "ok", wait for "ook"
    write_line(newsockfd, "ServerConnected");
    if (read_line(newsockfd, buffer, sizeof(buffer)) < 0 || strcmp(buffer, "ClientConnected") != 0) {
        LOG_ERROR("CommServer", "Protocol error: expected 'ClientConnected', got '%s'", buffer);
        close(newsockfd);
        return 1;
    }
    LOG_INFO("CommServer", "Received connection acknowledgment from client");
    
    // 2. Send "size w h", wait for "sok"
    snprintf(buffer, sizeof(buffer), "size %d %d", window_width, window_height);
    write_line(newsockfd, buffer);
    LOG_INFO("CommServer", "Sent: %s", buffer);
    
    if (read_line(newsockfd, buffer, sizeof(buffer)) < 0 || strcmp(buffer, "w_h") != 0) {
        LOG_ERROR("CommServer", "Protocol error: expected 'w_h', got '%s'", buffer);
        close(newsockfd);
        return 1;
    }
    LOG_INFO("CommServer", "Received window size acknowledgment from client");
    
    LOG_INFO("CommServer", "Handshake complete. Entering main loop...");
    
    // MAIN LOOP
    bool running = true;
    int loop_count = 0;
    
    while (running) {

        // Check for termination signal from master
        if (should_exit) {
            LOG_INFO("Input", "Termination signal received. Exiting main loop.");
            break;
        }
        loop_count++;
        
        // a) Send "drone" command
        if (write_line(newsockfd, "drone") < 0) {
            LOG_ERROR("CommServer", "Write error on 'drone'");
            break;
        }
        
        // Read MY drone position from BlackBoard (format: "x.x,y.y")
        char my_pos[50];
        ssize_t bytes = read(fdComm_FromBB, my_pos, sizeof(my_pos) - 1);
        if (bytes <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);
                continue;
            }
            LOG_ERROR("CommServer", "Failed to read from BlackBoard");
            break;
        }
        my_pos[bytes] = '\0';
        
        // Parse local coordinates (format: "x.x,y.y")
        Coord local;
        if (sscanf(my_pos, "%f,%f", &local.x, &local.y) != 2) {
            LOG_ERROR("CommServer", "Invalid format from BlackBoard: '%s'", my_pos);
            continue;
        }
        
        // Convert to virtual coordinates
        Coord virtual = local_to_virtual(local, window_width, window_height);
        
        // Send position in virtual coordinates (format: "x.x y.y" - note space, not comma)
        snprintf(buffer, sizeof(buffer), "%.1f %.1f", virtual.x, virtual.y);
        if (write_line(newsockfd, buffer) < 0) {
            LOG_ERROR("CommServer", "Write error on position");
            break;
        }
        
        if (loop_count % 20 == 0) {
            LOG_INFO("CommServer", "Sent drone pos: local(%.1f,%.1f) -> virtual(%.1f,%.1f)",
                   local.x, local.y, virtual.x, virtual.y);
        }
        

        // Wait for "drone_ok"
        if (read_line(newsockfd, buffer, sizeof(buffer)) < 0 || strcmp(buffer, "drone_ok") != 0) {
            // Check for termination signal from master
            if (should_exit) {
                LOG_INFO("Input", "Termination signal received. Exiting main loop.");
                break;
            }
            
            LOG_ERROR("CommServer", "Protocol error: expected 'drone_ok', got '%s'", buffer);
            break;
        }
        
        // b) Send "obstacle_ok" command
        if (write_line(newsockfd, "obstacle_ok") < 0) {
            LOG_ERROR("CommServer", "Write error on 'obstacle_ok'");
            break;
        }
        
    
        // Wait for client position in virtual coordinates (format: "x.x y.y")
        if (read_line(newsockfd, buffer, sizeof(buffer)) < 0) {
            // Check for termination signal from master
            if (should_exit) {
                LOG_INFO("Input", "Termination signal received. Exiting main loop.");
                break;
            }
                
            LOG_ERROR("CommServer", "Read error on client position");
            break;
        }
        
        // Parse virtual coordinates
        Coord client_virtual;
        if (sscanf(buffer, "%f %f", &client_virtual.x, &client_virtual.y) != 2) {
            LOG_ERROR("CommServer", "Invalid client position format: '%s'", buffer);
            // Send pok anyway to keep protocol in sync
            write_line(newsockfd, "position_ok");
            continue;
        }
        
        // Convert to local coordinates
        Coord client_local = virtual_to_local(client_virtual, window_width, window_height);
        
        // Send "position_ok" acknowledgement
        if (write_line(newsockfd, "position_ok") < 0) {
            LOG_ERROR("CommServer", "Write error on 'position_ok'");
            break;
        }
        
        // Write client position to BlackBoard in local coordinates (format: "x.x,y.y")
        char client_pos_str[50];
        snprintf(client_pos_str, sizeof(client_pos_str), "%.1f,%.1f", client_local.x, client_local.y);
        write(fdComm_ToBB, client_pos_str, strlen(client_pos_str) + 1);
        
        if (loop_count % 20 == 0) {
            LOG_INFO("CommServer", "Received client: virtual(%.1f,%.1f) -> local(%.1f,%.1f)",
                   client_virtual.x, client_virtual.y, client_local.x, client_local.y);
        }
        
        // Small delay
        usleep(50000); // 50ms
        
       
    }

    // --- READ (Will now fail automatically after 1s) ---
    // If read_line returns < 0, it means it timed out or failed
    // TERMINATION
    LOG_INFO("CommServer", "Sending quit signal");
    write_line(newsockfd, "quit");
    if (read_line(newsockfd, buffer, sizeof(buffer)) > 0) {
        if (strcmp(buffer, "quit_ok") == 0) {
            LOG_INFO("CommServer", "Clean shutdown acknowledged");
        } else {
            LOG_WARNING("CommServer", "Client replied with '%s' instead of 'quit_ok'", buffer);
        }
    } else {
        // This runs if the client is dead/silent for > 1 second
        LOG_WARNING("CommServer", "Client did not reply (Timeout). Closing anyway.");
    }

    close(newsockfd);
    close(listen_sockfd);
    close(fdComm_FromBB);
    close(fdComm_ToBB);
    
    logger_close();
    return 0;
}