#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/**
 * @function isNumericAddress: Checks whether the input string consists solely
 *                             of digits and dots (identifying potential IP candidates).
 *
 * @param str: A pointer to the input string to evaluate.
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
 * @function isValidIPv4: Validates whether a string is a strictly compliant IPv4
 *                        address in dotted-decimal format (4 octets, each 0 - 255).
 *
 * @param str: A pointer to the candidate IPv4 string.
 *
 * @return: true if the address is a valid IPv4 address, false otherwise.
 */
bool isValidIPv4(const char *str) {
    struct in_addr addr;
    // inet_pton validates octet range [0, 255] and syntax
    if (inet_pton(AF_INET, str, &addr) != 1) {
        return false;
    }

    // Ensure exactly 3 dots to prevent truncated addresses (e.g., "1.2.3")
    int dotCount = 0;
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '.') {
            dotCount++;
        }
    }

    return (dotCount == 3);
}

/**
 * @function resolveDomain: Performs forward DNS resolution (Domain name to IP addresses).
 *                          Outputs the Official IP and any Alias IPs found.
 *
 * @param domain: A pointer to a string containing the domain name.
 *
 * @return: None.
 */
void resolveDomain(const char *domain) {
    struct hostent *host = gethostbyname(domain);
    if (host == NULL || host->h_addr_list == NULL || host->h_addr_list[0] == NULL) {
        printf("Not found information\n");
        return;
    }

    // Print the primary/official IP address
    printf("Official IP: %s\n", inet_ntoa(*(struct in_addr *)host->h_addr_list[0]));

    // Print secondary/alias IP addresses
    printf("Alias IP:\n");
    for (int i = 1; host->h_addr_list[i] != NULL; i++) {
        printf("%s\n", inet_ntoa(*(struct in_addr *)host->h_addr_list[i]));
    }
}

/**
 * @function resolveIP: Performs reverse DNS resolution (IPv4 address to Hostnames).
 *                      Outputs the Official hostname and any Alias names found.
 *
 * @param ipStr: A pointer to a string containing the IPv4 address.
 *
 * @return: None.
 */
void resolveIP(const char *ipStr) {
    struct in_addr addr;
    inet_pton(AF_INET, ipStr, &addr);

    struct hostent *host = gethostbyaddr((const void *)&addr, sizeof(addr), AF_INET);
    if (host == NULL || host->h_name == NULL) {
        printf("Not found information\n");
        return;
    }

    // Print the primary/official hostname
    printf("Official name: %s\n", host->h_name);

    // Print alias hostnames if present
    printf("Alias name:\n");
    if (host->h_aliases != NULL) {
        for (int i = 0; host->h_aliases[i] != NULL; i++) {
            printf("%s\n", host->h_aliases[i]);
        }
    }
}

/**
 * @function main: Application entry point. Parses CLI arguments and routes
 *                 input to the appropriate resolution strategy.
 *
 * @param argc: Number of command-line arguments.
 * @param argv: Array of command-line argument strings.
 *
 * @return: 0 on success, 1 on argument error.
 */
int main(int argc, char *argv[]) {
    // Validate command-line argument count
    if (argc != 2) {
        printf("Usage: %s <parameter>\n", argv[0]);
        return 1;
    }

    char *param = argv[1];

    // If input consists solely of digits and dots:
    // - Valid IPv4 -> Perform reverse DNS resolution
    // - Invalid IPv4 (e.g., "1.2.3", "259.12.34.12") -> Report not found
    if (isNumericAddress(param)) {
        if (isValidIPv4(param)) {
            resolveIP(param);
        } else {
            printf("Not found information\n");
        }
    } else {
        // Otherwise, treat parameter as a domain name -> Perform forward DNS resolution
        resolveDomain(param);
    }

    return 0;
}
