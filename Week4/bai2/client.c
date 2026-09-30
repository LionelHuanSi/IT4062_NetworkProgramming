#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>

#define CHUNK_SIZE 65536
#define PATH_BUFFER_SIZE 1024

// Protocol Status Codes
#define STATUS_READY 0
#define STATUS_FILE_EXISTS 1
#define STATUS_ERROR 2
#define STATUS_SUCCESS 0
#define STATUS_INTERRUPTED 1

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
            return -1;
        } else if (nRead == 0) {
            return (ssize_t)(len - nLeft);
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
 * @function pack_upload_header: Serializes the 16-byte upload request header
 *                               into network byte order.
 *
 * @param buf: Pointer to the 16-byte destination buffer.
 * @param op: Operation code (e.g. 1 for upload).
 * @param name_len: Length of filename in bytes.
 * @param file_size: 64-bit file size.
 *
 * @return: None.
 */
void pack_upload_header(uint8_t *buf, uint32_t op, uint32_t name_len, uint64_t file_size) {
    uint32_t net_op = htonl(op);
    uint32_t net_name_len = htonl(name_len);
    memcpy(buf, &net_op, 4);
    memcpy(buf + 4, &net_name_len, 4);

    uint64_t sz = file_size;
    for (int i = 7; i >= 0; i--) {
        buf[8 + i] = (uint8_t)(sz & 0xFF);
        sz >>= 8;
    }
}

/**
 * @function getBasename: Extracts the filename component from a path string,
 *                        supporting both Unix '/' and Windows '\' directory separators.
 *
 * @param path: Full or relative path to the file.
 *
 * @return: Pointer to the start of the filename within path.
 */
const char *getBasename(const char *path) {
    const char *p = path;
    const char *last = path;

    while (*p != '\0') {
        if (*p == '/' || *p == '\\') {
            last = p + 1;
        }
        p++;
    }

    return last;
}

/**
 * @function main: Client entry point for TCP File Transfer.
 *                 Repeatedly prompts user for a file path, validates local existence,
 *                 negotiates with server, streams data in chunks, and reports results.
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

    char filepath[PATH_BUFFER_SIZE];

    while (1) {
        printf("INPUT: ");
        fflush(stdout);

        if (fgets(filepath, sizeof(filepath), stdin) == NULL) {
            break;
        }

        size_t len = strlen(filepath);

        // Trim trailing newline characters
        while (len > 0 && (filepath[len - 1] == '\n' || filepath[len - 1] == '\r')) {
            filepath[--len] = '\0';
        }

        // Terminate client when user enters an empty string
        if (len == 0) {
            break;
        }

        // Check if file exists locally
        FILE *fp = fopen(filepath, "rb");
        if (fp == NULL) {
            printf("OUTPUT:\nError: File not found\n\n");
            continue;
        }

        // Determine exact file size
        if (fseeko(fp, 0, SEEK_END) != 0) {
            perror("fseeko failed");
            fclose(fp);
            continue;
        }
        off_t fileSize = ftello(fp);
        if (fileSize < 0) {
            perror("ftello failed");
            fclose(fp);
            continue;
        }
        fseeko(fp, 0, SEEK_SET);

        const char *filename = getBasename(filepath);
        uint32_t nameLen = (uint32_t)strlen(filename);

        // Send 16-byte upload request header
        uint8_t headerBuf[16];
        pack_upload_header(headerBuf, 1, nameLen, (uint64_t)fileSize);

        if (send_all(sockfd, headerBuf, sizeof(headerBuf)) < 0) {
            printf("OUTPUT:\nError: File tranfering is interupted\n\n");
            fclose(fp);
            break;
        }

        // Send filename string
        if (send_all(sockfd, filename, nameLen) < 0) {
            printf("OUTPUT:\nError: File tranfering is interupted\n\n");
            fclose(fp);
            break;
        }

        // Receive server check response
        int32_t netStatus = 0;
        ssize_t ret = recv_all(sockfd, &netStatus, sizeof(netStatus));
        if (ret != (ssize_t)sizeof(netStatus)) {
            printf("OUTPUT:\nError: File tranfering is interupted\n\n");
            fclose(fp);
            break;
        }

        int32_t status = ntohl(netStatus);
        if (status == STATUS_FILE_EXISTS) {
            printf("OUTPUT:\nError: File is existent on server\n\n");
            fclose(fp);
            continue;
        } else if (status != STATUS_READY) {
            printf("OUTPUT:\nError: File tranfering is interupted\n\n");
            fclose(fp);
            continue;
        }

        // Stream file contents in chunks
        uint64_t totalSent = 0;
        bool transferError = false;
        char chunk[CHUNK_SIZE];

        while (totalSent < (uint64_t)fileSize) {
            size_t toRead = sizeof(chunk);
            if ((uint64_t)fileSize - totalSent < toRead) {
                toRead = (size_t)((uint64_t)fileSize - totalSent);
            }

            size_t nRead = fread(chunk, 1, toRead, fp);
            if (nRead == 0) {
                transferError = true;
                break;
            }

            if (send_all(sockfd, chunk, nRead) < 0) {
                transferError = true;
                break;
            }

            totalSent += (uint64_t)nRead;
            // Small microsecond delay per 64KB chunk to simulate network transmission time
            usleep(2500);
        }

        fclose(fp);

        if (transferError) {
            printf("OUTPUT:\nError: File tranfering is interupted\n\n");
            break;
        }

        // Receive final completion status from server
        int32_t netResult = 0;
        ret = recv_all(sockfd, &netResult, sizeof(netResult));
        if (ret != (ssize_t)sizeof(netResult)) {
            printf("OUTPUT:\nError: File tranfering is interupted\n\n");
            break;
        }

        int32_t result = ntohl(netResult);
        if (result == STATUS_SUCCESS) {
            printf("OUTPUT:\nSuccessful transfering\n\n");
        } else {
            printf("OUTPUT:\nError: File tranfering is interupted\n\n");
        }
    }

    close(sockfd);
    return 0;
}
