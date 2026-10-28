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
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>

#define BACKLOG        20
#define BUFFER_SIZE    4096
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
 * @function handle_client: Hàm thực thi của worker thread. Tiếp nhận yêu cầu mã hóa/giải mã,
 *                          nhận file tạm, thực hiện thuật toán Caesar và truyền trả kết quả.
 *
 * @param arg: Con trỏ cấp phát động chứa socket descriptor của client.
 *
 * @return: NULL khi kết thúc luồng.
 */
void *handle_client(void *arg) {
    int connfd = *((int *)arg);
    free(arg);
    pthread_detach(pthread_self());

    printf("[+] Thread %lu bắt đầu phục vụ client trên socket %d\n",
           (unsigned long)pthread_self(), connfd);

    char temp_in[256];
    char temp_out[256];
    snprintf(temp_in, sizeof(temp_in), "%s/temp_in_%lu_%d.tmp",
             STORAGE_DIR, (unsigned long)pthread_self(), connfd);
    snprintf(temp_out, sizeof(temp_out), "%s/temp_out_%lu_%d.tmp",
             STORAGE_DIR, (unsigned long)pthread_self(), connfd);

    while (1) {
        // B1: Nhận yêu cầu mã hóa/giải mã kèm khóa từ client
        MessageHeader req_hdr;
        ssize_t ret = recv_all(connfd, &req_hdr, sizeof(req_hdr));
        if (ret <= 0) {
            printf("[-] Client trên socket %d đã ngắt kết nối.\n", connfd);
            break;
        }

        uint8_t opcode = req_hdr.opcode;
        uint16_t key_len = ntohs(req_hdr.length);

        if (opcode != OPCODE_ENCRYPT && opcode != OPCODE_DECRYPT) {
            fprintf(stderr, "[!] Socket %d gửi opcode không hợp lệ: %u\n", connfd, opcode);
            send_header(connfd, OPCODE_ERROR, 0);
            break;
        }

        char key_str[64] = "";
        if (key_len > 0) {
            if (key_len >= sizeof(key_str)) key_len = sizeof(key_str) - 1;
            if (recv_all(connfd, key_str, key_len) != (ssize_t)key_len) {
                send_header(connfd, OPCODE_ERROR, 0);
                break;
            }
            key_str[key_len] = '\0';
        }

        int raw_key = atoi(key_str);
        int shift = (opcode == OPCODE_ENCRYPT) ? raw_key : -raw_key;

        printf("[*] [Socket %d] Yêu cầu: %s, Khóa: %d\n",
               connfd, (opcode == OPCODE_ENCRYPT) ? "Mã hóa" : "Giải mã", raw_key);

        // B2: Nhận dữ liệu file từ client và lưu vào file tạm
        FILE *fin_tmp = fopen(temp_in, "wb");
        if (fin_tmp == NULL) {
            perror("Không thể tạo file tạm đầu vào");
            send_header(connfd, OPCODE_ERROR, 0);
            break;
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
                // Nhận xong toàn bộ file
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
            fprintf(stderr, "[!] [Socket %d] Lỗi nhận dữ liệu file từ client.\n", connfd);
            unlink(temp_in);
            send_header(connfd, OPCODE_ERROR, 0);
            break;
        }

        // B3: Xử lý mã hóa / giải mã file tạm ra file kết quả
        if (!caesar_cipher_file(temp_in, temp_out, shift)) {
            fprintf(stderr, "[!] [Socket %d] Lỗi trong quá trình xử lý Caesar.\n", connfd);
            unlink(temp_in);
            unlink(temp_out);
            send_header(connfd, OPCODE_ERROR, 0);
            break;
        }

        // B4: Gửi dữ liệu của file kết quả về client
        FILE *fout_tmp = fopen(temp_out, "rb");
        if (fout_tmp == NULL) {
            perror("Không thể mở file tạm kết quả để gửi");
            unlink(temp_in);
            unlink(temp_out);
            send_header(connfd, OPCODE_ERROR, 0);
            break;
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
            // Gửi gói kết thúc truyền file: Opcode = 2, Length = 0
            send_header(connfd, OPCODE_DATA, 0);
            printf("[+] [Socket %d] Hoàn tất truyền file kết quả cho client.\n", connfd);
        }

        // B5: Xóa file tạm và file kết quả
        unlink(temp_in);
        unlink(temp_out);

        if (!send_ok) break;
    }

    close(connfd);
    unlink(temp_in);
    unlink(temp_out);
    printf("[-] Luồng phục vụ socket %d kết thúc.\n", connfd);
    return NULL;
}

/**
 * @function main: Điểm vào của server mã hóa Caesar đa luồng. Kiểm tra tham số cổng,
 *                 khởi tạo socket lắng nghe và tạo worker thread cho mỗi client kết nối.
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
    printf("  Server mã hóa/giải mã Caesar đa luồng (Multi-threaded)\n");
    printf("  Sinh viên: Nguyễn Văn Huấn - MSSV: 20235733\n");
    printf("  Lắng nghe trên cổng: %d | Thư mục tạm: %s/\n", port, STORAGE_DIR);
    printf("========================================================\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t sin_size = sizeof(client_addr);
        int *connfd = malloc(sizeof(int));
        if (connfd == NULL) {
            perror("Lỗi cấp phát bộ nhớ connfd");
            continue;
        }

        *connfd = accept(listen_sock, (struct sockaddr *)&client_addr, &sin_size);
        if (*connfd < 0) {
            if (errno == EINTR) {
                free(connfd);
                continue;
            }
            perror("Lỗi chấp nhận kết nối (accept)");
            free(connfd);
            break;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("[+] Kết nối mới từ [%s:%d] -> Socket: %d\n",
               client_ip, ntohs(client_addr.sin_port), *connfd);

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, connfd) != 0) {
            perror("Lỗi tạo luồng pthread_create");
            close(*connfd);
            free(connfd);
        }
    }

    close(listen_sock);
    return 0;
}
