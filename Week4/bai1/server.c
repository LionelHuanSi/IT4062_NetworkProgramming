#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>

#define BUFFER_SIZE 4096

/**
 * @function recv_all: Receives exactly len bytes from a stream socket,
 *                     handling short reads and TCP byte-stream fragmentation.
 *
 * @param fd: The connected socket file descriptor.
 * @param buf: Pointer to destination buffer.
 * @param len: Total number of bytes to receive.
 *
 * @return: Number of bytes received on success (len), 0 on orderly shutdown (EOF),
 *          or -1 on error.
 */
ssize_t recv_all(int fd, void *buf, size_t len) {
    size_t nLeft = len;
    char *ptr = (char *)buf;

    while (nLeft > 0) {
        ssize_t nRead = recv(fd, ptr, nLeft, 0);
        if (nRead < 0) {
            return -1; // Socket error
        } else if (nRead == 0) {
            return (ssize_t)(len - nLeft); // EOF / peer closed connection
        }
        nLeft -= nRead;
        ptr += nRead;
    }

    return (ssize_t)len;
}

/**
 * @function send_all: Transmits exactly len bytes over a stream socket,
 *                     handling short writes and socket buffer limitations.
 *
 * @param fd: The connected socket file descriptor.
 * @param buf: Pointer to the source data buffer.
 * @param len: Total number of bytes to transmit.
 *
 * @return: Number of bytes sent on success (len), or -1 on error.
 */
ssize_t send_all(int fd, const void *buf, size_t len) {
    size_t nLeft = len;
    const char *ptr = (const char *)buf;

    while (nLeft > 0) {
        ssize_t nWritten = send(fd, ptr, nLeft, 0);
        if (nWritten <= 0) {
            return -1;
        }
        nLeft -= nWritten;
        ptr += nWritten;
    }

    return (ssize_t)len;
}

/**
 * @function processString: Splits an alphanumeric string into digits and letters.
 *                          If the string contains any non-alphanumeric character,
 *                          sets response to "Error".
 *
 * @param input: A pointer to the input string received from the client.
 * @param output: A pointer to the output buffer for the formatted response.
 * @param outputSize: Maximum capacity of the output buffer.
 *
 * @return: None.
 */
void processString(const char *input, char *output, size_t outputSize) {
    char digits[BUFFER_SIZE] = "";
    char letters[BUFFER_SIZE] = "";
    size_t dIndex = 0;
    size_t lIndex = 0;

    for (size_t i = 0; input[i] != '\0'; i++) {
        unsigned char c = (unsigned char)input[i];

        if (isdigit(c)) {
            if (dIndex < BUFFER_SIZE - 1) {
                digits[dIndex++] = (char)c;
            }
        } else if (isalpha(c)) {
            if (lIndex < BUFFER_SIZE - 1) {
                letters[lIndex++] = (char)c;
            }
        } else {
            // Contains invalid character (not a digit and not an alphabetical letter)
            snprintf(output, outputSize, "Error");
            return;
        }
    }

    digits[dIndex] = '\0';
    letters[lIndex] = '\0';

    if (dIndex > 0 && lIndex > 0) {
        snprintf(output, outputSize, "%s\n%s", digits, letters);
    } else if (dIndex > 0) {
        snprintf(output, outputSize, "%s", digits);
    } else if (lIndex > 0) {
        snprintf(output, outputSize, "%s", letters);
    } else {
        output[0] = '\0';
    }
}

/**
 * @function main: Server entry point. Listens on a specified TCP port,
 *                 handles incoming client connections iteratively, receives
 *                 length-prefixed strings, processes them, and returns responses.
 *
 * @param argc: Number of command-line arguments.
 * @param argv: Array of command-line argument strings.
 *
 * @return: 0 on success, 1 on argument/socket error.
 */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <PortNumber>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        printf("Invalid port number: %s\n", argv[1]);
        return 1;
    }

    // Ignore SIGPIPE to avoid termination if client disconnects abruptly
    signal(SIGPIPE, SIG_IGN);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket creation failed");
        return 1;
    }

    // Enable SO_REUSEADDR to allow fast restarts
    int opt = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(listenfd);
        return 1;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons((uint16_t)port);

    if (bind(listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind failed");
        close(listenfd);
        return 1;
    }

    if (listen(listenfd, 10) < 0) {
        perror("listen failed");
        close(listenfd);
        return 1;
    }

    printf("TCP String Splitter Server is running on port %d...\n", port);

    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int connfd = accept(listenfd, (struct sockaddr *)&cliaddr, &clilen);
        if (connfd < 0) {
            perror("accept error");
            continue;
        }

        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cliaddr.sin_addr, clientIP, sizeof(clientIP));
        int clientPort = ntohs(cliaddr.sin_port);

        printf("[+] Client [%s:%d] connected.\n", clientIP, clientPort);

        char inputBuffer[BUFFER_SIZE];
        char outputBuffer[BUFFER_SIZE];

        while (1) {
            uint32_t netLen = 0;
            // Read 4-byte message length prefix
            ssize_t ret = recv_all(connfd, &netLen, sizeof(netLen));
            if (ret <= 0) {
                // Client closed connection or error
                break;
            }

            uint32_t msgLen = ntohl(netLen);
            if (msgLen >= BUFFER_SIZE) {
                printf("[-] Message length exceeds buffer capacity: %u bytes\n", msgLen);
                break;
            }

            // Read the message body of exact msgLen bytes
            ret = recv_all(connfd, inputBuffer, msgLen);
            if (ret != (ssize_t)msgLen) {
                printf("[-] Incomplete message received from [%s:%d]\n", clientIP, clientPort);
                break;
            }
            inputBuffer[msgLen] = '\0';

            printf("[+] Received from [%s:%d]: \"%s\"\n", clientIP, clientPort, inputBuffer);

            processString(inputBuffer, outputBuffer, sizeof(outputBuffer));

            printf("[+] Sending response to [%s:%d]:\n%s\n----------------------------------------\n",
                   clientIP, clientPort, outputBuffer);

            // Send length-prefixed response
            uint32_t respLen = (uint32_t)strlen(outputBuffer);
            uint32_t netRespLen = htonl(respLen);

            if (send_all(connfd, &netRespLen, sizeof(netRespLen)) < 0) {
                perror("send_all length error");
                break;
            }

            if (respLen > 0) {
                if (send_all(connfd, outputBuffer, respLen) < 0) {
                    perror("send_all body error");
                    break;
                }
            }
        }

        printf("[-] Client [%s:%d] disconnected.\n", clientIP, clientPort);
        close(connfd);
    }

    close(listenfd);
    return 0;
}
