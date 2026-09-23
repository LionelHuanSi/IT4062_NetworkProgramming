#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define BUFFER_SIZE 4096

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
 * @function main: Server entry point. Listens on a given UDP port, processes
 *                 strings received from clients, and returns the separated results.
 *
 * @param argc: Number of command-line arguments.
 * @param argv: Array of command-line argument strings.
 *
 * @return: 0 on success, 1 on argument error.
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

    printf("UDP String Splitter Server is running on port %d...\n", port);

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

        // Trim any trailing newline or carriage return characters
        while (n > 0 && (buffer[n - 1] == '\n' || buffer[n - 1] == '\r')) {
            buffer[--n] = '\0';
        }

        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cliaddr.sin_addr, clientIP, sizeof(clientIP));
        int clientPort = ntohs(cliaddr.sin_port);

        printf("[+] Received from [%s:%d]: \"%s\"\n", clientIP, clientPort, buffer);

        processString(buffer, response, sizeof(response));

        printf("[+] Sending response to [%s:%d]:\n%s\n----------------------------------------\n",
               clientIP, clientPort, response);

        sendto(sockfd, response, strlen(response), 0,
               (struct sockaddr *)&cliaddr, len);
    }


    close(sockfd);
    return 0;
}
