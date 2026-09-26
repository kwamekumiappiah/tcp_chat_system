#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#define MAX_SIZE 1024
#define MAX_CLIENTS 100

// Internal Encapsulated State (Private to server.c) 🔒
static int clients[MAX_CLIENTS];
static int client_count = 0;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static int server_fd = -1;

// Private Helper Functions ---------------------------------------------------

/**
 * @brief Adds a new client socket descriptor to the global tracking array.
 *
 * @param client_fd File descriptor of the newly connected client socket.
 *
 * @return void
 *
 * @details Thread-Safety & Calls:
 *          - Acquires `clients_mutex` via pthread_mutex_lock().
 *          - Appends `client_fd` to `clients[]` if capacity allows.
 *          - Releases `clients_mutex` via pthread_mutex_unlock().
 */
static void add_client(int client_fd) {
    pthread_mutex_lock(&clients_mutex);
    if (client_count < MAX_CLIENTS) {
        clients[client_count++] = client_fd;
    }
    pthread_mutex_unlock(&clients_mutex);
}

/**
 * @brief Removes a client socket descriptor using an O(1) swap-and-pop strategy.
 *
 * @param client_fd File descriptor of the client leaving the server.
 *
 * @return void
 *
 * @details Thread-Safety & Calls:
 *          - Acquires `clients_mutex` via pthread_mutex_lock().
 *          - Searches `clients[]`, replaces target with last element, and decrements `client_count`.
 *          - Releases `clients_mutex` via pthread_mutex_unlock().
 */
static void remove_client(int client_fd) {
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

/**
 * @brief Broadcasts a message string to all connected clients except the sender.
 *
 * @param message Null-terminated string buffer containing the formatted message.
 * @param sender_fd Socket descriptor of sender (excluded from broadcast), or -1 for server alerts.
 *
 * @return void
 *
 * @details Thread-Safety & Calls:
 *          - Acquires `clients_mutex` via pthread_mutex_lock().
 *          - Calls write() for each matching active socket descriptor.
 *          - Releases `clients_mutex` via pthread_mutex_unlock().
 */
static void send_to_all(char *message, int sender_fd) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i] != sender_fd) {
            write(clients[i], message, strlen(message));
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

/**
 * @brief Signal handler callback triggered when SIGINT (Ctrl+C) is received.
 *
 * @param sig Signal number received (e.g., SIGINT).
 *
 * @return void (Terminates process via exit(0)).
 *
 * @details Key Functions Called:
 *          - stop_server() to close sockets and release mutex resources.
 *          - exit(0) to terminate process cleanly.
 */
static void handle_sigint(int sig) {
    stop_server();
    exit(0);
}

/**
 * @brief Manages the active read loop for a connected client.
 *
 * Continuously reads data from the socket, formats it with the client's name,
 * prints it to the server console, and broadcasts it to all peers.
 *
 * @param client_fd Socket file descriptor for receiving messages.
 * @param name Null-terminated string containing client's display name.
 *
 * @return void
 *
 * @details Key Functions Called:
 *          - read() to capture raw socket input.
 *          - snprintf() to safely construct string buffers.
 *          - send_to_all() to broadcast messages to peers.
 */
static void chat_client(int client_fd, char *name) {
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

/**
 * @brief Entry point function executed by each spawned POSIX thread.
 *
 * Controls full client connection lifecycle: prompting for name, announcing arrival,
 * delegating to chat_client(), and performing orderly cleanup upon disconnection.
 *
 * @param arg Dynamically allocated pointer to client socket descriptor (int *).
 *
 * @return NULL on thread termination.
 *
 * @details Key Functions Called:
 *          - write(), read(), strcspn() to capture and sanitize name.
 *          - add_client(), remove_client() to update global tracking array.
 *          - send_to_all() for join and leave announcements.
 *          - chat_client() for core messaging session.
 *          - close(), free() for resource deallocation.
 */
static void *handle_client(void *arg) {
    int client_fd = *((int *)arg);
    free(arg);

    char name[32];
    char welcome_msg[] = "Enter your name: ";

    write(client_fd, welcome_msg, strlen(welcome_msg));

    ssize_t bytes = read(client_fd, name, sizeof(name) - 1);
    if (bytes <= 0) {
        close(client_fd);
        return NULL;
    }
    name[bytes] = '\0';
    name[strcspn(name, "\r\n")] = '\0';

    add_client(client_fd);

    char join_msg[MAX_SIZE];
    snprintf(join_msg, sizeof(join_msg), "%s joined the chat!\n", name);
    printf("%s", join_msg);
    send_to_all(join_msg, client_fd);

    chat_client(client_fd, name);

    remove_client(client_fd);

    char leave_msg[MAX_SIZE];
    snprintf(leave_msg, sizeof(leave_msg), "%s left the chat.\n", name);
    printf("%s", leave_msg);
    send_to_all(leave_msg, client_fd);

    close(client_fd);
    return NULL;
}

// Public API Functions -------------------------------------------------------

void stop_server(void) {
    printf("\nShutting down server safely...\n");

    pthread_mutex_lock(&clients_mutex);
    char *msg = "Server is shutting down...\n";
    for (int i = 0; i < client_count; i++) {
        write(clients[i], msg, strlen(msg));
        close(clients[i]);
    }
    client_count = 0;
    pthread_mutex_unlock(&clients_mutex);

    pthread_mutex_destroy(&clients_mutex);

    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }

    printf("Server stopped cleanly.\n");
}

int start_server(int port) {
    signal(SIGINT, handle_sigint);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 5) < 0) {
        perror("Listen failed");
        close(server_fd);
        return -1;
    }

    printf("Chat server running on port %d...\n", port);

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