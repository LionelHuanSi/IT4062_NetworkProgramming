#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "rftp_protocol.h"

/**
 * @function get_basename: Trích xuất tên tệp tin cơ sở từ chuỗi đường dẫn (bỏ qua các thư mục cha).
 *
 * @param path: Chuỗi đường dẫn tệp tin (hỗ trợ cả dấu phân cách '/' và '\').
 *
 * @return: Con trỏ tới vị trí bắt đầu tên tệp cơ sở bên trong chuỗi path.
 */
const char *get_basename(const char *path) {
    const char *last = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            last = p + 1;
        }
    }
    return last;
}

/**
 * @function do_upload: Thực hiện tải một tệp tin cục bộ lên máy chủ theo giao thức RFTP Stop-and-Wait ARQ.
 *
 * @param sockfd: File descriptor của socket UDP client.
 * @param servaddr: Con trỏ tới cấu trúc sockaddr_in của máy chủ đích.
 * @param local_path: Đường dẫn tới tệp tin cục bộ cần tải lên.
 *
 * @return: Không có.
 */
void do_upload(int sockfd, const struct sockaddr_in *servaddr, const char *local_path) {
    FILE *fp = fopen(local_path, "rb");
    if (fp == NULL) {
        perror("Cannot open local file for upload");
        return;
    }

    const char *filename = get_basename(local_path);
    uint16_t name_len = (uint16_t)strlen(filename);

    RFTPPacket wrq_pkt;
    memset(&wrq_pkt, 0, sizeof(wrq_pkt));
    wrq_pkt.header.magic = htons(RFTP_MAGIC);
    wrq_pkt.header.opcode = OP_WRQ;
    wrq_pkt.header.block_number = htonl(0);
    wrq_pkt.header.payload_len = htons(name_len);
    memcpy(wrq_pkt.data, filename, name_len);
    wrq_pkt.header.checksum = 0;
    wrq_pkt.header.checksum = compute_checksum(&wrq_pkt, sizeof(RFTPHeader) + name_len);

    printf("[+] Requesting upload of '%s' to server...\n", filename);

    // Send WRQ and wait for ACK(0)
    int retries = 0;
    bool ack_ok = false;
    socklen_t slen = sizeof(struct sockaddr_in);

    while (retries < RFTP_MAX_RETRIES && !ack_ok) {
        sendto(sockfd, &wrq_pkt, sizeof(RFTPHeader) + name_len, 0, (const struct sockaddr *)servaddr, slen);

        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(sockfd, &rset);
        struct timeval tv = { .tv_sec = RFTP_TIMEOUT_SEC, .tv_usec = 0 };

        if (select(sockfd + 1, &rset, NULL, NULL, &tv) > 0) {
            RFTPPacket ack;
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            ssize_t n = recvfrom(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&from, &flen);
            if (n >= (ssize_t)sizeof(RFTPHeader)) {
                if (ntohs(ack.header.magic) == RFTP_MAGIC) {
                    if (ack.header.opcode == OP_ACK && ntohl(ack.header.block_number) == 0) {
                        ack_ok = true;
                        break;
                    } else if (ack.header.opcode == OP_ERROR) {
                        uint16_t elen = ntohs(ack.header.payload_len);
                        printf("[-] Server rejected upload: %.*s\n", elen, ack.data);
                        fclose(fp);
                        return;
                    }
                }
            }
        } else {
            retries++;
            printf("[!] Timeout waiting for WRQ ACK. Retry %d/%d...\n", retries, RFTP_MAX_RETRIES);
        }
    }

    if (!ack_ok) {
        printf("[-] Failed to establish upload session with server.\n");
        fclose(fp);
        return;
    }

    // Stream DATA packets
    uint32_t block_num = 1;
    char buffer[RFTP_PAYLOAD_SIZE];
    bool eof_reached = false;

    while (!eof_reached) {
        size_t bytes_read = fread(buffer, 1, sizeof(buffer), fp);
        if (bytes_read < sizeof(buffer)) {
            eof_reached = true;
        }

        RFTPPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.magic = htons(RFTP_MAGIC);
        pkt.header.opcode = OP_DATA;
        pkt.header.block_number = htonl(block_num);
        pkt.header.payload_len = htons((uint16_t)bytes_read);
        memcpy(pkt.data, buffer, bytes_read);
        pkt.header.checksum = 0;
        pkt.header.checksum = compute_checksum(&pkt, sizeof(RFTPHeader) + bytes_read);

        retries = 0;
        bool block_ack = false;

        while (retries < RFTP_MAX_RETRIES && !block_ack) {
            sendto(sockfd, &pkt, sizeof(RFTPHeader) + bytes_read, 0, (const struct sockaddr *)servaddr, slen);

            fd_set rset;
            FD_ZERO(&rset);
            FD_SET(sockfd, &rset);
            struct timeval tv = { .tv_sec = RFTP_TIMEOUT_SEC, .tv_usec = 0 };

            if (select(sockfd + 1, &rset, NULL, NULL, &tv) > 0) {
                RFTPPacket ack;
                struct sockaddr_in from;
                socklen_t flen = sizeof(from);
                ssize_t n = recvfrom(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&from, &flen);
                if (n >= (ssize_t)sizeof(RFTPHeader)) {
                    if (ntohs(ack.header.magic) == RFTP_MAGIC &&
                        ack.header.opcode == OP_ACK &&
                        ntohl(ack.header.block_number) == block_num) {
                        block_ack = true;
                        break;
                    }
                }
            } else {
                retries++;
                printf("[!] Timeout on block %u. Retrying %d/%d...\n", block_num, retries, RFTP_MAX_RETRIES);
            }
        }

        if (!block_ack) {
            printf("[-] Upload failed: Max retries exceeded on block %u.\n", block_num);
            fclose(fp);
            return;
        }

        block_num++;
    }

    // Send FIN
    RFTPPacket fin_pkt;
    memset(&fin_pkt, 0, sizeof(fin_pkt));
    fin_pkt.header.magic = htons(RFTP_MAGIC);
    fin_pkt.header.opcode = OP_FIN;
    fin_pkt.header.block_number = htonl(block_num);
    fin_pkt.header.payload_len = 0;
    fin_pkt.header.checksum = 0;
    fin_pkt.header.checksum = compute_checksum(&fin_pkt, sizeof(RFTPHeader));

    sendto(sockfd, &fin_pkt, sizeof(RFTPHeader), 0, (const struct sockaddr *)servaddr, slen);

    fclose(fp);
    printf("[+] Upload completed successfully for '%s'\n", filename);
}

/**
 * @function do_download: Thực hiện yêu cầu và tải một tệp tin từ máy chủ về máy cục bộ theo giao thức RFTP Stop-and-Wait ARQ.
 *
 * @param sockfd: File descriptor của socket UDP client.
 * @param servaddr: Con trỏ tới cấu trúc sockaddr_in của máy chủ đích.
 * @param filename: Tên tệp tin trên máy chủ cần tải về.
 *
 * @return: Không có.
 */
void do_download(int sockfd, const struct sockaddr_in *servaddr, const char *filename) {
    uint16_t name_len = (uint16_t)strlen(filename);

    RFTPPacket rrq_pkt;
    memset(&rrq_pkt, 0, sizeof(rrq_pkt));
    rrq_pkt.header.magic = htons(RFTP_MAGIC);
    rrq_pkt.header.opcode = OP_RRQ;
    rrq_pkt.header.block_number = htonl(0);
    rrq_pkt.header.payload_len = htons(name_len);
    memcpy(rrq_pkt.data, filename, name_len);
    rrq_pkt.header.checksum = 0;
    rrq_pkt.header.checksum = compute_checksum(&rrq_pkt, sizeof(RFTPHeader) + name_len);

    printf("[+] Requesting download of '%s' from server...\n", filename);

    socklen_t slen = sizeof(struct sockaddr_in);
    sendto(sockfd, &rrq_pkt, sizeof(RFTPHeader) + name_len, 0, (const struct sockaddr *)servaddr, slen);

    char local_dest[512];
    snprintf(local_dest, sizeof(local_dest), "downloaded_%s", filename);

    FILE *fp = NULL;
    uint32_t expected_block = 1;
    bool finished = false;

    while (!finished) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(sockfd, &rset);
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };

        int nready = select(sockfd + 1, &rset, NULL, NULL, &tv);
        if (nready <= 0) {
            printf("[-] Timeout waiting for file data from server.\n");
            break;
        }

        RFTPPacket pkt;
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &flen);
        if (n < (ssize_t)sizeof(RFTPHeader)) continue;

        if (ntohs(pkt.header.magic) != RFTP_MAGIC) continue;

        if (pkt.header.opcode == OP_ERROR) {
            uint16_t elen = ntohs(pkt.header.payload_len);
            printf("[-] Server reported error: %.*s\n", elen, pkt.data);
            if (fp) { fclose(fp); unlink(local_dest); }
            return;
        }

        uint16_t recv_cs = pkt.header.checksum;
        pkt.header.checksum = 0;
        if (compute_checksum(&pkt, (size_t)n) != recv_cs) {
            printf("[!] Corrupted packet received. Ignoring.\n");
            continue;
        }

        uint32_t block_num = ntohl(pkt.header.block_number);
        uint16_t plen = ntohs(pkt.header.payload_len);

        if (pkt.header.opcode == OP_DATA) {
            if (fp == NULL) {
                fp = fopen(local_dest, "wb");
                if (fp == NULL) {
                    perror("Failed to open local destination file");
                    return;
                }
            }

            if (block_num == expected_block) {
                if (plen > 0) {
                    fwrite(pkt.data, 1, plen, fp);
                }

                // Send ACK
                RFTPPacket ack;
                memset(&ack, 0, sizeof(ack));
                ack.header.magic = htons(RFTP_MAGIC);
                ack.header.opcode = OP_ACK;
                ack.header.block_number = htonl(block_num);
                ack.header.payload_len = 0;
                ack.header.checksum = 0;
                ack.header.checksum = compute_checksum(&ack, sizeof(RFTPHeader));
                sendto(sockfd, &ack, sizeof(RFTPHeader), 0, (const struct sockaddr *)servaddr, slen);

                expected_block++;
            } else if (block_num < expected_block) {
                // Re-send ACK for duplicate block
                RFTPPacket ack;
                memset(&ack, 0, sizeof(ack));
                ack.header.magic = htons(RFTP_MAGIC);
                ack.header.opcode = OP_ACK;
                ack.header.block_number = htonl(block_num);
                ack.header.payload_len = 0;
                ack.header.checksum = 0;
                ack.header.checksum = compute_checksum(&ack, sizeof(RFTPHeader));
                sendto(sockfd, &ack, sizeof(RFTPHeader), 0, (const struct sockaddr *)servaddr, slen);
            }
        } else if (pkt.header.opcode == OP_FIN) {
            // Send FIN ACK
            RFTPPacket ack;
            memset(&ack, 0, sizeof(ack));
            ack.header.magic = htons(RFTP_MAGIC);
            ack.header.opcode = OP_ACK;
            ack.header.block_number = htonl(block_num);
            ack.header.payload_len = 0;
            ack.header.checksum = 0;
            ack.header.checksum = compute_checksum(&ack, sizeof(RFTPHeader));
            sendto(sockfd, &ack, sizeof(RFTPHeader), 0, (const struct sockaddr *)servaddr, slen);

            finished = true;
            printf("[+] Download completed successfully. Saved to: %s\n", local_dest);
        }
    }

    if (fp) fclose(fp);
}

/**
 * @function main: Điểm vào của client RFTP. Kiểm tra tham số dòng lệnh và kích hoạt tiến trình upload hoặc download.
 *
 * @param argc: Số lượng tham số dòng lệnh.
 * @param argv: Mảng tham số dòng lệnh (<Server_IP> <Port> <upload|download> <filename>).
 *
 * @return: 0 khi thành công, 1 khi gặp lỗi tham số hoặc lỗi socket.
 */
int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: %s <Server_IP> <Port> <upload|download> <filename>\n", argv[0]);
        printf("Examples:\n");
        printf("  %s 127.0.0.1 6600 upload sample.txt\n", argv[0]);
        printf("  %s 127.0.0.1 6600 download sample.txt\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    const char *action = argv[3];
    const char *target_file = argv[4];

    if (port <= 0 || port > 65535) {
        printf("Invalid port number: %s\n", argv[2]);
        return 1;
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        return 1;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, server_ip, &servaddr.sin_addr) <= 0) {
        printf("Invalid IP address: %s\n", server_ip);
        close(sockfd);
        return 1;
    }

    if (strcmp(action, "upload") == 0) {
        do_upload(sockfd, &servaddr, target_file);
    } else if (strcmp(action, "download") == 0) {
        do_download(sockfd, &servaddr, target_file);
    } else {
        printf("Unknown action: %s (Must be 'upload' or 'download')\n", action);
    }

    close(sockfd);
    return 0;
}
