#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

// Virtual coordinate system (top-left origin)
typedef struct {
    int x;
    int y;
} VirtualCoord;

typedef struct {
    int x;
    int y;
} LocalCoord;

// Convert local to virtual (adjust if your coordinates differ)
VirtualCoord local_to_virtual(LocalCoord local, int window_width, int window_height) {
    VirtualCoord v;
    v.x = local.x;
    v.y = local.y;
    // If your origin is different (e.g., bottom-left), adjust here:
    // v.y = window_height - local.y;
    return v;
}

// Convert virtual to local
LocalCoord virtual_to_local(VirtualCoord virt, int window_width, int window_height) {
    LocalCoord l;
    l.x = virt.x;
    l.y = virt.y;
    // If your origin is different (e.g., bottom-left), adjust here:
    // l.y = window_height - virt.y;
    return l;
}

// Read a line from socket (string messages)
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

// Write a line to socket
int write_line(int sockfd, const char *message) {
    int len = strlen(message);
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s\n", message);
    return write(sockfd, buffer, strlen(buffer));
}


// ============= CLIENT COMMUNICATION PROCESS =============
void communication_client(const char *hostname, int portno, 
                          int pipe_from_bb, int pipe_to_bb,
                          int *window_width, int *window_height) {
    
    // Setup socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("ERROR opening socket");
        exit(1);
    }
    
    struct hostent *server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr, "ERROR, no such host\n");
        exit(1);
    }
    
    struct sockaddr_in serv_addr;
    bzero(&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy(server->h_addr, &serv_addr.sin_addr.s_addr, server->h_length);
    serv_addr.sin_port = htons(portno);
    
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("ERROR connecting");
        exit(1);
    }
    
    printf("[COMM CLIENT] Connected to server!\n");
    
    // PROTOCOL START
    char buffer[256];
    
    // 1. Wait for "ok", send "ook"
    read_line(sockfd, buffer, sizeof(buffer));
    if (strcmp(buffer, "ok") != 0) {
        fprintf(stderr, "Protocol error: expected 'ok'\n");
        close(sockfd);
        return;
    }
    write_line(sockfd, "ook");
    
    // 2. Wait for "size w h", send "sok"
    read_line(sockfd, buffer, sizeof(buffer));
    int w, h;
    sscanf(buffer, "size %d %d", &w, &h);
    *window_width = w;
    *window_height = h;
    write_line(sockfd, "sok");
    
    printf("[COMM CLIENT] Window size: %dx%d\n", w, h);
    
    // 3. MAIN LOOP
    int should_quit = 0;
    while (!should_quit) {
        
        // a) Receive drone position (server's drone)
        read_line(sockfd, buffer, sizeof(buffer));
        if (strcmp(buffer, "drone") != 0) {
            fprintf(stderr, "Protocol error: expected 'drone'\n");
            break;
        }
        
        read_line(sockfd, buffer, sizeof(buffer));
        VirtualCoord v_server_drone;
        sscanf(buffer, "%d %d", &v_server_drone.x, &v_server_drone.y);
        
        write_line(sockfd, "dok");
        
        // Convert to local and write to blackboard (for DISPLAY ONLY - no repulsion)
        LocalCoord l_server_drone = virtual_to_local(v_server_drone, w, h);
        write(pipe_to_bb, &l_server_drone, sizeof(LocalCoord));
        
        // b) Send obstacle (MY drone position to server)
        read_line(sockfd, buffer, sizeof(buffer));
        if (strcmp(buffer, "obst") != 0) {
            fprintf(stderr, "Protocol error: expected 'obst'\n");
            break;
        }
        
        // Read MY drone position from blackboard
        LocalCoord my_drone;
        int n = read(pipe_from_bb, &my_drone, sizeof(LocalCoord));
        if (n < 0) {
            perror("Error reading from blackboard");
            break;
        }
        
        // Convert to virtual
        VirtualCoord v_my_drone = local_to_virtual(my_drone, w, h);
        
        // Send position
        sprintf(buffer, "%d %d", v_my_drone.x, v_my_drone.y);
        write_line(sockfd, buffer);
        
        // Wait for "pok"
        read_line(sockfd, buffer, sizeof(buffer));
        if (strcmp(buffer, "pok") != 0) {
            fprintf(stderr, "Protocol error: expected 'pok'\n");
            break;
        }
        
        // c) Check for quit from server
        read_line(sockfd, buffer, sizeof(buffer));
        if (strcmp(buffer, "q") == 0) {
            write_line(sockfd, "qok");
            should_quit = 1;
        }
        
        usleep(50000); // 50ms
    }
    
    printf("[COMM CLIENT] Connection closed\n");
    close(sockfd);
}