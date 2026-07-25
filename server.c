#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/syslimits.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/select.h>

#define MAX_CLIENTS 50

struct http_request {
    char method[8];
    char path[256];
    char version[16];
};

struct client_state {
    int fd;
    char buf[4096];
    size_t buf_len;   // how many bytes currently accumulated
    int in_use;       // is this slot occupied by a real client, or free?
};

int parse_request(char* raw_request, struct http_request *out) {
    char *request_line = strstr(raw_request, "\r\n");
    if (request_line == NULL) return -1;
    *request_line = '\0';

    for (int i = 0; i < 3; i++) {
        char *token = strtok((i == 0 ? raw_request : NULL), " ");
        if (token == NULL) return -1;

        if (i == 0) {
            strlcpy(out->method, token, sizeof(out->method));
        } else if (i == 1) {
            strlcpy(out->path, token, sizeof(out->path));
        } else {
            strlcpy(out->version, token, sizeof(out->version));
        }
    }

    return 0;
}

int resolve_safe_path(const char *root, const char *req_path, char *resolved_out) {
    char root_buf[PATH_MAX];
    char combined_buf[PATH_MAX];
    char resolved_buf[PATH_MAX];

    char *resolved_root = realpath(root, root_buf);
    if (resolved_root == NULL) {
        perror("file not found");

        return -1;
    }

    /* concat the paths */
    combined_buf[0] = '\0';
    strlcat(combined_buf, root, sizeof(combined_buf));
    strlcat(combined_buf, req_path, sizeof(combined_buf));

    char *resolved_combined = realpath(combined_buf, resolved_buf);
    if (resolved_combined == NULL) {
        perror("file not found");

        return -1;
    }

    size_t root_len = strlen(root_buf);
    if (strncmp(resolved_buf, root_buf, root_len) != 0) {
        return -1;
    }

    if (resolved_buf[root_len] != '\0' && resolved_buf[root_len] != '/') {
        return -1;
    }

    strlcpy(resolved_out, resolved_buf, PATH_MAX);

    return 0;
}

void send_response(int client_fd, int status_code, const char *reason, const char *body) {
    char out_buf[1024];
    ssize_t out_n_bytes;
    char headers[1024];

    size_t body_len = strlen(body);

    snprintf(headers, sizeof(headers),
        "HTTP/1.1 %i %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %lu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, reason, body_len);

    /* compose response for the client */
    out_buf[0] = '\0';
    strlcat(out_buf, headers, sizeof(out_buf));
    strlcat(out_buf, body, sizeof(out_buf));

    /* write response back to client */
    out_n_bytes = write(client_fd, out_buf, strlen(out_buf));

    if(out_n_bytes < 0) {
        perror("failed to write response back to client");
        exit(EXIT_FAILURE);
    }
}

void send_file_response(int client_fd, int file_fd, size_t file_size) {
    char headers[1024];
    char chunk[4096];

    snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        file_size);

    // send headers first, as their own write
    int header_bytes = write(client_fd, headers, strlen(headers));

    if(header_bytes < 0) {
        perror("failed to write response back to client");
        close(file_fd);
        close(client_fd);

        exit(EXIT_FAILURE);
    }

    while (1) {
        int res = read(file_fd, chunk, sizeof(chunk));

        if (res > 0) {
            int n_bytes = write(client_fd, chunk, res);

            if (n_bytes < 0) {
                perror("failed to write response back to client");
                close(file_fd);
                close(client_fd);

                exit(EXIT_FAILURE);
            }
        } else if (res == 0) {
            // end of file reached
            close(file_fd);
            close(client_fd);

            break;
        } else {
            // write fails
            perror("failed to write response back to client");
            close(file_fd);
            close(client_fd);

            exit(EXIT_FAILURE);
        }
    }
}

int main() {
    int server_fd;
    int rc;
    fd_set readfds;
    int nfds;

    struct sockaddr_in server_addr;
    struct client_state clients[MAX_CLIENTS];

    /* create socket -> listen file descriptor */
    server_fd = socket(PF_INET, SOCK_STREAM, 0);

    if (server_fd < 0 ) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof server_addr); // zero-ing the server addr struct to avoid surprises :)
    server_addr.sin_port = htons(4000);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_family = AF_INET;

    /* binding the socket to a port */
    rc = bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));

    if (rc < 0) {
        perror("Failed to bind to port");
        exit(EXIT_FAILURE);
    }

    /* listening at the bounded port */
    rc = listen(server_fd, 5);

    if (rc < 0) {
        perror("Failed to listen on port:4000");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].in_use = 0;
    }

    while (1) {
        nfds = 0;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);

        if (server_fd >= nfds) nfds = server_fd + 1;

        for(int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].in_use == 1) {
                FD_SET(clients[i].fd, &readfds);

                if (clients[i].fd >= nfds) {
                    nfds = clients[i].fd + 1;
                }
            }
        }

        int ready = select(nfds, &readfds, NULL, NULL, NULL);
        if (ready < 0) {
            perror("select failed");
            exit(EXIT_FAILURE);
        }

        if (FD_ISSET(server_fd, &readfds)) {
            int client_fd;
            socklen_t client_len;

            struct sockaddr_in client_addr;

            memset(&client_addr, 0, sizeof(client_addr)); // zero-ing the client addr struct to avoid surprises :)
            client_len = sizeof(client_addr);

            /* accepting client connection (blocking) */
            client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

            if (client_fd < 0) {
                perror("Failed to accept incoming requests");
                exit(EXIT_FAILURE);
            }

            int slot_found = 0;

            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].in_use == 0) {
                    clients[i].fd = client_fd;
                    clients[i].in_use = 1;
                    clients[i].buf_len = 0;
                    slot_found = 1;

                    break;
                }
            }

            // if all slots are take, close connection...
            if (!slot_found) close(client_fd);
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].in_use == 1) {
                if (FD_ISSET(clients[i].fd, &readfds)) {
                    struct client_state *client = &clients[i];
                    ssize_t in_n_bytes;
                    char *root = "./public";
                    char resolved[PATH_MAX];

                    struct http_request req;
                    struct stat file_stat;

                    in_n_bytes = read(client->fd, client->buf + client->buf_len, sizeof(client->buf) - client->buf_len);

                    if (in_n_bytes > 0) {
                        client->buf_len += in_n_bytes;
                        client->buf[client->buf_len] = '\0';
                        char *headers_end = strstr(client->buf, "\r\n\r\n");

                        if (headers_end != NULL) {
                            int res = parse_request(client->buf, &req);

                            if (res < 0) {
                                send_response(client->fd, 400, "Bad Request", "");
                                close(client->fd);

                                client->in_use = 0;
                                continue;
                            }

                            /* resolving path to prevent path traversal and handle path mismatch */
                            int resolve_res = resolve_safe_path(root, req.path, resolved);

                            if (resolve_res < 0) {
                                send_response(client->fd, 404, "Not Found", "");
                                close(client->fd);

                                client->in_use = 0;
                                continue;
                            }

                            int file_fd = open(resolved, O_RDONLY);

                            if (file_fd < 0) {
                                perror("failed to open file");
                                send_response(client->fd, 404, "Not Found", "");
                                close(client->fd);

                                client->in_use = 0;
                                continue;
                            }

                            if (fstat(file_fd, &file_stat) < 0) {
                                perror("failed to stat file");
                                send_response(client->fd, 500, "Internal Server Error", "");
                                close(client->fd);

                                client->in_use = 0;
                                continue;
                            }

                            if (!S_ISREG(file_stat.st_mode)) {
                                send_response(client->fd, 404, "Not Found", "");
                                close(file_fd);
                                close(client->fd);

                                client->in_use = 0;
                                continue;
                            }

                            send_file_response(client->fd, file_fd, file_stat.st_size); // closing fd already done here

                            client->in_use = 0;
                        }
                    } else if (in_n_bytes == 0) {
                        close(client->fd);
                        client->in_use = 0;
                    } else {
                        perror("failed to read");
                        close(client->fd);
                        client->in_use = 0;
                    }
                }
            }
        }
    }

    return 0;
}
