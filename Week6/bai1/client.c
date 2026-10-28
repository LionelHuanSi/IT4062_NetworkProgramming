#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define BUFFER_SIZE    4096
#define MAX_PATH_LEN   512

#define OPCODE_ENCRYPT 0
#define OPCODE_DECRYPT 1
#define OPCODE_DATA    2
#define OPCODE_ERROR   3

typedef struct {
    uint8_t opcode;
    uint16_t length;
} __attribute__((packed)) MessageHeader;

/**
 * @function recv_all: Nhận chính xác len bytes từ stream socket TCP,
 *                     xử lý các trường hợp phân mảnh gói tin qua mạng.
 *
 * @param fd: File descriptor của socket kết nối.
 * @param buf: Con trỏ vùng nhớ đệm lưu dữ liệu nhận được.
 * @param len: Tổng số bytes cần nhận.
 *
 * @return: Số bytes thực nhận (len), 0 nếu đối phương đóng kết nối (EOF), hoặc -1 nếu có lỗi.
 */
ssize_t recv_all(int fd, void *buf, size_t len) {
    size_t nLeft = len;
    char *ptr = (char *)buf;
    while (nLeft > 0) {
        ssize_t nRead = recv(fd, ptr, nLeft, 0);
        if (nRead < 0) return -1;
        else if (nRead == 0) return (ssize_t)(len - nLeft);
        nLeft -= nRead;
        ptr += nRead;
    }
    return (ssize_t)len;
}

/**
 * @function send_all: Gửi chính xác len bytes qua stream socket TCP,
 *                     đảm bảo toàn bộ dữ liệu được đẩy đi hết mà không bị sót.
 *
 * @param fd: File descriptor của socket kết nối.
 * @param buf: Con trỏ tới vùng nhớ chứa dữ liệu cần gửi.
 * @param len: Tổng số bytes cần truyền đi.
 *
 * @return: Số bytes đã gửi thành công (len), hoặc -1 nếu có lỗi.
 */
ssize_t send_all(int fd, const void *buf, size_t len) {
    size_t nLeft = len;
    const char *ptr = (const char *)buf;
    while (nLeft > 0) {
        ssize_t nWritten = send(fd, ptr, nLeft, 0);
        if (nWritten <= 0) return -1;
        nLeft -= nWritten;
        ptr += nWritten;
    }
    return (ssize_t)len;
}

/**
 * @function send_header: Đóng gói và gửi tiêu đề thông điệp giao thức (Opcode và Length).
 *
 * @param fd: File descriptor của socket server.
 * @param opcode: Mã thao tác giao thức.
 * @param length: Độ dài payload (Host Byte Order).
 *
 * @return: true nếu gửi thành công, false nếu gặp lỗi.
 */
bool send_header(int fd, uint8_t opcode, uint16_t length) {
    MessageHeader hdr;
    hdr.opcode = opcode;
    hdr.length = htons(length);
    return send_all(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr);
}

/**
 * @function trim_crlf: Xóa bỏ các ký tự xuống dòng ('\r', '\n') ở cuối chuỗi văn bản.
 *
 * @param str: Con trỏ tới chuỗi ký tự cần xử lý.
 *
 * @return: Không có (thao tác trực tiếp trên chuỗi).
 */
void trim_crlf(char *str) {
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r')) {
        str[--len] = '\0';
    }
}

/**
 * @function process_caesar_request: Gửi yêu cầu mã hóa/giải mã cùng khóa, truyền toàn bộ
 *                                   nội dung file nguồn lên server và nhận file kết quả lưu ra đĩa.
 *
 * @param sockfd: File descriptor của socket kết nối server.
 * @param opcode: OPCODE_ENCRYPT (0) hoặc OPCODE_DECRYPT (1).
 * @param src_path: Đường dẫn tệp tin nguồn cần xử lý.
 * @param key: Giá trị số nguyên khóa dịch chuyển Caesar.
 * @param dest_path: Đường dẫn tệp tin lưu kết quả nhận về.
 *
 * @return: true nếu quá trình trao đổi và lưu file thành công, false nếu có lỗi.
 */
bool process_caesar_request(int sockfd, uint8_t opcode, const char *src_path, int key, const char *dest_path) {
    FILE *fin = fopen(src_path, "rb");
    if (fin == NULL) {
        printf("[Lỗi] Không thể mở file nguồn: %s\n", src_path);
        return false;
    }

    // B1: Gửi yêu cầu mã hóa/giải mã kèm giá trị khóa
    char key_str[32];
    snprintf(key_str, sizeof(key_str), "%d", key);
    uint16_t key_len = (uint16_t)strlen(key_str);

    if (!send_header(sockfd, opcode, key_len) ||
        (key_len > 0 && send_all(sockfd, key_str, key_len) != (ssize_t)key_len)) {
        printf("[Lỗi] Không thể gửi tiêu đề yêu cầu tới server.\n");
        fclose(fin);
        return false;
    }

    // B2: Truyền dữ liệu file nguồn lên server
    printf("[*] Đang gửi dữ liệu file '%s' lên server...\n", src_path);
    char buffer[BUFFER_SIZE];
    size_t n_read;
    size_t total_sent = 0;

    while ((n_read = fread(buffer, 1, sizeof(buffer), fin)) > 0) {
        if (!send_header(sockfd, OPCODE_DATA, (uint16_t)n_read) ||
            send_all(sockfd, buffer, n_read) != (ssize_t)n_read) {
            printf("[Lỗi] Lỗi gián đoạn khi truyền dữ liệu file lên server.\n");
            fclose(fin);
            return false;
        }
        total_sent += n_read;
    }
    fclose(fin);

    // Gửi thông điệp kết thúc truyền file: Opcode = 2, Length = 0
    if (!send_header(sockfd, OPCODE_DATA, 0)) {
        printf("[Lỗi] Lỗi gửi tín hiệu kết thúc file lên server.\n");
        return false;
    }
    printf("[+] Đã gửi xong file lên server (%zu bytes). Đang chờ kết quả...\n", total_sent);

    // B3 & B4: Nhận file kết quả từ server và ghi vào dest_path
    FILE *fout = fopen(dest_path, "wb");
    if (fout == NULL) {
        printf("[Lỗi] Không thể tạo file kết quả để ghi: %s\n", dest_path);
        return false;
    }

    size_t total_received = 0;
    bool receive_success = true;

    while (1) {
        MessageHeader resp_hdr;
        if (recv_all(sockfd, &resp_hdr, sizeof(resp_hdr)) != (ssize_t)sizeof(resp_hdr)) {
            printf("[Lỗi] Mất kết nối tới server khi đang nhận dữ liệu.\n");
            receive_success = false;
            break;
        }

        if (resp_hdr.opcode == OPCODE_ERROR) {
            printf("[Lỗi] Server thông báo có lỗi xảy ra trong quá trình xử lý!\n");
            receive_success = false;
            break;
        }

        if (resp_hdr.opcode != OPCODE_DATA) {
            printf("[Lỗi] Nhận được opcode không hợp lệ từ server: %u\n", resp_hdr.opcode);
            receive_success = false;
            break;
        }

        uint16_t chunk_len = ntohs(resp_hdr.length);
        if (chunk_len == 0) {
            // Server hoàn tất truyền file kết quả
            break;
        }

        if (recv_all(sockfd, buffer, chunk_len) != (ssize_t)chunk_len) {
            printf("[Lỗi] Nhận thiếu dữ liệu chunk từ server.\n");
            receive_success = false;
            break;
        }

        if (fwrite(buffer, 1, chunk_len, fout) != chunk_len) {
            printf("[Lỗi] Ghi file cục bộ thất bại.\n");
            receive_success = false;
            break;
        }
        total_received += chunk_len;
    }
    fclose(fout);

    if (receive_success) {
        printf("[Thành công] Đã nhận và lưu file kết quả tại: '%s' (%zu bytes)\n",
               dest_path, total_received);
        return true;
    } else {
        unlink(dest_path);
        return false;
    }
}

/**
 * @function main: Điểm vào của chương trình client mã hóa Caesar. Kết nối tới server,
 *                 hiển thị giao diện menu cho người dùng lựa chọn chức năng mã hóa/giải mã.
 *
 * @param argc: Số lượng tham số dòng lệnh.
 * @param argv: Mảng các chuỗi tham số dòng lệnh (IP_Addr, Port_Number).
 *
 * @return: 0 khi thoát bình thường, 1 nếu tham số hoặc kết nối không hợp lệ.
 */
int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Sử dụng: %s <IP_Addr> <Port_Number>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        printf("Số hiệu cổng không hợp lệ: %s\n", argv[2]);
        return 1;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Lỗi tạo socket");
        return 1;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, argv[1], &servaddr.sin_addr) <= 0) {
        printf("Địa chỉ IP không hợp lệ: %s\n", argv[1]);
        close(sockfd);
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Không thể kết nối tới server");
        close(sockfd);
        return 1;
    }

    printf("Đã kết nối thành công tới server mã hóa Caesar tại %s:%d\n", argv[1], port);

    char input_buf[MAX_PATH_LEN];
    while (1) {
        printf("\n====================================================\n");
        printf("   CHƯƠNG TRÌNH MÃ HÓA CAESAR FILE (TCP CLIENT)\n");
        printf("   Sinh viên: Nguyễn Văn Huấn - MSSV: 20235733\n");
        printf("====================================================\n");
        printf("1. Mã hóa file (Encryption)\n");
        printf("2. Giải mã file (Decryption)\n");
        printf("3. Thoát (Exit)\n");
        printf("Lựa chọn của bạn: ");
        fflush(stdout);

        if (fgets(input_buf, sizeof(input_buf), stdin) == NULL) break;
        trim_crlf(input_buf);
        if (strlen(input_buf) == 0) continue;

        int choice = atoi(input_buf);
        if (choice == 3) {
            printf("Đang thoát chương trình...\n");
            break;
        }

        if (choice != 1 && choice != 2) {
            printf("[Lỗi] Lựa chọn không hợp lệ! Vui lòng chọn 1, 2 hoặc 3.\n");
            continue;
        }

        uint8_t opcode = (choice == 1) ? OPCODE_ENCRYPT : OPCODE_DECRYPT;

        char src_path[MAX_PATH_LEN] = "";
        char dest_path[MAX_PATH_LEN] = "";
        int key = 0;

        printf("Nhập đường dẫn file nguồn: ");
        fflush(stdout);
        if (fgets(src_path, sizeof(src_path), stdin) == NULL) break;
        trim_crlf(src_path);

        if (strlen(src_path) == 0) {
            printf("[Lỗi] Tên file nguồn không được để trống!\n");
            continue;
        }

        printf("Nhập giá trị khóa k (số nguyên): ");
        fflush(stdout);
        if (fgets(input_buf, sizeof(input_buf), stdin) == NULL) break;
        trim_crlf(input_buf);
        key = atoi(input_buf);

        printf("Nhập đường dẫn file lưu kết quả: ");
        fflush(stdout);
        if (fgets(dest_path, sizeof(dest_path), stdin) == NULL) break;
        trim_crlf(dest_path);

        if (strlen(dest_path) == 0) {
            snprintf(dest_path, sizeof(dest_path), "%s_%s",
                     (choice == 1) ? "encrypted" : "decrypted", src_path);
            printf("[Gợi ý] Tự động đặt tên file kết quả: %s\n", dest_path);
        }

        if (!process_caesar_request(sockfd, opcode, src_path, key, dest_path)) {
            printf("[!] Thao tác thất bại. Vui lòng thử lại.\n");
        }
    }

    close(sockfd);
    return 0;
}
