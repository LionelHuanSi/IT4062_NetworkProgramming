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
#include <sys/wait.h>
#include <sys/file.h>
#include <errno.h>
#include <signal.h>

#define DEFAULT_ACCOUNT_FILE "account.txt"
#define MAX_USERNAME_LEN     64
#define MAX_PASSWORD_LEN     64
#define MAX_MESSAGE_LEN      512
#define MAX_LOGIN_ATTEMPTS   3
#define BACKLOG              20

#define STATUS_LOCKED        0
#define STATUS_ACTIVE        1

typedef enum {
    CMD_LOGIN  = 1,
    CMD_LOGOUT = 2,
    CMD_EXIT   = 3
} CommandType;

typedef enum {
    STATUS_SUCCESS            = 200,
    STATUS_BAD_REQUEST        = 400,
    STATUS_PASSWORD_INCORRECT = 401,
    STATUS_ACCOUNT_LOCKED     = 403,
    STATUS_ACCOUNT_NOT_FOUND  = 404,
    STATUS_ALREADY_LOGGED_IN  = 409,
    STATUS_NOT_LOGGED_IN      = 412,
    STATUS_SERVER_ERROR       = 500
} ResponseStatus;

typedef struct {
    uint32_t command;
    uint32_t payload_len;
} __attribute__((packed)) RequestHeader;

typedef struct {
    uint32_t status_code;
    uint32_t payload_len;
} __attribute__((packed)) ResponseHeader;

/**
 * @function sig_chld: Bộ xử lý tín hiệu SIGCHLD thu dọn tiến trình con khi kết thúc,
 *                     sử dụng waitpid không chặn để ngăn ngừa hoàn toàn tiến trình zombie.
 *
 * @param signo: Số hiệu tín hiệu nhận được (SIGCHLD).
 *
 * @return: Không có.
 */
void sig_chld(int signo) {
    (void)signo;
    pid_t pid;
    int stat;
    while ((pid = waitpid(-1, &stat, WNOHANG)) > 0) {
        printf("[Server] Tiến trình con PID %d đã kết thúc và được thu dọn.\n", pid);
    }
}

/**
 * @function recv_all: Nhận chính xác len bytes từ stream socket TCP,
 *                     xử lý các trường hợp nhận thiếu do phân mảnh gói tin.
 *
 * @param fd: File descriptor của socket kết nối.
 * @param buf: Con trỏ tới bộ đệm lưu trữ dữ liệu nhận được.
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
 *                     xử lý các trường hợp gửi từng phần và giới hạn bộ đệm socket.
 *
 * @param fd: File descriptor của socket kết nối.
 * @param buf: Con trỏ tới bộ đệm chứa dữ liệu cần gửi.
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
 * @function send_response: Đóng gói và gửi phản hồi (ResponseHeader + payload) tới client.
 *
 * @param fd: File descriptor của socket client.
 * @param status: Mã trạng thái phản hồi (ResponseStatus).
 * @param msg: Chuỗi thông báo gửi kèm.
 *
 * @return: true nếu gửi thành công, false nếu socket bị lỗi.
 */
bool send_response(int fd, ResponseStatus status, const char *msg) {
    uint32_t msg_len = (msg != NULL) ? (uint32_t)strlen(msg) : 0;
    ResponseHeader resp;
    resp.status_code = htonl((uint32_t)status);
    resp.payload_len = htonl(msg_len);

    if (send_all(fd, &resp, sizeof(resp)) != (ssize_t)sizeof(resp)) return false;
    if (msg_len > 0) {
        if (send_all(fd, msg, msg_len) != (ssize_t)msg_len) return false;
    }
    return true;
}

/**
 * @function check_account: Đọc tệp account.txt có khóa chia sẻ (shared lock),
 *                          kiểm tra thông tin tài khoản, mật khẩu và trạng thái hoạt động.
 *
 * @param username: Tên người dùng cần kiểm tra.
 * @param password: Mật khẩu người dùng nhập vào.
 * @param out_found: Con trỏ cờ báo tìm thấy tài khoản (true/false).
 * @param out_pass_match: Con trỏ cờ báo mật khẩu trùng khớp (true/false).
 * @param out_status: Con trỏ nhận trạng thái tài khoản (1: Hoạt động, 0: Bị khóa).
 *
 * @return: true nếu mở và đọc file thành công, false nếu gặp lỗi I/O.
 */
bool check_account(const char *username, const char *password,
                   bool *out_found, bool *out_pass_match, int *out_status) {
    *out_found = false;
    *out_pass_match = false;
    *out_status = STATUS_LOCKED;

    FILE *fp = fopen(DEFAULT_ACCOUNT_FILE, "r");
    if (fp == NULL) {
        perror("Không thể mở tệp account.txt");
        return false;
    }

    // Áp dụng Shared Lock để tránh xung đột khi tiến trình khác đang ghi
    flock(fileno(fp), LOCK_SH);

    char u[MAX_USERNAME_LEN];
    char p[MAX_PASSWORD_LEN];
    int st;

    while (fscanf(fp, "%63s %63s %d", u, p, &st) == 3) {
        if (strcmp(u, username) == 0) {
            *out_found = true;
            *out_status = st;
            if (strcmp(p, password) == 0) {
                *out_pass_match = true;
            }
            break;
        }
    }

    flock(fileno(fp), LOCK_UN);
    fclose(fp);
    return true;
}

/**
 * @function lock_account: Cập nhật trạng thái tài khoản thành bị khóa (status = 0)
 *                         vào tệp account.txt sử dụng Exclusive Lock và ghi nguyên tử qua file tạm.
 *
 * @param username: Tên người dùng cần khóa tài khoản.
 *
 * @return: true nếu cập nhật thành công, false nếu có lỗi I/O.
 */
bool lock_account(const char *username) {
    FILE *fp = fopen(DEFAULT_ACCOUNT_FILE, "r+");
    if (fp == NULL) {
        perror("Lỗi mở account.txt để khóa tài khoản");
        return false;
    }

    // Khóa độc quyền tệp tin (Exclusive Lock) liên tiến trình
    flock(fileno(fp), LOCK_EX);

    char tmp_file[256];
    snprintf(tmp_file, sizeof(tmp_file), "%s.tmp.%d", DEFAULT_ACCOUNT_FILE, getpid());
    FILE *f_tmp = fopen(tmp_file, "w");
    if (f_tmp == NULL) {
        flock(fileno(fp), LOCK_UN);
        fclose(fp);
        return false;
    }

    char u[MAX_USERNAME_LEN];
    char p[MAX_PASSWORD_LEN];
    int st;
    bool updated = false;

    rewind(fp);
    while (fscanf(fp, "%63s %63s %d", u, p, &st) == 3) {
        if (strcmp(u, username) == 0) {
            st = STATUS_LOCKED;
            updated = true;
        }
        fprintf(f_tmp, "%s %s %d\n", u, p, st);
    }

    fclose(f_tmp);
    if (updated) {
        rename(tmp_file, DEFAULT_ACCOUNT_FILE);
    } else {
        unlink(tmp_file);
    }

    flock(fileno(fp), LOCK_UN);
    fclose(fp);
    return updated;
}

/**
 * @function handle_client: Hàm xử lý phiên giao tiếp với client trong tiến trình con (Child Process).
 *                          Tiếp nhận và thực thi các lệnh đăng nhập, đăng xuất và tự động khóa sau 3 lần sai.
 *
 * @param conn_sock: File descriptor của socket client đã kết nối.
 *
 * @return: Không có (tiến trình con sẽ thoát khi hoàn thành).
 */
void handle_client(int conn_sock) {
    printf("[PID %d] Bắt đầu phục vụ client trên socket %d\n", getpid(), conn_sock);

    bool is_logged_in = false;
    char current_user[MAX_USERNAME_LEN] = "";
    int failed_attempts = 0;
    char last_attempt_user[MAX_USERNAME_LEN] = "";

    while (1) {
        RequestHeader req;
        ssize_t ret = recv_all(conn_sock, &req, sizeof(req));
        if (ret <= 0) {
            printf("[PID %d] Client trên socket %d đã đóng kết nối.\n", getpid(), conn_sock);
            break;
        }

        uint32_t cmd = ntohl(req.command);
        uint32_t payload_len = ntohl(req.payload_len);

        char payload[MAX_MESSAGE_LEN] = "";
        if (payload_len > 0) {
            if (payload_len >= sizeof(payload)) {
                send_response(conn_sock, STATUS_BAD_REQUEST, "Payload quá lớn.");
                break;
            }
            if (recv_all(conn_sock, payload, payload_len) != (ssize_t)payload_len) {
                break;
            }
            payload[payload_len] = '\0';
        }

        switch (cmd) {
            case CMD_LOGIN: {
                if (is_logged_in) {
                    char msg[MAX_MESSAGE_LEN];
                    snprintf(msg, sizeof(msg),
                             "Cửa sổ này đã đăng nhập với tài khoản '%s'. Vui lòng đăng xuất trước.",
                             current_user);
                    send_response(conn_sock, STATUS_ALREADY_LOGGED_IN, msg);
                    break;
                }

                char user[MAX_USERNAME_LEN] = "";
                char pass[MAX_PASSWORD_LEN] = "";
                const char *delim = strchr(payload, '\n');
                if (delim != NULL) {
                    size_t ulen = (size_t)(delim - payload);
                    if (ulen >= sizeof(user)) ulen = sizeof(user) - 1;
                    strncpy(user, payload, ulen);
                    user[ulen] = '\0';

                    strncpy(pass, delim + 1, sizeof(pass) - 1);
                    pass[sizeof(pass) - 1] = '\0';
                } else {
                    send_response(conn_sock, STATUS_BAD_REQUEST, "Định dạng đăng nhập không hợp lệ.");
                    break;
                }

                bool found = false, pass_match = false;
                int status = STATUS_LOCKED;
                if (!check_account(user, pass, &found, &pass_match, &status)) {
                    send_response(conn_sock, STATUS_SERVER_ERROR, "Lỗi đọc dữ liệu tài khoản trên server.");
                    break;
                }

                if (!found) {
                    send_response(conn_sock, STATUS_ACCOUNT_NOT_FOUND, "Tài khoản không tồn tại.");
                } else if (status == STATUS_LOCKED) {
                    send_response(conn_sock, STATUS_ACCOUNT_LOCKED, "Tài khoản đang bị khóa (Status = 0).");
                } else if (!pass_match) {
                    if (strcmp(last_attempt_user, user) == 0) {
                        failed_attempts++;
                    } else {
                        failed_attempts = 1;
                        strncpy(last_attempt_user, user, sizeof(last_attempt_user) - 1);
                    }

                    if (failed_attempts >= MAX_LOGIN_ATTEMPTS) {
                        lock_account(user);
                        char msg[MAX_MESSAGE_LEN];
                        snprintf(msg, sizeof(msg),
                                 "Mật khẩu không đúng! Tài khoản '%s' đã bị khóa do nhập sai 3 lần liên tiếp.",
                                 user);
                        send_response(conn_sock, STATUS_ACCOUNT_LOCKED, msg);
                        failed_attempts = 0;
                    } else {
                        int remaining = MAX_LOGIN_ATTEMPTS - failed_attempts;
                        char msg[MAX_MESSAGE_LEN];
                        snprintf(msg, sizeof(msg),
                                 "Mật khẩu không đúng! Bạn còn %d lần thử.", remaining);
                        send_response(conn_sock, STATUS_PASSWORD_INCORRECT, msg);
                    }
                } else {
                    // Đăng nhập thành công
                    failed_attempts = 0;
                    is_logged_in = true;
                    strncpy(current_user, user, sizeof(current_user) - 1);

                    char msg[MAX_MESSAGE_LEN];
                    snprintf(msg, sizeof(msg), "Xin chào %s! Đăng nhập thành công.", user);
                    send_response(conn_sock, STATUS_SUCCESS, msg);
                    printf("[PID %d] Người dùng '%s' đăng nhập thành công.\n", getpid(), user);
                }
                break;
            }

            case CMD_LOGOUT: {
                if (!is_logged_in) {
                    send_response(conn_sock, STATUS_NOT_LOGGED_IN, "Bạn chưa đăng nhập.");
                    break;
                }

                char msg[MAX_MESSAGE_LEN];
                snprintf(msg, sizeof(msg), "Tạm biệt %s! Đăng xuất thành công.", current_user);
                printf("[PID %d] Người dùng '%s' đăng xuất.\n", getpid(), current_user);

                is_logged_in = false;
                current_user[0] = '\0';
                send_response(conn_sock, STATUS_SUCCESS, msg);
                break;
            }

            case CMD_EXIT: {
                if (is_logged_in) {
                    printf("[PID %d] Người dùng '%s' thoát ứng dụng.\n", getpid(), current_user);
                }
                return;
            }

            default: {
                send_response(conn_sock, STATUS_BAD_REQUEST, "Mã lệnh không xác định.");
                break;
            }
        }
    }
}

/**
 * @function main: Điểm vào của server xác thực đa tiến trình (forking server).
 *                 Khởi tạo socket lắng nghe, đăng ký handler thu dọn tiến trình con và fork khi có kết nối.
 *
 * @param argc: Số lượng tham số dòng lệnh.
 * @param argv: Mảng các chuỗi tham số dòng lệnh (Port_Number).
 *
 * @return: 0 nếu kết thúc bình thường, 1 nếu gặp lỗi khởi tạo.
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

    // Đăng ký bộ xử lý tín hiệu SIGCHLD để dọn tiến trình con
    signal(SIGCHLD, sig_chld);

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
    printf("  Server xác thực đa tiến trình (Forking Server)\n");
    printf("  Sinh viên: Nguyễn Văn Huấn - MSSV: 20235733\n");
    printf("  Cổng: %d | CSDL: %s | PID Server Cha: %d\n", port, DEFAULT_ACCOUNT_FILE, getpid());
    printf("========================================================\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t sin_size = sizeof(client_addr);
        int conn_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &sin_size);

        if (conn_sock < 0) {
            if (errno == EINTR) continue;
            perror("Lỗi chấp nhận kết nối (accept)");
            break;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("[+] Kết nối mới từ [%s:%d]\n", client_ip, ntohs(client_addr.sin_port));

        pid_t pid = fork();
        if (pid < 0) {
            perror("Lỗi fork() tiến trình con");
            close(conn_sock);
            continue;
        }

        if (pid == 0) {
            // TIẾN TRÌNH CON: Đóng listen_sock, phục vụ client rồi thoát
            close(listen_sock);
            handle_client(conn_sock);
            close(conn_sock);
            exit(0);
        }

        // TIẾN TRÌNH CHA: Đóng conn_sock đã bàn giao cho tiến trình con
        close(conn_sock);
    }

    close(listen_sock);
    return 0;
}
