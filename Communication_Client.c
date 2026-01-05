#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
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

// Transformation parameters
#define VIRTUAL_X0 0.0
#define VIRTUAL_Y0 0.0
#define VIRTUAL_ALFA 0.0 

//sig_atomic_t ensures atomic access during signal handling
volatile sig_atomic_t should_exit = 0;

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
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout - check if we should exit
                if (should_exit) return -2;  // Special code for termination
                continue;  // Otherwise retry
            }
            return -1;  // Real error
        }
        if (n == 0) return -1;  // Connection closed
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


//termination handler from master process
void handle_terminate(int signo) {
    if (signo == SIGTERM) {
        should_exit = 1;
    }
}

int main(int argc, char *argv[]) {

    // Setup signal handling FIRST
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_terminate;
    sigaction(SIGTERM, &sa, NULL);

    // Initialize logger and log self
    log_process("CommClient", getpid());
    logger_init("system.log",0);
    

    if (argc != 5) {
        fprintf(stderr, "Usage: %s <hostname> <port> <fdComm_FromBB> <fdComm_ToBB>\n", argv[0]);
        return 1;
    }
    
    char *hostname = argv[1];
    int portno = atoi(argv[2]);
    int fdComm_FromBB = atoi(argv[3]);  // Read MY drone position from BB
    int fdComm_ToBB = atoi(argv[4]);    // Write SERVER's position to BB
    
    LOG_INFO("CommClient", "Connecting to %s:%d", hostname, portno);
    
    // Setup socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        LOG_ERRNO("CommClient", "ERROR opening socket");
        return 1;
    }
    
    struct hostent *server = gethostbyname(hostname);
    if (server == NULL) {
        LOG_ERROR("CommClient", "ERROR, no such host: %s", hostname);
        return 1;
    }
    
    struct sockaddr_in serv_addr;
    bzero(&serv_addr, sizeof(serv_addr));
    //internet test
    serv_addr.sin_family = AF_INET;
    bcopy(server->h_addr, &serv_addr.sin_addr.s_addr, server->h_length);
    serv_addr.sin_port = htons(portno);
    
   int retries = 0;
    int max_retries = 5;  // 5 retries * 3 seconds = 15 seconds total
    printf("Attempting to connect to server at %s:%d...\n", hostname, portno);
    
    while (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        // 1. Capture the specific error immediately (usually ECONNREFUSED)
        int err_code = errno; 

        if (retries >= max_retries) {
            // Restore errno so LOG_ERRNO prints the correct reason for the final failure
            errno = err_code;
            LOG_ERRNO("CommClient", "Give up: Failed to connect after retries");
            printf("\n*** Unable to connect to server at %s:%d ***\n", hostname, portno);
            printf("*** Server may not be running. Shutting down... ***\n\n");
            close(sockfd);
            close(fdComm_FromBB);
            close(fdComm_ToBB);
            
            // Signal parent process to terminate all children
            kill(getppid(), SIGTERM);
            
            logger_close();
            return 1;
        }
        
        // 2. Print the ACTUAL error message (e.g., "Connection refused")
        LOG_WARNING("CommClient", "Connection failed: %s. Retrying in 3s... (Attempt %d/%d)", 
                    strerror(err_code), retries + 1, max_retries);
        printf("Connection failed: %s. Retrying in 3s... (Attempt %d/%d)\n",
               strerror(err_code), retries + 1, max_retries);
        
        sleep(3);
        retries++;
    }
    LOG_INFO("CommClient", "Connected to server!");
    
    // Set socket timeout so we can check for termination signals
    struct timeval tv;
    tv.tv_sec = 5;  // 5 second timeout for internet reliability
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    
    char buffer[256];
    int window_width, window_height;
    
    // PROTOCOL: Initial handshake
    // 1. Wait for "ok", send "ook"
    int ret;
    while ((ret = read_line(sockfd, buffer, sizeof(buffer))) == -2) {
        // Keep waiting but check for termination
        if (should_exit) {
            LOG_INFO("CommClient", "Termination during handshake, exiting.");
            close(sockfd);
            return 0;
        }
    }
    if (ret < 0 || strcmp(buffer, "ServerConnected") != 0) {
        LOG_ERROR("CommClient", "Protocol error: expected 'ServerConnected', got '%s'", buffer);
        close(sockfd);
        return 1;
    }
    LOG_INFO("CommClient", "Server connected");
    write_line(sockfd, "ClientConnected");
    LOG_INFO("CommClient", "Sent connection acknowledgment to server");
    
    // 2. Wait for "size w h", send "sok"
    while ((ret = read_line(sockfd, buffer, sizeof(buffer))) == -2) {
        if (should_exit) {
            LOG_INFO("CommClient", "Termination during handshake, exiting.");
            close(sockfd);
            return 0;
        }
    }
    if (ret < 0) {
        LOG_ERROR("CommClient", "Read error on size");
        close(sockfd);
        return 1;
    }
    
    if (sscanf(buffer, "size %d %d", &window_width, &window_height) != 2) {
        LOG_ERROR("CommClient", "Protocol error: invalid size format '%s'", buffer);
        close(sockfd);
        return 1;
    }
    LOG_INFO("CommClient", "Received window size: %dx%d", window_width, window_height);
    
    write_line(sockfd, "w_h");
    LOG_INFO("CommClient", "Sent window size acknowledgment to server");
    
    LOG_INFO("CommClient", "Handshake complete. Entering main loop...");
    
    // MAIN LOOP
    bool running = true;
    int loop_count = 0;
    
    // Keep track of last known position for non-blocking reads
    Coord last_local = {window_width / 2.0f, window_height / 2.0f};  // Default center
    
    while (running) {
        if (should_exit) {
            LOG_INFO("CommClient", "Termination signal received. Exiting main loop.");
            break;
        }

        loop_count++;
        
        // a) Wait for "drone" or "q" command
        int ret = read_line(sockfd, buffer, sizeof(buffer));
        if (ret == -2) {
            LOG_INFO("CommClient", "Termination during read, exiting.");
            break;
        }
        if (ret < 0) {
            LOG_ERROR("CommClient", "Read error");
            break;
        }
        
        // Check for quit signal
        if (strcmp(buffer, "quit") == 0) {
            LOG_INFO("CommClient", "Received quit signal");
            write_line(sockfd, "quit_ok");
            running = false;
            break;
        }
        
        if (strcmp(buffer, "drone") != 0) {
            LOG_ERROR("CommClient", "Protocol error: expected 'drone' or 'q', got '%s'", buffer);
            break;
        }
        
        // Wait for server position in virtual coordinates (format: "x.x y.y")
        ret = read_line(sockfd, buffer, sizeof(buffer));
        if (ret == -2) {
            LOG_INFO("CommClient", "Termination during read, exiting.");
            break;
        }
        if (ret < 0) {
            LOG_ERROR("CommClient", "Read error on server position");
            break;
        }
        
        // Parse virtual coordinates
        Coord server_virtual;
        if (sscanf(buffer, "%f %f", &server_virtual.x, &server_virtual.y) != 2) {
            LOG_ERROR("CommClient", "Invalid server position format: '%s'", buffer);
            // Send drone_ok anyway to keep protocol in sync
            write_line(sockfd, "drone_ok");
            continue;
        }
        
        // Convert to local coordinates
        Coord server_local = virtual_to_local(server_virtual, window_width, window_height);
        
        // Send "drone_ok" acknowledgement
        if (write_line(sockfd, "drone_ok") < 0) {
            LOG_ERROR("CommClient", "Write error on 'drone_ok'");
            break;
        }
        
        // Write server position to BlackBoard in local coordinates (format: "x.x,y.y")
        char server_pos_str[50];
        snprintf(server_pos_str, sizeof(server_pos_str), "%.1f,%.1f", server_local.x, server_local.y);
        write(fdComm_ToBB, server_pos_str, strlen(server_pos_str) + 1);
        
        if (loop_count % 20 == 0) {
            LOG_INFO("CommClient", "Received server: virtual(%.1f,%.1f) -> local(%.1f,%.1f)",
                   server_virtual.x, server_virtual.y, server_local.x, server_local.y);
        }
        
        // b) Wait for "obst" command
        ret = read_line(sockfd, buffer, sizeof(buffer));
        if (ret == -2) {
            LOG_INFO("CommClient", "Termination during read, exiting.");
            break;
        }
        if (ret < 0) {
            LOG_ERROR("CommClient", "Read error");
            break;
        }
        
        if (strcmp(buffer, "obstacle_ok") != 0) {
            LOG_ERROR("CommClient", "Protocol error: expected 'obstacle_ok', got '%s'", buffer);
            break;
        }
        
        // Read MY drone position from BlackBoard (format: "x.x,y.y")
        char my_pos[50];
        ssize_t bytes = read(fdComm_FromBB, my_pos, sizeof(my_pos) - 1);
        if (bytes > 0) {
            my_pos[bytes] = '\0';
            // Parse local coordinates (format: "x.x,y.y")
            if (sscanf(my_pos, "%f,%f", &last_local.x, &last_local.y) != 2) {
                LOG_ERROR("CommClient", "Invalid format from BlackBoard: '%s'", my_pos);
            }
        } else if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR("CommClient", "Failed to read from BlackBoard");
            break;
        }
        // If EAGAIN, just use last_local
        
        // Convert to virtual coordinates
        Coord virtual = local_to_virtual(last_local, window_width, window_height);
        
        // Send position in virtual coordinates (format: "x.x y.y" - note space, not comma)
        snprintf(buffer, sizeof(buffer), "%.1f %.1f", virtual.x, virtual.y);
        if (write_line(sockfd, buffer) < 0) {
            LOG_ERROR("CommClient", "Write error on position");
            break;
        }
        
        if (loop_count % 20 == 0) {
            LOG_INFO("CommClient", "Sent my pos: local(%.1f,%.1f) -> virtual(%.1f,%.1f)",
                   last_local.x, last_local.y, virtual.x, virtual.y);
        }
        
        // Wait for "position_ok"
        ret = read_line(sockfd, buffer, sizeof(buffer));
        if (ret == -2) {
            LOG_INFO("CommClient", "Termination during read, exiting.");
            break;
        }
        if (ret < 0 || strcmp(buffer, "position_ok") != 0) {
            LOG_ERROR("CommClient", "Protocol error: expected 'position_ok', got '%s'", buffer);
            break;
        }
        
        // Small delay
        usleep(50000); // 50ms
    }
    
    LOG_INFO("CommClient", "Connection closed");
    
    close(sockfd);
    close(fdComm_FromBB);
    close(fdComm_ToBB);
    
    logger_close();
    
    return 0;
}