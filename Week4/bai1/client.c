#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
 * @function main: Client entry point. Connects to the server via TCP socket,
 *                 reads input strings repeatedly until an empty string is entered,
 *                 sends length-prefixed strings, and prints received responses.
 *
 * @param argc: Number of command-line arguments.
 * @param argv: Array of command-line argument strings.
 *
 * @return: 0 on success, 1 on argument/socket error.
 */
int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <IPAddress> <PortNumber>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        printf("Invalid port number: %s\n", argv[2]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        return 1;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, argv[1], &servaddr.sin_addr) <= 0) {
        printf("Invalid IP address: %s\n", argv[1]);
        close(sockfd);
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("connect failed");
        close(sockfd);
        return 1;
    }

    char sendline[BUFFER_SIZE];
    char recvline[BUFFER_SIZE];

    while (1) {
        printf("INPUT: ");
        fflush(stdout);

        if (fgets(sendline, sizeof(sendline), stdin) == NULL) {
            break;
        }

        size_t len = strlen(sendline);

        // Trim trailing newline characters
        while (len > 0 && (sendline[len - 1] == '\n' || sendline[len - 1] == '\r')) {
            sendline[--len] = '\0';
        }

        // Terminate client when user inputs an empty string
        if (len == 0) {
            break;
        }

        // Send length-prefixed message
        uint32_t netLen = htonl((uint32_t)len);
        if (send_all(sockfd, &netLen, sizeof(netLen)) < 0) {
            perror("send length failed");
            break;
        }

        if (send_all(sockfd, sendline, len) < 0) {
            perror("send data failed");
            break;
        }

        // Receive length-prefixed response
        uint32_t netRespLen = 0;
        ssize_t ret = recv_all(sockfd, &netRespLen, sizeof(netRespLen));
        if (ret <= 0) {
            printf("Server closed connection.\n");
            break;
        }

        uint32_t respLen = ntohl(netRespLen);
        if (respLen >= BUFFER_SIZE) {
            printf("Response length too large: %u\n", respLen);
            break;
        }

        ret = recv_all(sockfd, recvline, respLen);
        if (ret != (ssize_t)respLen) {
            printf("Incomplete response received.\n");
            break;
        }
        recvline[respLen] = '\0';

        printf("OUTPUT:\n%s\n\n", recvline);
    }

    close(sockfd);
    return 0;
}
