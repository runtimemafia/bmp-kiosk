// server_communication.h
#ifndef SERVER_COMMUNICATION_H
#define SERVER_COMMUNICATION_H

#include <stddef.h>

// Initialize the module with server URL (e.g. "https://example.com/api/qr").
// Returns 0 on success.
int server_comm_init(const char *server_url);

// Send qr_data to server. Returns 0 on success, non-zero on error.
// This is blocking (performs the HTTP POST synchronously).
int send_qr_to_server(const char *qr_data);

// Cleanup on program exit.
void server_comm_cleanup(void);

#endif // SERVER_COMMUNICATION_H
