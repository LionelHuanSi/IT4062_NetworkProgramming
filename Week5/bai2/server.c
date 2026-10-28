#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>

#include "rftp_protocol.h"

#define STORAGE_DIR "storage"

/**
 * @function send_ack: Gửi một gói tin xác nhận RFTP ACK cho một số hiệu khối xác định tới client.
 *
 * @param sockfd: File descriptor của socket UDP.
 * @param dest_addr: Địa chỉ đích của client nhận ACK.
 * @param addr_len: Độ dài cấu trúc địa chỉ client.
 * @param block_num: Số thứ tự khối (block number) cần xác nhận.
 *
 * @return: Không có.
 */
void send_ack(int sockfd, const struct sockaddr_in *dest_addr, socklen_t addr_len, uint32_t block_num) {
    RFTPPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.magic = htons(RFTP_MAGIC);
    pkt.header.opcode = OP_ACK;
    pkt.header.block_number = htonl(block_num);
    pkt.header.payload_len = 0;
    pkt.header.checksum = 0;
    pkt.header.checksum = compute_checksum(&pkt, sizeof(RFTPHeader));

    sendto(sockfd, &pkt, sizeof(RFTPHeader), 0, (const struct sockaddr *)dest_addr, addr_len);
}

/**
 * @function send_error: Gửi một gói tin thông báo lỗi RFTP ERROR kèm mã lỗi và thông điệp tới client.
 *
 * @param sockfd: File descriptor của socket UDP.
 * @param dest_addr: Địa chỉ đích của client nhận thông báo lỗi.
 * @param addr_len: Độ dài cấu trúc địa chỉ client.
 * @param err_code: Mã định danh lỗi (RFTPErrorCode).
 * @param err_msg: Chuỗi thông điệp mô tả chi tiết lỗi.
 *
 * @return: Không có.
 */
void send_error(int sockfd, const struct sockaddr_in *dest_addr, socklen_t addr_len, uint16_t err_code, const char *err_msg) {
    RFTPPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.magic = htons(RFTP_MAGIC);
    pkt.header.opcode = OP_ERROR;
    pkt.header.block_number = htonl(err_code);
    uint16_t len = (uint16_t)strlen(err_msg);
    if (len > RFTP_PAYLOAD_SIZE) len = RFTP_PAYLOAD_SIZE;
    pkt.header.payload_len = htons(len);
    memcpy(pkt.data, err_msg, len);
    pkt.header.checksum = 0;
    pkt.header.checksum = compute_checksum(&pkt, sizeof(RFTPHeader) + len);

    sendto(sockfd, &pkt, sizeof(RFTPHeader) + len, 0, (const struct sockaddr *)dest_addr, addr_len);
}

/**
 * @function handle_upload: Xử lý quy trình nhận tệp tin tải lên từ client (WRQ) theo giao thức Stop-and-Wait ARQ.
 *
 * @param sockfd: File descriptor của socket UDP server.
 * @param client_addr: Địa chỉ socket của client tải lên.
 * @param addr_len: Độ dài cấu trúc địa chỉ client.
 * @param filename: Tên tệp tin được tải lên và lưu vào thư mục storage.
 *
 * @return: Không có.
 */
void handle_upload(int sockfd, const struct sockaddr_in *client_addr, socklen_t addr_len, const char *filename) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_DIR, filename);

    FILE *fp = fopen(filepath, "wb");
    if (fp == NULL) {
        send_error(sockfd, client_addr, addr_len, ERR_ACCESS_DENIED, "Cannot create file on server.");
        return;
    }

    printf("[+] Receiving upload: %s\n", filename);
    send_ack(sockfd, client_addr, addr_len, 0); // ACK(0) acknowledges WRQ

    uint32_t expected_block = 1;
    bool finished = false;

    while (!finished) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(sockfd, &rset);
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };

        int nready = select(sockfd + 1, &rset, NULL, NULL, &tv);
        if (nready <= 0) {
            printf("[-] Timeout waiting for data block %u. Aborting upload.\n", expected_block);
            break;
        }

        RFTPPacket pkt;
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&from_addr, &from_len);
        if (n < (ssize_t)sizeof(RFTPHeader)) continue;

        uint16_t magic = ntohs(pkt.header.magic);
        if (magic != RFTP_MAGIC) continue;

        uint16_t recv_cs = pkt.header.checksum;
        pkt.header.checksum = 0;
        uint16_t calc_cs = compute_checksum(&pkt, (size_t)n);
        if (recv_cs != calc_cs) {
            printf("[!] Checksum mismatch for block %u. Ignoring.\n", ntohl(pkt.header.block_number));
            continue;
        }

        uint32_t block_num = ntohl(pkt.header.block_number);
        uint16_t plen = ntohs(pkt.header.payload_len);

        if (pkt.header.opcode == OP_DATA) {
            if (block_num == expected_block) {
                if (plen > 0) {
                    fwrite(pkt.data, 1, plen, fp);
                }
                send_ack(sockfd, client_addr, addr_len, block_num);
                expected_block++;
            } else if (block_num < expected_block) {
                // Duplicate packet: re-ACK previous block
                send_ack(sockfd, client_addr, addr_len, block_num);
            }
        } else if (pkt.header.opcode == OP_FIN) {
            send_ack(sockfd, client_addr, addr_len, block_num);
            finished = true;
            printf("[+] Successfully received file '%s'\n", filename);
        }
    }

    fclose(fp);
}

/**
 * @function handle_download: Xử lý quy trình gửi tệp tin cho client tải về (RRQ) theo giao thức Stop-and-Wait ARQ.
 *
 * @param sockfd: File descriptor của socket UDP server.
 * @param client_addr: Địa chỉ socket của client tải về.
 * @param addr_len: Độ dài cấu trúc địa chỉ client.
 * @param filename: Tên tệp tin trên máy chủ mà client yêu cầu tải về.
 *
 * @return: Không có.
 */
void handle_download(int sockfd, const struct sockaddr_in *client_addr, socklen_t addr_len, const char *filename) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_DIR, filename);

    FILE *fp = fopen(filepath, "rb");
    if (fp == NULL) {
        send_error(sockfd, client_addr, addr_len, ERR_FILE_NOT_FOUND, "File not found on server.");
        return;
    }

    printf("[+] Processing download request: %s\n", filename);

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

        int retries = 0;
        bool ack_received = false;

        while (retries < RFTP_MAX_RETRIES && !ack_received) {
            sendto(sockfd, &pkt, sizeof(RFTPHeader) + bytes_read, 0, (const struct sockaddr *)client_addr, addr_len);

            fd_set rset;
            FD_ZERO(&rset);
            FD_SET(sockfd, &rset);
            struct timeval tv = { .tv_sec = RFTP_TIMEOUT_SEC, .tv_usec = 0 };

            int nready = select(sockfd + 1, &rset, NULL, NULL, &tv);
            if (nready > 0) {
                RFTPPacket ack_pkt;
                struct sockaddr_in from;
                socklen_t flen = sizeof(from);
                ssize_t n = recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)&from, &flen);
                if (n >= (ssize_t)sizeof(RFTPHeader)) {
                    if (ntohs(ack_pkt.header.magic) == RFTP_MAGIC &&
                        ack_pkt.header.opcode == OP_ACK &&
                        ntohl(ack_pkt.header.block_number) == block_num) {
                        ack_received = true;
                        break;
                    }
                }
            } else {
                retries++;
                printf("[!] Timeout on block %u. Retry %d/%d...\n", block_num, retries, RFTP_MAX_RETRIES);
            }
        }

        if (!ack_received) {
            printf("[-] Max retries reached. Download aborted.\n");
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

    for (int r = 0; r < RFTP_MAX_RETRIES; r++) {
        sendto(sockfd, &fin_pkt, sizeof(RFTPHeader), 0, (const struct sockaddr *)client_addr, addr_len);
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(sockfd, &rset);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        if (select(sockfd + 1, &rset, NULL, NULL, &tv) > 0) {
            break; // Finished
        }
    }

    fclose(fp);
    printf("[+] Successfully sent file '%s'\n", filename);
}

/**
 * @function main: Điểm vào của server RFTP. Khởi tạo thư mục lưu trữ, gắn socket UDP và lắng nghe yêu cầu upload/download.
 *
 * @param argc: Số lượng tham số dòng lệnh.
 * @param argv: Mảng các chuỗi tham số dòng lệnh (Port tùy chọn).
 *
 * @return: 0 khi kết thúc bình thường, 1 nếu gặp lỗi tạo socket hoặc bind port.
 */
int main(int argc, char *argv[]) {
    int port = (argc >= 2) ? atoi(argv[1]) : RFTP_DEFAULT_PORT;
    if (port <= 0 || port > 65535) port = RFTP_DEFAULT_PORT;

    mkdir(STORAGE_DIR, 0755);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        return 1;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons((uint16_t)port);

    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Bind failed");
        close(sockfd);
        return 1;
    }

    printf("==================================================\n");
    printf("  RFTP Reliable UDP File Server\n");
    printf("  Listening on UDP port: %d | Storage: %s/\n", port, STORAGE_DIR);
    printf("==================================================\n");

    while (1) {
        RFTPPacket pkt;
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        ssize_t n = recvfrom(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&client_addr, &addr_len);
        if (n < (ssize_t)sizeof(RFTPHeader)) continue;

        if (ntohs(pkt.header.magic) != RFTP_MAGIC) continue;

        uint16_t plen = ntohs(pkt.header.payload_len);
        char filename[256] = "";
        if (plen > 0) {
            if (plen >= sizeof(filename)) plen = sizeof(filename) - 1;
            memcpy(filename, pkt.data, plen);
            filename[plen] = '\0';
        }

        if (pkt.header.opcode == OP_WRQ) {
            handle_upload(sockfd, &client_addr, addr_len, filename);
        } else if (pkt.header.opcode == OP_RRQ) {
            handle_download(sockfd, &client_addr, addr_len, filename);
        }
    }

    close(sockfd);
    return 0;
}
