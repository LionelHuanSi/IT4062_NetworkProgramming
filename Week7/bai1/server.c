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
#include <poll.h>
#include <sys/stat.h>
#include <errno.h>

#define BACKLOG        20
#define BUFFER_SIZE    4096
#define MAX_CLIENTS    1024
#define STORAGE_DIR    "server_storage"

#define OPCODE_ENCRYPT 0
#define OPCODE_DECRYPT 1
#define OPCODE_DATA    2
#define OPCODE_ERROR   3

// Cấu trúc tiêu đề thông điệp ứng dụng (3 bytes)
typedef struct {
    uint8_t opcode;
    uint16_t length;
} __attribute__((packed)) MessageHeader;

/**
 * @function recv_all: Nhận chính xác len bytes từ stream socket TCP,
 *                     xử lý phân mảnh luồng dữ liệu mạng.
 *
 * @param fd: File descriptor của socket đang kết nối.
 * @param buf: Con trỏ tới vùng nhớ đệm nhận dữ liệu.
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
 *                     xử lý trường hợp bộ đệm gửi bị đầy hoặc gửi từng phần.
 *
 * @param fd: File descriptor của socket đang kết nối.
 * @param buf: Con trỏ tới vùng nhớ chứa dữ liệu cần truyền đi.
 * @param len: Tổng số bytes cần gửi.
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
 * @function send_header: Đóng gói và gửi tiêu đề thông điệp (Opcode và Length) tới socket.
 *
 * @param fd: File descriptor của socket đối tác.
 * @param opcode: Mã thao tác giao thức (0: Encrypt, 1: Decrypt, 2: Data, 3: Error).
 * @param length: Độ dài của payload đi kèm theo thứ tự Host Byte Order.
 *
 * @return: true nếu gửi thành công, false nếu socket bị lỗi.
 */
bool send_header(int fd, uint8_t opcode, uint16_t length) {
    MessageHeader hdr;
    hdr.opcode = opcode;
    hdr.length = htons(length);
    return send_all(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr);
}

/**
 * @function caesar_cipher_file: Đọc dữ liệu từ file nguồn, thực hiện dịch chuyển Caesar
 *                               cho các ký tự chữ cái và ghi kết quả ra file đích.
 *
 * @param in_path: Đường dẫn tệp tin nguồn cần xử lý.
 * @param out_path: Đường dẫn tệp tin kết quả xuất ra.
 * @param shift: Giá trị độ dịch Caesar (đã tính toán chiều âm cho giải mã).
 *
 * @return: true nếu xử lý và ghi file thành công, false nếu gặp lỗi I/O.
 */
bool caesar_cipher_file(const char *in_path, const char *out_path, int shift) {
    FILE *fin = fopen(in_path, "rb");
    if (fin == NULL) return false;

    FILE *fout = fopen(out_path, "wb");
    if (fout == NULL) {
        fclose(fin);
        return false;
    }

    int normalized_shift = ((shift % 26) + 26) % 26;
    unsigned char buffer[BUFFER_SIZE];
    size_t bytes_read;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fin)) > 0) {
        for (size_t i = 0; i < bytes_read; i++) {
            unsigned char c = buffer[i];
            if (c >= 'a' && c <= 'z') {
                buffer[i] = (unsigned char)('a' + (c - 'a' + normalized_shift) % 26);
            } else if (c >= 'A' && c <= 'Z') {
                buffer[i] = (unsigned char)('A' + (c - 'A' + normalized_shift) % 26);
            }
        }
        if (fwrite(buffer, 1, bytes_read, fout) != bytes_read) {
            fclose(fin);
            fclose(fout);
            return false;
        }
    }

    fclose(fin);
    fclose(fout);
    return true;
}

/**
 * @function handle_client_request: Xử lý một yêu cầu mã hóa/giải mã từ client theo đúng
 *                                  các bước B1 đến B5 của giao thức được quy định trong đề bài.
 *
 * @param connfd: File descriptor của socket client đã sẵn sàng dữ liệu (POLLIN).
 *
 * @return: 1 nếu xử lý thành công yêu cầu, 0 nếu client đã ngắt kết nối (EOF), -1 nếu gặp lỗi mạng.
 */
int handle_client_request(int connfd) {
    char temp_in[256];
    char temp_out[256];
    snprintf(temp_in, sizeof(temp_in), "%s/temp_in_%d.tmp", STORAGE_DIR, connfd);
    snprintf(temp_out, sizeof(temp_out), "%s/temp_out_%d.tmp", STORAGE_DIR, connfd);

    // B1: Nhận yêu cầu mã hóa/giải mã kèm khóa từ client
    MessageHeader req_hdr;
    ssize_t ret = recv_all(connfd, &req_hdr, sizeof(req_hdr));
    if (ret <= 0) {
        return 0; // Client ngắt kết nối
    }

    uint8_t opcode = req_hdr.opcode;
    uint16_t key_len = ntohs(req_hdr.length);

    if (opcode != OPCODE_ENCRYPT && opcode != OPCODE_DECRYPT) {
        fprintf(stderr, "[!] [Socket %d] Opcode không hợp lệ: %u\n", connfd, opcode);
        send_header(connfd, OPCODE_ERROR, 0);
        return -1;
    }

    char key_str[64] = "";
    if (key_len > 0) {
        if (key_len >= sizeof(key_str)) key_len = sizeof(key_str) - 1;
        if (recv_all(connfd, key_str, key_len) != (ssize_t)key_len) {
            send_header(connfd, OPCODE_ERROR, 0);
            return -1;
        }
        key_str[key_len] = '\0';
    }

    int raw_key = atoi(key_str);
    int shift = (opcode == OPCODE_ENCRYPT) ? raw_key : -raw_key;

    printf("[*] [Socket %d] B1: Yêu cầu %s, Khóa: %d\n",
           connfd, (opcode == OPCODE_ENCRYPT) ? "MÃ HÓA" : "GIẢI MÃ", raw_key);

    // B2: Nhận dữ liệu file từ client và lưu vào file tạm
    FILE *fin_tmp = fopen(temp_in, "wb");
    if (fin_tmp == NULL) {
        perror("Không thể tạo file tạm đầu vào");
        send_header(connfd, OPCODE_ERROR, 0);
        return -1;
    }

    bool receive_ok = true;
    while (1) {
        MessageHeader chunk_hdr;
        if (recv_all(connfd, &chunk_hdr, sizeof(chunk_hdr)) != (ssize_t)sizeof(chunk_hdr)) {
            receive_ok = false;
            break;
        }

        if (chunk_hdr.opcode != OPCODE_DATA) {
            receive_ok = false;
            break;
        }

        uint16_t chunk_len = ntohs(chunk_hdr.length);
        if (chunk_len == 0) {
            // Nhận xong toàn bộ dữ liệu file (Opcode = 2, Length = 0)
            break;
        }

        char chunk_buf[BUFFER_SIZE];
        if (recv_all(connfd, chunk_buf, chunk_len) != (ssize_t)chunk_len) {
            receive_ok = false;
            break;
        }

        if (fwrite(chunk_buf, 1, chunk_len, fin_tmp) != chunk_len) {
            receive_ok = false;
            break;
        }
    }
    fclose(fin_tmp);

    if (!receive_ok) {
        fprintf(stderr, "[!] [Socket %d] Lỗi khi nhận dữ liệu file từ client.\n", connfd);
        unlink(temp_in);
        send_header(connfd, OPCODE_ERROR, 0);
        return -1;
    }

    // B3: Xử lý mã hóa/giải mã file tạm ra file kết quả
    if (!caesar_cipher_file(temp_in, temp_out, shift)) {
        fprintf(stderr, "[!] [Socket %d] Lỗi trong quá trình xử lý Caesar.\n", connfd);
        unlink(temp_in);
        unlink(temp_out);
        send_header(connfd, OPCODE_ERROR, 0);
        return -1;
    }

    // B4: Gửi dữ liệu của file kết quả về client
    FILE *fout_tmp = fopen(temp_out, "rb");
    if (fout_tmp == NULL) {
        perror("Không thể mở file kết quả để gửi");
        unlink(temp_in);
        unlink(temp_out);
        send_header(connfd, OPCODE_ERROR, 0);
        return -1;
    }

    bool send_ok = true;
    char send_buf[BUFFER_SIZE];
    size_t n_read;
    while ((n_read = fread(send_buf, 1, sizeof(send_buf), fout_tmp)) > 0) {
        if (!send_header(connfd, OPCODE_DATA, (uint16_t)n_read)) {
            send_ok = false;
            break;
        }
        if (send_all(connfd, send_buf, n_read) != (ssize_t)n_read) {
            send_ok = false;
            break;
        }
    }
    fclose(fout_tmp);

    if (send_ok) {
        // Gửi thông điệp kết thúc truyền file: Opcode = 2, Length = 0
        send_header(connfd, OPCODE_DATA, 0);
        printf("[+] [Socket %d] B4: Đã gửi xong file kết quả về cho client.\n", connfd);
    }

    // B5: Xóa file tạm và file kết quả
    unlink(temp_in);
    unlink(temp_out);

    return send_ok ? 1 : -1;
}

/**
 * @function main: Điểm vào của server mã hóa Caesar sử dụng cơ chế I/O multiplexing với poll().
 *                 Quản lý danh sách các socket descriptor kết nối và điều phối sự kiện đọc/ghi.
 *
 * @param argc: Số lượng tham số dòng lệnh.
 * @param argv: Mảng các chuỗi tham số dòng lệnh (Port_Number).
 *
 * @return: 0 nếu kết thúc bình thường, 1 nếu gặp lỗi khởi tạo socket.
 */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Sử dụng: %s <Port_Number>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        printf("Số hiệu cổng không hợp lệ: %s\n", argv[1]);
        return 1;
    }

    mkdir(STORAGE_DIR, 0755);

    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        perror("Lỗi tạo socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((uint16_t)port);

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Lỗi gán cổng (bind)");
        close(listen_sock);
        return 1;
    }

    if (listen(listen_sock, BACKLOG) < 0) {
        perror("Lỗi lắng nghe (listen)");
        close(listen_sock);
        return 1;
    }

    printf("========================================================\n");
    printf("  Server mã hóa/giải mã Caesar I/O Multiplexing (poll)\n");
    printf("  Sinh viên: Nguyễn Văn Huấn - MSSV: 20235733\n");
    printf("  Lắng nghe trên cổng: %d | Thư mục tạm: %s/\n", port, STORAGE_DIR);
    printf("========================================================\n");

    // Khởi tạo mảng pollfd
    struct pollfd fds[MAX_CLIENTS];
    fds[0].fd = listen_sock;
    fds[0].events = POLLIN;

    for (int i = 1; i < MAX_CLIENTS; i++) {
        fds[i].fd = -1;
    }

    int nfds = 1;

    while (1) {
        int nready = poll(fds, nfds, -1);
        if (nready < 0) {
            if (errno == EINTR) continue;
            perror("Lỗi hàm poll()");
            break;
        }

        // 1. Kiểm tra kết nối mới trên listen_sock
        if (fds[0].revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t sin_size = sizeof(client_addr);
            int conn_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &sin_size);
            if (conn_sock >= 0) {
                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                int client_port = ntohs(client_addr.sin_port);

                int i;
                for (i = 1; i < MAX_CLIENTS; i++) {
                    if (fds[i].fd < 0) {
                        fds[i].fd = conn_sock;
                        fds[i].events = POLLIN;
                        break;
                    }
                }

                if (i == MAX_CLIENTS) {
                    fprintf(stderr, "Đã đạt giới hạn tối đa số lượng client kết nối.\n");
                    close(conn_sock);
                } else {
                    if (i >= nfds) {
                        nfds = i + 1;
                    }
                    printf("[+] Kết nối mới từ [%s:%d] -> Socket: %d (slot: %d)\n",
                           client_ip, client_port, conn_sock, i);
                }
            }
            if (--nready <= 0) continue;
        }

        // 2. Kiểm tra dữ liệu sẵn sàng trên các socket client
        for (int i = 1; i < nfds; i++) {
            if (fds[i].fd < 0) continue;

            if (fds[i].revents & (POLLIN | POLLERR | POLLHUP)) {
                int client_fd = fds[i].fd;
                int res = handle_client_request(client_fd);

                if (res <= 0) {
                    if (res == 0) {
                        printf("[-] Client trên socket %d đã ngắt kết nối (slot: %d).\n", client_fd, i);
                    } else {
                        printf("[!] Gặp lỗi mạng với client trên socket %d. Đóng kết nối.\n", client_fd);
                    }
                    close(client_fd);
                    fds[i].fd = -1;

                    // Thu gọn nfds nếu socket bị đóng là socket ở cuối mảng
                    while (nfds > 1 && fds[nfds - 1].fd < 0) {
                        nfds--;
                    }
                }

                if (--nready <= 0) break;
            }
        }
    }

    close(listen_sock);
    return 0;
}
