#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define BUFFER_SIZE 4096

/**
 * @function main: Client entry point. Connects to server via UDP, reads input
 *                 from stdin repeatedly until an empty string is entered,
 *                 sends to server, and prints received responses.
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

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
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

        ssize_t sent = sendto(sockfd, sendline, len, 0,
                              (const struct sockaddr *)&servaddr, sizeof(servaddr));
        if (sent < 0) {
            perror("sendto failed");
            break;
        }

        socklen_t addrlen = sizeof(servaddr);
        ssize_t n = recvfrom(sockfd, recvline, sizeof(recvline) - 1, 0,
                             (struct sockaddr *)&servaddr, &addrlen);
        if (n < 0) {
            perror("recvfrom failed");
            break;
        }

        recvline[n] = '\0';
        printf("OUTPUT:\n%s\n\n", recvline);
    }


    close(sockfd);
    return 0;
}
