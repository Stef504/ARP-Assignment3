#define _POSIX_C_SOURCE 200809L
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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Transformation parameters
#define VIRTUAL_X0 0.0
#define VIRTUAL_Y0 0.0
#define VIRTUAL_ALFA 0.0 // Angle in radians

volatile sig_atomic_t terminate_flag = 0;

void handle_signal(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        terminate_flag = 1;
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
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <sockfd> <fdComm_FromBB> <fdComm_ToBB> <width> <height>\n", argv[0]);
        return 1;
    }
    
    int listen_sockfd = atoi(argv[1]);
    int fdComm_FromBB = atoi(argv[2]);  // Read MY drone position from BB
    int fdComm_ToBB = atoi(argv[3]);    // Write CLIENT's position to BB
    int window_width = atoi(argv[4]);
    int window_height = atoi(argv[5]);
    
    printf("[COMM SERVER] Window size: %dx%d\n", window_width, window_height);
    printf("[COMM SERVER] Waiting for client connection...\n");
    
    // Accept connection
    struct sockaddr_in cli_addr;
    socklen_t clilen = sizeof(cli_addr);
    int newsockfd = accept(listen_sockfd, (struct sockaddr *)&cli_addr, &clilen);
    if (newsockfd < 0) {
        perror("ERROR on accept");
        return 1;
    }
    
    printf("[COMM SERVER] Client connected!\n");
    
    // Setup signal handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    char buffer[256];
    
    // PROTOCOL: Initial handshake
    // 1. Send "ok", wait for "ook"
    write_line(newsockfd, "ok");
    if (read_line(newsockfd, buffer, sizeof(buffer)) < 0 || strcmp(buffer, "ook") != 0) {
        fprintf(stderr, "Protocol error: expected 'ook', got '%s'\n", buffer);
        close(newsockfd);
        return 1;
    }
    printf("[COMM SERVER] Received: ook\n");
    
    // 2. Send "size w h", wait for "sok"
    snprintf(buffer, sizeof(buffer), "size %d %d", window_width, window_height);
    write_line(newsockfd, buffer);
    printf("[COMM SERVER] Sent: %s\n", buffer);
    
    if (read_line(newsockfd, buffer, sizeof(buffer)) < 0 || strcmp(buffer, "sok") != 0) {
        fprintf(stderr, "Protocol error: expected 'sok', got '%s'\n", buffer);
        close(newsockfd);
        return 1;
    }
    printf("[COMM SERVER] Received: sok\n");
    
    printf("[COMM SERVER] Handshake complete. Entering main loop...\n");
    
    // MAIN LOOP
    bool running = true;
    int loop_count = 0;
    
    while (running && !terminate_flag) {
        loop_count++;
        
        // a) Send "drone" command
        if (write_line(newsockfd, "drone") < 0) {
            fprintf(stderr, "Write error on 'drone'\n");
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
            fprintf(stderr, "Failed to read from BlackBoard\n");
            break;
        }
        my_pos[bytes] = '\0';
        
        // Parse local coordinates (format: "x.x,y.y")
        Coord local;
        if (sscanf(my_pos, "%f,%f", &local.x, &local.y) != 2) {
            fprintf(stderr, "Invalid format from BlackBoard: '%s'\n", my_pos);
            continue;
        }
        
        // Convert to virtual coordinates
        Coord virtual = local_to_virtual(local, window_width, window_height);
        
        // Send position in virtual coordinates (format: "x.x y.y" - note space, not comma)
        snprintf(buffer, sizeof(buffer), "%.1f %.1f", virtual.x, virtual.y);
        if (write_line(newsockfd, buffer) < 0) {
            fprintf(stderr, "Write error on position\n");
            break;
        }
        
        if (loop_count % 20 == 0) {
            printf("[COMM SERVER] Sent drone pos: local(%.1f,%.1f) -> virtual(%.1f,%.1f)\n",
                   local.x, local.y, virtual.x, virtual.y);
        }
        
        // Wait for "dok"
        if (read_line(newsockfd, buffer, sizeof(buffer)) < 0 || strcmp(buffer, "dok") != 0) {
            fprintf(stderr, "Protocol error: expected 'dok', got '%s'\n", buffer);
            break;
        }
        
        // b) Send "obst" command
        if (write_line(newsockfd, "obst") < 0) {
            fprintf(stderr, "Write error on 'obst'\n");
            break;
        }
        
        // Wait for client position in virtual coordinates (format: "x.x y.y")
        if (read_line(newsockfd, buffer, sizeof(buffer)) < 0) {
            fprintf(stderr, "Read error on client position\n");
            break;
        }
        
        // Parse virtual coordinates
        Coord client_virtual;
        if (sscanf(buffer, "%f %f", &client_virtual.x, &client_virtual.y) != 2) {
            fprintf(stderr, "Invalid client position format: '%s'\n", buffer);
            // Send pok anyway to keep protocol in sync
            write_line(newsockfd, "pok");
            continue;
        }
        
        // Convert to local coordinates
        Coord client_local = virtual_to_local(client_virtual, window_width, window_height);
        
        // Send "pok" acknowledgement
        if (write_line(newsockfd, "pok") < 0) {
            fprintf(stderr, "Write error on 'pok'\n");
            break;
        }
        
        // Write client position to BlackBoard in local coordinates (format: "x.x,y.y")
        char client_pos_str[50];
        snprintf(client_pos_str, sizeof(client_pos_str), "%.1f,%.1f", client_local.x, client_local.y);
        write(fdComm_ToBB, client_pos_str, strlen(client_pos_str) + 1);
        
        if (loop_count % 20 == 0) {
            printf("[COMM SERVER] Received client: virtual(%.1f,%.1f) -> local(%.1f,%.1f)\n",
                   client_virtual.x, client_virtual.y, client_local.x, client_local.y);
        }
        
        // Small delay
        usleep(50000); // 50ms
        
        // Check for quit signal from BlackBoard (via SIGTERM or pipe close)
        if (terminate_flag) {
            printf("[COMM SERVER] Termination signal received.\n");
            break;
        }
    }
    
    // TERMINATION
    printf("[COMM SERVER] Sending quit signal\n");
    write_line(newsockfd, "q");
    read_line(newsockfd, buffer, sizeof(buffer));
    if (strcmp(buffer, "qok") == 0) {
        printf("[COMM SERVER] Clean shutdown\n");
    }
    
    close(newsockfd);
    close(listen_sockfd);
    close(fdComm_FromBB);
    close(fdComm_ToBB);
    
    return 0;
}