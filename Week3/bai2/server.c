#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#define BUFFER_SIZE 4096

/**
 * @function isNumericAddress: Checks if the string consists solely of digits and dots.
 *
 * @param str: A pointer to the string to check.
 *
 * @return: true if the string consists solely of digits and contains at least one dot,
 *          false otherwise.
 */
bool isNumericAddress(const char *str) {
    if (str == NULL || *str == '\0') {
        return false;
    }

    bool hasDot = false;
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '.') {
            hasDot = true;
        } else if (!isdigit((unsigned char)str[i])) {
            return false;
        }
    }

    return hasDot;
}

/**
 * @function isValidIPv4: Validates whether a string is a strictly valid IPv4 address.
 *
 * @param str: A pointer to the candidate IPv4 string.
 *
 * @return: true if the address is a valid IPv4 address (4 octets, 0-255 each),
 *          false otherwise.
 */
bool isValidIPv4(const char *str) {
    struct in_addr addr;
    if (inet_pton(AF_INET, str, &addr) != 1) {
        return false;
    }

    int dotCount = 0;
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '.') {
            dotCount++;
        }
    }

    return (dotCount == 3);
}

/**
 * @function resolveQuery: Processes domain/IP resolution according to Week 3 specifications.
 *
 * @param query: The domain name or IP string sent by the client.
 * @param response: The output buffer for the resolution result.
 * @param responseSize: Maximum capacity of the response buffer.
 *
 * @return: None.
 */
void resolveQuery(const char *query, char *response, size_t responseSize) {
    if (query == NULL || *query == '\0') {
        snprintf(response, responseSize, "Not found information");
        return;
    }

    // Case 1: Numeric address candidate
    if (isNumericAddress(query)) {
        if (!isValidIPv4(query)) {
            snprintf(response, responseSize, "IP Address is invalid");
            return;
        }

        struct in_addr addr;
        inet_pton(AF_INET, query, &addr);

        struct hostent *host = gethostbyaddr((const void *)&addr, sizeof(addr), AF_INET);
        if (host == NULL || host->h_name == NULL) {
            snprintf(response, responseSize, "Not found information");
            return;
        }

        char buffer[BUFFER_SIZE];
        int offset = snprintf(buffer, sizeof(buffer), "Official name: %s\nAlias name:\n", host->h_name);

        if (host->h_aliases != NULL) {
            for (int i = 0; host->h_aliases[i] != NULL && offset < (int)sizeof(buffer); i++) {
                offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s\n", host->h_aliases[i]);
            }
        }

        // Remove trailing newline if present to keep format clean
        if (offset > 0 && buffer[offset - 1] == '\n') {
            buffer[offset - 1] = '\0';
        }

        snprintf(response, responseSize, "%s", buffer);
        return;
    }

    // Case 2: Candidate domain name
    struct hostent *host = gethostbyname(query);
    if (host == NULL || host->h_addr_list == NULL || host->h_addr_list[0] == NULL) {
        snprintf(response, responseSize, "Not found information");
        return;
    }

    char buffer[BUFFER_SIZE];
    int offset = snprintf(buffer, sizeof(buffer), "Official IP: %s\nAlias IP:\n",
                          inet_ntoa(*(struct in_addr *)host->h_addr_list[0]));

    for (int i = 1; host->h_addr_list[i] != NULL && offset < (int)sizeof(buffer); i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s\n",
                          inet_ntoa(*(struct in_addr *)host->h_addr_list[i]));
    }

    // Remove trailing newline if present
    if (offset > 0 && buffer[offset - 1] == '\n') {
        buffer[offset - 1] = '\0';
    }

    snprintf(response, responseSize, "%s", buffer);
}

/**
 * @function main: Server entry point for UDP Domain Name Resolver.
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

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        return 1;
    }

    struct sockaddr_in servaddr, cliaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons((uint16_t)port);

    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind failed");
        close(sockfd);
        return 1;
    }

    printf("UDP Domain Resolver Server is running on port %d...\n", port);

    char buffer[BUFFER_SIZE];
    char response[BUFFER_SIZE];

    while (1) {
        socklen_t len = sizeof(cliaddr);
        ssize_t n = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                             (struct sockaddr *)&cliaddr, &len);
        if (n < 0) {
            perror("recvfrom error");
            continue;
        }

        buffer[n] = '\0';

        // Trim any trailing newline characters
        while (n > 0 && (buffer[n - 1] == '\n' || buffer[n - 1] == '\r')) {
            buffer[--n] = '\0';
        }

        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cliaddr.sin_addr, clientIP, sizeof(clientIP));
        int clientPort = ntohs(cliaddr.sin_port);

        printf("[+] Received query from [%s:%d]: \"%s\"\n", clientIP, clientPort, buffer);

        resolveQuery(buffer, response, sizeof(response));

        printf("[+] Sending response to [%s:%d]:\n%s\n----------------------------------------\n",
               clientIP, clientPort, response);

        sendto(sockfd, response, strlen(response), 0,
               (struct sockaddr *)&cliaddr, len);
    }


    close(sockfd);
    return 0;
}
