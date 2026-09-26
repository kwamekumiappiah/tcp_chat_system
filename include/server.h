#ifndef SERVER_H
#define SERVER_H

/**
 * @brief Initializes and starts the TCP chat server listening loop.
 *
 * Configures the server socket, sets socket options (SO_REUSEADDR), binds
 * to the specified port, and enters an infinite loop accepting incoming
 * client connections. Spawns a detached thread for each client.
 *
 * @param port The TCP port number on which the server will listen (e.g., 8080).
 *
 * @return Returns 0 on clean exit, or -1 if socket creation, binding, or listening fails.
 *
 * @details Key Functions Called:
 *          - socket(), setsockopt(), bind(), listen(), accept()
 *          - signal() to register SIGINT handler
 *          - pthread_create(), pthread_detach() to handle clients concurrently
 */
int start_server(int port);

/**
 * @brief Safely shuts down the chat server and reclaims allocated resources.
 *
 * Locks the client mutex, notifies all connected sockets with a shutdown message,
 * closes all client socket descriptors, destroys the client mutex, and closes
 * the primary server listening socket.
 *
 * @param void
 *
 * @return void
 *
 * @details Key Functions Called:
 *          - pthread_mutex_lock(), pthread_mutex_unlock(), pthread_mutex_destroy()
 *          - write(), close()
 */
void stop_server(void);

#endif // SERVER_H