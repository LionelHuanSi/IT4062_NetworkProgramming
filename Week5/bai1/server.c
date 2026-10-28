#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>

#define DEFAULT_ACCOUNT_FILE "account.txt"
#define MAX_USERNAME_LEN 64
#define MAX_PASSWORD_LEN 64
#define MAX_MESSAGE_LEN 512
#define MAX_LOGIN_ATTEMPTS 3
#define BUFFER_SIZE 2048

#define STATUS_LOCKED 0
#define STATUS_ACTIVE 1

// Protocol Command Codes
typedef enum {
    CMD_LOGIN = 1,
    CMD_LOGOUT = 2,
    CMD_EXIT = 3
} CommandType;

// Protocol Response Status Codes
typedef enum {
    STATUS_SUCCESS = 200,
    STATUS_BAD_REQUEST = 400,
    STATUS_PASSWORD_INCORRECT = 401,
    STATUS_ACCOUNT_LOCKED = 403,
    STATUS_ACCOUNT_NOT_FOUND = 404,
    STATUS_ALREADY_LOGGED_IN = 409,
    STATUS_NOT_LOGGED_IN = 412,
    STATUS_SERVER_ERROR = 500
} ResponseStatus;

// Message Headers (Network Byte Order)
typedef struct {
    uint32_t command;
    uint32_t payload_len;
} __attribute__((packed)) RequestHeader;

typedef struct {
    uint32_t status_code;
    uint32_t payload_len;
} __attribute__((packed)) ResponseHeader;

// Account linked list node
typedef struct Account {
    char username[MAX_USERNAME_LEN];
    char password[MAX_PASSWORD_LEN];
    int status;           // 1 = Active, 0 = Locked
    int failed_attempts;  // Consecutive failed login attempts
    struct Account *next;
} Account;

// Client session state
typedef struct {
    int socket_fd;
    bool is_logged_in;
    char logged_in_user[MAX_USERNAME_LEN];
} ClientSession;

static Account *g_accounts = NULL;
static char g_account_file[256] = DEFAULT_ACCOUNT_FILE;
static ClientSession g_sessions[FD_SETSIZE];
static int g_listen_fd = -1;

/**
 * @function recv_all: Nhận chính xác len bytes từ stream socket TCP,
 *                     xử lý các trường hợp nhận thiếu do phân mảnh gói tin.
 *
 * @param fd: File descriptor của socket đã kết nối.
 * @param buf: Con trỏ tới bộ đệm lưu trữ dữ liệu nhận được.
 * @param len: Tổng số bytes cần nhận.
 *
 * @return: Số bytes thực nhận (len), 0 nếu kết nối đóng tuần tự (EOF), hoặc -1 nếu có lỗi.
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
 *                     xử lý các trường hợp ghi thiếu và giới hạn bộ đệm socket.
 *
 * @param fd: File descriptor của socket đã kết nối.
 * @param buf: Con trỏ tới bộ đệm chứa dữ liệu nguồn cần gửi.
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
 * @function free_accounts: Giải phóng toàn bộ các node được cấp phát động
 *                          trong danh sách liên kết tài khoản người dùng.
 *
 * @return: Không có.
 */
void free_accounts(void) {
    Account *curr = g_accounts;
    while (curr != NULL) {
        Account *tmp = curr;
        curr = curr->next;
        free(tmp);
    }
    g_accounts = NULL;
}

/**
 * @function load_accounts: Tải danh sách tài khoản từ tệp lưu trữ vào bộ nhớ
 *                          dưới dạng danh sách liên kết đơn.
 *
 * @param filepath: Đường dẫn tới tệp dữ liệu tài khoản (ví dụ: account.txt).
 *
 * @return: true nếu mở và đọc file thành công, false nếu có lỗi.
 */
bool load_accounts(const char *filepath) {
    if (filepath != NULL && strlen(filepath) > 0) {
        strncpy(g_account_file, filepath, sizeof(g_account_file) - 1);
        g_account_file[sizeof(g_account_file) - 1] = '\0';
    }

    FILE *fp = fopen(g_account_file, "r");
    if (fp == NULL) {
        perror("Failed to open accounts file");
        return false;
    }

    free_accounts();

    char user[MAX_USERNAME_LEN];
    char pass[MAX_PASSWORD_LEN];
    int status = 0;

    while (fscanf(fp, "%63s %63s %d", user, pass, &status) == 3) {
        Account *node = (Account *)malloc(sizeof(Account));
        if (node == NULL) {
            perror("Memory allocation error");
            fclose(fp);
            return false;
        }
        strncpy(node->username, user, sizeof(node->username) - 1);
        node->username[sizeof(node->username) - 1] = '\0';

        strncpy(node->password, pass, sizeof(node->password) - 1);
        node->password[sizeof(node->password) - 1] = '\0';

        node->status = status;
        node->failed_attempts = 0;

        node->next = g_accounts;
        g_accounts = node;
    }

    fclose(fp);
    return true;
}

/**
 * @function find_account: Tìm kiếm tài khoản trong danh sách liên kết theo tên đăng nhập.
 *
 * @param username: Tên đăng nhập cần tìm kiếm.
 *
 * @return: Con trỏ tới node Account nếu tìm thấy, NULL nếu không tồn tại.
 */
Account* find_account(const char *username) {
    if (username == NULL || *username == '\0') return NULL;
    Account *curr = g_accounts;
    while (curr != NULL) {
        if (strcmp(curr->username, username) == 0) return curr;
        curr = curr->next;
    }
    return NULL;
}

/**
 * @function save_accounts: Ghi nguyên tử toàn bộ danh sách tài khoản từ bộ nhớ
 *                          xuống tệp đĩa thông qua tệp tạm thời và đổi tên (rename).
 *
 * @return: true nếu ghi và đổi tên thành công, false nếu gặp lỗi I/O.
 */
bool save_accounts(void) {
    char tmpPath[300];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", g_account_file);

    FILE *fp = fopen(tmpPath, "w");
    if (fp == NULL) {
        perror("Failed to create temporary file in save_accounts");
        return false;
    }

    Account *curr = g_accounts;
    while (curr != NULL) {
        fprintf(fp, "%s %s %d\n", curr->username, curr->password, curr->status);
        curr = curr->next;
    }

    fclose(fp);

    if (rename(tmpPath, g_account_file) != 0) {
        perror("Failed to rename in save_accounts");
        return false;
    }
    return true;
}

/**
 * @function update_account_status: Cập nhật cờ trạng thái của một tài khoản (kích hoạt / khóa)
 *                                  và lưu ngay lập tức xuống tệp dữ liệu trên đĩa.
 *
 * @param username: Tên người dùng cần cập nhật trạng thái.
 * @param new_status: Trạng thái mới (STATUS_ACTIVE hoặc STATUS_LOCKED).
 *
 * @return: true nếu cập nhật và lưu thành công, false nếu không tìm thấy người dùng.
 */
bool update_account_status(const char *username, int new_status) {
    Account *acc = find_account(username);
    if (acc == NULL) return false;
    acc->status = new_status;
    return save_accounts();
}

/**
 * @function handle_signal: Hàm xử lý tín hiệu ngắt (SIGINT/SIGTERM) để giải phóng
 *                          tài nguyên bộ nhớ, đóng các socket và thoát an toàn.
 *
 * @param sig: Mã tín hiệu hệ thống nhận được.
 *
 * @return: Không có (kết thúc tiến trình server).
 */
void handle_signal(int sig) {
    (void)sig;
    printf("\n[Server] Shutting down...\n");
    if (g_listen_fd >= 0) close(g_listen_fd);
    for (int i = 0; i < FD_SETSIZE; i++) {
        if (g_sessions[i].socket_fd >= 0) close(g_sessions[i].socket_fd);
    }
    free_accounts();
    exit(0);
}

/**
 * @function init_sessions: Khởi tạo bảng quản lý phiên làm việc của các client kết nối.
 *
 * @return: Không có.
 */
void init_sessions(void) {
    for (int i = 0; i < FD_SETSIZE; i++) {
        g_sessions[i].socket_fd = -1;
        g_sessions[i].is_logged_in = false;
        g_sessions[i].logged_in_user[0] = '\0';
    }
}

/**
 * @function close_client: Đóng kết nối của một client, xóa file descriptor khỏi
 *                         tập master set của select và đặt lại trạng thái phiên.
 *
 * @param fd: File descriptor của socket client cần đóng.
 * @param master_set: Con trỏ tới tập fd_set chính mà select theo dõi.
 *
 * @return: Không có.
 */
void close_client(int fd, fd_set *master_set) {
    printf("[-] Client on socket %d disconnected.\n", fd);
    close(fd);
    FD_CLR(fd, master_set);
    g_sessions[fd].socket_fd = -1;
    g_sessions[fd].is_logged_in = false;
    g_sessions[fd].logged_in_user[0] = '\0';
}

/**
 * @function dispatch_command: Phân tích cú pháp yêu cầu từ client, thực thi nghiệp vụ
 *                             xác thực và gửi gói tin phản hồi có định dạng về cho client.
 *
 * @param session: Con trỏ tới trạng thái phiên làm việc hiện tại của client.
 * @param cmd: Mã lệnh yêu cầu nhận được từ client (CommandType).
 * @param payload: Chuỗi dữ liệu đính kèm lệnh từ client.
 * @param fd: File descriptor của client đang giao tiếp.
 * @param master_set: Con trỏ tới tập fd_set chính để quản lý kết nối nếu cần đóng.
 *
 * @return: Không có.
 */
void dispatch_command(ClientSession *session, uint32_t cmd, const char *payload, int fd, fd_set *master_set) {
    ResponseStatus status = STATUS_SUCCESS;
    char out_msg[MAX_MESSAGE_LEN] = "";

    switch (cmd) {
        case CMD_LOGIN: {
            if (session->is_logged_in) {
                status = STATUS_ALREADY_LOGGED_IN;
                snprintf(out_msg, sizeof(out_msg),
                         "This client window is already logged in as '%s'. Please log out first.",
                         session->logged_in_user);
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
                status = STATUS_BAD_REQUEST;
                snprintf(out_msg, sizeof(out_msg), "Invalid login format.");
                break;
            }

            Account *acc = find_account(user);
            if (acc == NULL) {
                status = STATUS_ACCOUNT_NOT_FOUND;
                snprintf(out_msg, sizeof(out_msg), "Account does not exist.");
            } else if (acc->status == STATUS_LOCKED) {
                status = STATUS_ACCOUNT_LOCKED;
                snprintf(out_msg, sizeof(out_msg), "Account is blocked / locked.");
            } else if (strcmp(acc->password, pass) != 0) {
                acc->failed_attempts++;
                if (acc->failed_attempts >= MAX_LOGIN_ATTEMPTS) {
                    update_account_status(user, STATUS_LOCKED);
                    status = STATUS_ACCOUNT_LOCKED;
                    snprintf(out_msg, sizeof(out_msg),
                             "Password incorrect! Account has been locked due to %d consecutive failed attempts.",
                             MAX_LOGIN_ATTEMPTS);
                } else {
                    status = STATUS_PASSWORD_INCORRECT;
                    int remaining = MAX_LOGIN_ATTEMPTS - acc->failed_attempts;
                    snprintf(out_msg, sizeof(out_msg),
                             "Password incorrect! You have %d attempt(s) remaining.", remaining);
                }
            } else {
                // Login successful
                acc->failed_attempts = 0;
                session->is_logged_in = true;
                strncpy(session->logged_in_user, user, sizeof(session->logged_in_user) - 1);
                session->logged_in_user[sizeof(session->logged_in_user) - 1] = '\0';
                status = STATUS_SUCCESS;
                snprintf(out_msg, sizeof(out_msg), "Hello %s! Login successful.", user);
                printf("[+] User '%s' logged in on socket %d\n", user, fd);
            }
            break;
        }

        case CMD_LOGOUT: {
            if (!session->is_logged_in) {
                status = STATUS_NOT_LOGGED_IN;
                snprintf(out_msg, sizeof(out_msg), "You are not logged in on this client window.");
                break;
            }

            printf("[+] User '%s' logged out from socket %d\n", session->logged_in_user, fd);
            snprintf(out_msg, sizeof(out_msg), "Goodbye %s! Logged out successfully.", session->logged_in_user);
            session->is_logged_in = false;
            session->logged_in_user[0] = '\0';
            status = STATUS_SUCCESS;
            break;
        }

        case CMD_EXIT: {
            if (session->is_logged_in) {
                printf("[+] User '%s' exited from socket %d\n", session->logged_in_user, fd);
            }
            close_client(fd, master_set);
            return;
        }

        default: {
            status = STATUS_BAD_REQUEST;
            snprintf(out_msg, sizeof(out_msg), "Unknown command opcode.");
            break;
        }
    }

    uint32_t msg_len = (uint32_t)strlen(out_msg);
    ResponseHeader resp;
    resp.status_code = htonl(status);
    resp.payload_len = htonl(msg_len);

    if (send_all(fd, &resp, sizeof(resp)) < 0) {
        close_client(fd, master_set);
        return;
    }

    if (msg_len > 0) {
        if (send_all(fd, out_msg, msg_len) < 0) {
            close_client(fd, master_set);
            return;
        }
    }
}

/**
 * @function main: Điểm vào của server. Xác thực tham số dòng lệnh, tải CSDL tài khoản,
 *                 khởi tạo socket lắng nghe và chạy vòng lặp sự kiện bất đồng bộ với select().
 *
 * @param argc: Số lượng tham số dòng lệnh.
 * @param argv: Mảng các chuỗi tham số dòng lệnh (Port_Number).
 *
 * @return: 0 khi thoát bình thường, 1 nếu gặp lỗi khởi tạo hoặc lỗi socket.
 */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <Port_Number>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        printf("Invalid port number: %s\n", argv[1]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (!load_accounts(DEFAULT_ACCOUNT_FILE)) {
        fprintf(stderr, "Error: Could not load accounts from %s\n", DEFAULT_ACCOUNT_FILE);
        return 1;
    }

    init_sessions();

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        perror("Socket creation failed");
        return 1;
    }

    int opt = 1;
    if (setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
        close(g_listen_fd);
        return 1;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons((uint16_t)port);

    if (bind(g_listen_fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Bind failed");
        close(g_listen_fd);
        return 1;
    }

    if (listen(g_listen_fd, 10) < 0) {
        perror("Listen failed");
        close(g_listen_fd);
        return 1;
    }

    printf("==================================================\n");
    printf("  TCP Authentication Server (Concurrent select)\n");
    printf("  Port: %d | Database: %s\n", port, DEFAULT_ACCOUNT_FILE);
    printf("==================================================\n");

    fd_set master_set, read_set;
    FD_ZERO(&master_set);
    FD_SET(g_listen_fd, &master_set);
    int max_fd = g_listen_fd;

    while (1) {
        read_set = master_set;
        int activity = select(max_fd + 1, &read_set, NULL, NULL, NULL);
        if (activity < 0) {
            perror("select error");
            break;
        }

        // New client connection
        if (FD_ISSET(g_listen_fd, &read_set)) {
            struct sockaddr_in cliaddr;
            socklen_t clilen = sizeof(cliaddr);
            int conn_fd = accept(g_listen_fd, (struct sockaddr *)&cliaddr, &clilen);
            if (conn_fd >= 0) {
                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &cliaddr.sin_addr, client_ip, sizeof(client_ip));
                int client_port = ntohs(cliaddr.sin_port);

                if (conn_fd < FD_SETSIZE) {
                    FD_SET(conn_fd, &master_set);
                    if (conn_fd > max_fd) max_fd = conn_fd;

                    g_sessions[conn_fd].socket_fd = conn_fd;
                    g_sessions[conn_fd].is_logged_in = false;
                    g_sessions[conn_fd].logged_in_user[0] = '\0';

                    printf("[+] Client connected from [%s:%d] on socket %d\n",
                           client_ip, client_port, conn_fd);
                } else {
                    fprintf(stderr, "Max client limit reached. Closing %d\n", conn_fd);
                    close(conn_fd);
                }
            }
        }

        // Check data on client sockets
        for (int fd = 0; fd <= max_fd; fd++) {
            if (fd != g_listen_fd && FD_ISSET(fd, &read_set)) {
                RequestHeader req;
                ssize_t ret = recv_all(fd, &req, sizeof(req));
                if (ret <= 0) {
                    close_client(fd, &master_set);
                    continue;
                }

                uint32_t cmd = ntohl(req.command);
                uint32_t payload_len = ntohl(req.payload_len);

                char payload[BUFFER_SIZE] = "";
                if (payload_len > 0) {
                    if (payload_len >= sizeof(payload)) {
                        fprintf(stderr, "Payload exceeds buffer. Closing %d\n", fd);
                        close_client(fd, &master_set);
                        continue;
                    }
                    ret = recv_all(fd, payload, payload_len);
                    if (ret != (ssize_t)payload_len) {
                        close_client(fd, &master_set);
                        continue;
                    }
                    payload[payload_len] = '\0';
                }

                dispatch_command(&g_sessions[fd], cmd, payload, fd, &master_set);
            }
        }
    }

    close(g_listen_fd);
    free_accounts();
    return 0;
}
