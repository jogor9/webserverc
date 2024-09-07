#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define MAX_REQUEST_SIZE (1024)

#define LOGF(fmt, ...)                                                         \
        fprintf(stderr,                                                        \
                __FILE__ ":%s:%d: " fmt "\n",                                  \
                __func__,                                                      \
                __LINE__,                                                      \
                __VA_ARGS__)
#define LOG(x) LOGF("%s", x)

ssize_t read_process(const char *restrict command,
                     char *restrict output,
                     size_t max_sz)
{
        FILE *stream;
        ssize_t out;

        stream = popen(command, "r");
        if (!stream) {
                return -1;
        }

        out = fread(output, 1, max_sz, stream);
        if (!feof(stream) || ferror(stream)) {
                out = -1;
        }

        pclose(stream);

        return out;
}

// parses at_least strs and maybe_more additional strs in input
// negative maybe_more means infinite
// str must be non-empty
static inline const char *linear_parse(const char *restrict str,
                                       size_t at_least,
                                       ssize_t maybe_more,
                                       bool negate,
                                       const char *restrict input)
{
        assert(*str != 0);
        size_t len = strlen(str);

        for (; at_least > 0; --at_least) {
                if ((strncmp(str, input, len) != 0) ^ negate) {
                        return NULL;
                }
                input += len;
        }

        if (maybe_more < 0) {
                while ((strncmp(str, input, len) == 0) ^ negate) {
                        input += len;
                }
        } else {
                while (maybe_more > 0
                       && (strncmp(str, input, len) == 0) ^ negate) {
                        input += len;
                        --maybe_more;
                }
        }

        return input;
}

int main(int argc, char **argv)
{
        const int server_port = 7696;
        static const char BadRequest[] = "HTTP/1.1 400 Bad Request\r\n"
                                         "Content-Type: text/plain\r\n"
                                         "\r\n"
                                         "Bad Request",
                          NotFound[] = "HTTP/1.1 404 Not Found\r\n"
                                       "Content-Type: text/plain\r\n"
                                       "\r\n"
                                       "Not Found",
                          InternalServerError[] =
                                  "HTTP/1.1 500 Internal Server Error\r\n"
                                  "Content-Type: text/plain\r\n"
                                  "\r\n"
                                  "Internal Server Error",
                          XdgMimeCommand[] = "xdg-mime query filetype ",
                          ResponseFmt[] = "HTTP/1.1 200 OK\r\n"
                                          "Content-Type: %.*s\r\n"
                                          "\r\n";
        int server_fd, client_fd, requested_fd, status = 0;
        struct sockaddr_in server_address = {
                .sin_family = AF_INET,
                .sin_addr = {
                        .s_addr = INADDR_ANY,
                },
                .sin_port = htons(server_port),
        };
        struct sockaddr_in client_address;
        struct stat file_stats;
        socklen_t client_address_len;
        static char request_buffer[MAX_REQUEST_SIZE
                                   + sizeof(XdgMimeCommand)] = { 0 },
                                     mime_type[MAX_REQUEST_SIZE] = { 0 };
        size_t response_sz;
        ssize_t recv_bytes, bytes_read, bytes_written;
        char *path, *path_end, *response;

        if (argc < 2) {
                eprintf("usage: %s DIRECTORY\n", argv[0]);
                status = 1;
                goto main_exit;
        }
        if (chdir(argv[1]) < 0) {
                eprintf("Could not open directory '%s'", argv[1]);
                perror(NULL);
                status = 1;
                goto main_exit;
        }

        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
                perror("Could not create socket");
                status = 1;
                goto main_exit;
        }

        if (bind(server_fd,
                 (struct sockaddr *)&server_address,
                 sizeof(server_address))
            < 0) {
                eprintf("Could not bind to localhost:%d: ", server_port);
                perror(NULL);
                status = 1;
                goto main_close_socket;
        }

        if (listen(server_fd, 100) < 0) {
                eprintf("Could not listen on localhost:%d: ", server_port);
                perror(NULL);
                status = 1;
                goto main_close_socket;
        }

        while (1) {
                client_address_len = sizeof(client_address);
                client_fd = accept(server_fd,
                                   (struct sockaddr *)&client_address,
                                   &client_address_len);
                if (client_fd < 0) {
                        perror("Failed to accept a client request");
                        continue;
                }

                recv_bytes =
                        recv(client_fd, request_buffer, MAX_REQUEST_SIZE, 0);
                if (recv_bytes < 0) {
                        perror("Failed to retrieve client request");
                        goto main_loop_err_close_client;
                }
                LOGF("Received client request:\n%s", request_buffer);
                request_buffer[recv_bytes] = 0;
		fputs(request_buffer, stderr);
                path = request_buffer;
                path = (char *)linear_parse("GET /", 1, 0, false, path);
                if (!path) {
                        LOG("not a GET request");
                        strcpy(request_buffer, BadRequest);
                        response_sz = sizeof(BadRequest) - 1;
                        goto main_loop_err_send_response;
                }
                path_end = (char *)linear_parse(" ", 1, -1, true, path);
                if (!path_end) {
                        LOG("invalid path value");
                        strcpy(request_buffer, BadRequest);
                        response_sz = sizeof(BadRequest) - 1;
                        goto main_loop_err_send_response;
                }
                *path_end = 0;

                requested_fd = open(path, O_RDONLY);
                if (requested_fd < 0) {
                        LOGF("could not open '%s'", path);
                        strcpy(request_buffer, NotFound);
                        response_sz = sizeof(NotFound) - 1;
                        goto main_loop_err_send_response;
                }

                if (fstat(requested_fd, &file_stats) < 0) {
                        LOG("could not retrieve file information");
                        strcpy(request_buffer, InternalServerError);
                        response_sz = sizeof(InternalServerError) - 1;
                        goto main_loop_err_close_file;
                }

                memmove(request_buffer + sizeof(XdgMimeCommand) - 1,
                        path,
                        path_end - path);
                path_end = request_buffer + sizeof(XdgMimeCommand) - 1
                         + (path_end - path);
                path = request_buffer + sizeof(XdgMimeCommand) - 1;
                memcpy(request_buffer,
                       XdgMimeCommand,
                       sizeof(XdgMimeCommand) - 1);
                *path_end = 0;

                bytes_read = read_process(request_buffer,
                                          mime_type,
                                          MAX_REQUEST_SIZE);
                if (bytes_read < 0) {
                        LOGF("could not read process '%s'", request_buffer);
                        strcpy(request_buffer, InternalServerError);
                        response_sz = sizeof(InternalServerError) - 1;
                        goto main_loop_err_close_file;
                }

                response = malloc(sizeof(ResponseFmt) - 4 + bytes_read - 1
                                  + file_stats.st_size);
                if (!response) {
                        LOG("insufficient memory");
                        strcpy(request_buffer, InternalServerError);
                        response_sz = sizeof(InternalServerError) - 1;
                        goto main_loop_err_close_file;
                }
                bytes_written = sprintf(response,
                                        ResponseFmt,
                                        (int)(bytes_read - 1),
                                        mime_type);
                if (read(requested_fd,
                         response + bytes_written,
                         file_stats.st_size)
                    < 0) {
                        LOGF("could not read from '%s'", path);
                        strcpy(request_buffer, InternalServerError);
                        response_sz = sizeof(InternalServerError) - 1;
                        goto main_loop_err_free_response;
                }
                if (send(client_fd,
                         response,
                         bytes_written + file_stats.st_size,
                         0)
                    < 0) {
                        LOG("could not send response");
                        strcpy(request_buffer, InternalServerError);
                        response_sz = sizeof(InternalServerError) - 1;
                        goto main_loop_err_free_response;
                }
                free(response);
                close(requested_fd);
                close(client_fd);
                continue;
        main_loop_err_free_response:
                free(response);
        main_loop_err_close_file:
                close(requested_fd);
        main_loop_err_send_response:
                send(client_fd, request_buffer, response_sz, 0);
        main_loop_err_close_client:
                close(client_fd);
        }

main_close_socket:
        close(server_fd);
main_exit:
        return status;
}
