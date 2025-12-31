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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Transformation parameters
#define VIRTUAL_X0 0.0
#define VIRTUAL_Y0 0.0
#define VIRTUAL_ALFA 0.0 

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
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <hostname> <port> <fdComm_FromBB> <fdComm_ToBB>\n", argv[0]);
        return 1;
    }
    
    char *hostname = argv[1];
    int portno = atoi(argv[2]);
    int fdComm_FromBB = atoi(argv[3]);  // Read MY drone position from BB
    int fdComm_ToBB = atoi(argv[4]);    // Write SERVER's position to BB
    
    printf("[COMM CLIENT] Connecting to %s:%d\n", hostname, portno);
    
    // Setup socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("ERROR opening socket");
        return 1;
    }
    
    struct hostent *server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr, "ERROR, no such host: %s\n", hostname);
        return 1;
    }
    
    struct sockaddr_in serv_addr;
    bzero(&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy(server->h_addr, &serv_addr.sin_addr.s_addr, server->h_length);
    serv_addr.sin_port = htons(portno);
    
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("ERROR connecting");
        return 1;
    }
    
    printf("[COMM CLIENT] Connected to server!\n");
    
    char buffer[256];
    int window_width, window_height;
    
    // PROTOCOL: Initial handshake
    // 1. Wait for "ok", send "ook"
    if (read_line(sockfd, buffer, sizeof(buffer)) < 0 || strcmp(buffer, "ok") != 0) {
        fprintf(stderr, "Protocol error: expected 'ok', got '%s'\n", buffer);
        close(sockfd);
        return 1;
    }
    printf("[COMM CLIENT] Received: ok\n");
    write_line(sockfd, "ook");
    printf("[COMM CLIENT] Sent: ook\n");
    
    // 2. Wait for "size w h", send "sok"
    if (read_line(sockfd, buffer, sizeof(buffer)) < 0) {
        fprintf(stderr, "Read error on size\n");
        close(sockfd);
        return 1;
    }
    
    if (sscanf(buffer, "size %d %d", &window_width, &window_height) != 2) {
        fprintf(stderr, "Protocol error: invalid size format '%s'\n", buffer);
        close(sockfd);
        return 1;
    }
    printf("[COMM CLIENT] Received window size: %dx%d\n", window_width, window_height);
    
    write_line(sockfd, "sok");
    printf("[COMM CLIENT] Sent: sok\n");
    
    printf("[COMM CLIENT] Handshake complete. Entering main loop...\n");
    
    // MAIN LOOP
    bool running = true;
    int loop_count = 0;
    
    while (running) {
        loop_count++;
        
        // a) Wait for "drone" or "q" command
        if (read_line(sockfd, buffer, sizeof(buffer)) < 0) {
            fprintf(stderr, "Read error\n");
            break;
        }
        
        // Check for quit signal
        if (strcmp(buffer, "q") == 0) {
            printf("[COMM CLIENT] Received quit signal\n");
            write_line(sockfd, "qok");
            running = false;
            break;
        }
        
        if (strcmp(buffer, "drone") != 0) {
            fprintf(stderr, "Protocol error: expected 'drone' or 'q', got '%s'\n", buffer);
            break;
        }
        
        // Wait for server position in virtual coordinates (format: "x.x y.y")
        if (read_line(sockfd, buffer, sizeof(buffer)) < 0) {
            fprintf(stderr, "Read error on server position\n");
            break;
        }
        
        // Parse virtual coordinates
        Coord server_virtual;
        if (sscanf(buffer, "%f %f", &server_virtual.x, &server_virtual.y) != 2) {
            fprintf(stderr, "Invalid server position format: '%s'\n", buffer);
            // Send dok anyway to keep protocol in sync
            write_line(sockfd, "dok");
            continue;
        }
        
        // Convert to local coordinates
        Coord server_local = virtual_to_local(server_virtual, window_width, window_height);
        
        // Send "dok" acknowledgement
        if (write_line(sockfd, "dok") < 0) {
            fprintf(stderr, "Write error on 'dok'\n");
            break;
        }
        
        // Write server position to BlackBoard in local coordinates (format: "x.x,y.y")
        char server_pos_str[50];
        snprintf(server_pos_str, sizeof(server_pos_str), "%.1f,%.1f", server_local.x, server_local.y);
        write(fdComm_ToBB, server_pos_str, strlen(server_pos_str) + 1);
        
        if (loop_count % 20 == 0) {
            printf("[COMM CLIENT] Received server: virtual(%.1f,%.1f) -> local(%.1f,%.1f)\n",
                   server_virtual.x, server_virtual.y, server_local.x, server_local.y);
        }
        
        // b) Wait for "obst" command
        if (read_line(sockfd, buffer, sizeof(buffer)) < 0) {
            fprintf(stderr, "Read error\n");
            break;
        }
        
        if (strcmp(buffer, "obst") != 0) {
            fprintf(stderr, "Protocol error: expected 'obst', got '%s'\n", buffer);
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
        if (write_line(sockfd, buffer) < 0) {
            fprintf(stderr, "Write error on position\n");
            break;
        }
        
        if (loop_count % 20 == 0) {
            printf("[COMM CLIENT] Sent my pos: local(%.1f,%.1f) -> virtual(%.1f,%.1f)\n",
                   local.x, local.y, virtual.x, virtual.y);
        }
        
        // Wait for "pok"
        if (read_line(sockfd, buffer, sizeof(buffer)) < 0 || strcmp(buffer, "pok") != 0) {
            fprintf(stderr, "Protocol error: expected 'pok', got '%s'\n", buffer);
            break;
        }
        
        // Small delay
        usleep(50000); // 50ms
    }
    
    printf("[COMM CLIENT] Connection closed\n");
    
    close(sockfd);
    close(fdComm_FromBB);
    close(fdComm_ToBB);
    
    return 0;
}