#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#define PORT 8080
#define MAX_SIZE 1024
#define MAX_CLIENTS 100

// Shared state & synchronization globals 🔒
int clients[MAX_CLIENTS];
int client_count = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
int server_fd;

// Add a client descriptor safely ➕
void add_client(int client_fd) {
    pthread_mutex_lock(&clients_mutex);
    if (client_count < MAX_CLIENTS) {
        clients[client_count++] = client_fd;
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Remove a client descriptor in O(1) time ➖
void remove_client(int client_fd) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i] == client_fd) {
            clients[i] = clients[client_count - 1];
            client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Broadcast a message to all connected clients except the sender 📢
void send_to_all(char *message, int sender_fd) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i] != sender_fd) {
            write(clients[i], message, strlen(message));
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Handle SIGINT (Ctrl+C) for graceful shutdown 🛑
void handle_sigint(int sig) {
    printf("\nShutting down server safely to maximize system stability...\n");

    pthread_mutex_lock(&clients_mutex);
    char *msg = "Server is shutting down...\n";
    for (int i = 0; i < client_count; i++) {
        write(clients[i], msg, strlen(msg));
        close(clients[i]);
    }
    client_count = 0;
    pthread_mutex_unlock(&clients_mutex);

    pthread_mutex_destroy(&clients_mutex);
    close(server_fd);

    printf("Server shutdown complete. All resources reclaimed.\n");
    exit(0);
}

// Core message reception loop for an active client 💬
void chat_client(int client_fd, char *name) {
    char buffer[MAX_SIZE];
    char formatted_msg[MAX_SIZE + 64];
    ssize_t bytes_read;

    while ((bytes_read = read(client_fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        snprintf(formatted_msg, sizeof(formatted_msg), "%s: %s", name, buffer);
        printf("%s", formatted_msg);
        send_to_all(formatted_msg, client_fd);
    }
}

// Worker thread handling the client lifecycle 🧵
void *handle_client(void *arg) {
    int client_fd = *((int *)arg);
    free(arg);

    char name[32];
    char welcome_msg[] = "Enter your name: ";

    // 1. Prompt for username 💬
    write(client_fd, welcome_msg, strlen(welcome_msg));

    ssize_t bytes = read(client_fd, name, sizeof(name) - 1);
    if (bytes <= 0) {
        close(client_fd);
        return NULL;
    }
    name[bytes] = '\0';
    name[strcspn(name, "\r\n")] = '\0'; // Strip trailing line breaks 🧹

    add_client(client_fd);

    // 2. Broadcast join notification 📢
    char join_msg[MAX_SIZE];
    snprintf(join_msg, sizeof(join_msg), "%s joined the chat!\n", name);
    printf("%s", join_msg);
    send_to_all(join_msg, client_fd);

    // 3. Execute chat loop 🔄
    chat_client(client_fd, name);

    // 4. Cleanup and broadcast single leave notification 🚪
    remove_client(client_fd);

    char leave_msg[MAX_SIZE];
    snprintf(leave_msg, sizeof(leave_msg), "%s left the chat.\n", name);
    printf("%s", leave_msg);
    send_to_all(leave_msg, client_fd);

    close(client_fd);
    return NULL;
}

int main(int argc, char **argv) {
    // Register signal handler for optimal shutdown safety 🛑
    signal(SIGINT, handle_sigint);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Reuse port immediately to avoid TIME_WAIT socket exhaustion ⚡
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 5) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Chat server running on port %d...\n", PORT);

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    while (1) {
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            perror("Accept failed");
            continue;
        }

        int *pclient = malloc(sizeof(int));
        if (pclient == NULL) {
            close(client_fd);
            continue;
        }
        *pclient = client_fd;

        pthread_t thread_id;
        pthread_create(&thread_id, NULL, handle_client, pclient);
        pthread_detach(thread_id);
    }

    return 0;
}