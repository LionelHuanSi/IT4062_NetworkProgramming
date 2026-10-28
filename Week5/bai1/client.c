#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>

#define MAX_USERNAME_LEN 64
#define MAX_PASSWORD_LEN 64
#define MAX_MESSAGE_LEN 512
#define BUFFER_SIZE 2048

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

// Message Headers
typedef struct {
    uint32_t command;
    uint32_t payload_len;
} __attribute__((packed)) RequestHeader;

typedef struct {
    uint32_t status_code;
    uint32_t payload_len;
} __attribute__((packed)) ResponseHeader;

typedef enum {
    CLIENT_STATE_UNAUTHENTICATED,
    CLIENT_STATE_AUTHENTICATED
} ClientState;

static int g_sockfd = -1;

/* ---------------- Network Stream Helpers ---------------- */

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

/* ---------------- Client Logic ---------------- */

/**
 * @function handle_exit: Xử lý tín hiệu ngắt (SIGINT/SIGTERM), gửi thông điệp CMD_EXIT
 *                        tới server để giải phóng phiên và đóng socket client an toàn.
 *
 * @param sig: Mã tín hiệu hệ thống nhận được.
 *
 * @return: Không có (hàm kết thúc tiến trình client).
 */
void handle_exit(int sig) {
    (void)sig;
    if (g_sockfd >= 0) {
        RequestHeader req;
        req.command = htonl(CMD_EXIT);
        req.payload_len = 0;
        send_all(g_sockfd, &req, sizeof(req));
        close(g_sockfd);
    }
    printf("\n[Client] Exiting...\n");
    exit(0);
}

/**
 * @function trim_crlf: Cắt bỏ các ký tự xuống dòng ('\r', '\n') ở cuối chuỗi văn bản.
 *
 * @param str: Con trỏ tới chuỗi ký tự cần cắt bỏ ký tự xuống dòng.
 *
 * @return: Không có (thao tác trực tiếp trên chuỗi str).
 */
void trim_crlf(char *str) {
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r')) {
        str[--len] = '\0';
    }
}

/**
 * @function send_request: Đóng gói và gửi một bản tin yêu cầu (RequestHeader + payload)
 *                         tới server theo giao thức nhị phân của hệ thống.
 *
 * @param fd: File descriptor của socket kết nối với server.
 * @param cmd: Mã lệnh yêu cầu (CommandType).
 * @param payload: Chuỗi dữ liệu payload gửi kèm (hoặc NULL nếu không có).
 *
 * @return: true nếu gửi toàn bộ thành công, false nếu gặp lỗi socket.
 */
bool send_request(int fd, CommandType cmd, const char *payload) {
    uint32_t payload_len = (payload != NULL) ? (uint32_t)strlen(payload) : 0;
    RequestHeader req;
    req.command = htonl(cmd);
    req.payload_len = htonl(payload_len);

    if (send_all(fd, &req, sizeof(req)) < 0) return false;
    if (payload_len > 0) {
        if (send_all(fd, payload, payload_len) < 0) return false;
    }
    return true;
}

/**
 * @function receive_response: Đọc bản tin phản hồi (ResponseHeader + payload) từ server
 *                             và trích xuất mã trạng thái cùng chuỗi thông báo.
 *
 * @param fd: File descriptor của socket kết nối với server.
 * @param out_status: Con trỏ nhận mã trạng thái phản hồi (ResponseStatus).
 * @param out_msg: Con trỏ bộ đệm nhận nội dung chuỗi thông báo phản hồi.
 * @param msg_capacity: Kích thước tối đa của bộ đệm out_msg.
 *
 * @return: true nếu nhận và giải mã thành công, false nếu ngắt kết nối hoặc có lỗi.
 */
bool receive_response(int fd, ResponseStatus *out_status, char *out_msg, size_t msg_capacity) {
    ResponseHeader resp;
    ssize_t ret = recv_all(fd, &resp, sizeof(resp));
    if (ret <= 0) return false;

    *out_status = (ResponseStatus)ntohl(resp.status_code);
    uint32_t payload_len = ntohl(resp.payload_len);

    if (payload_len > 0) {
        if (payload_len >= msg_capacity) {
            payload_len = (uint32_t)(msg_capacity - 1);
        }
        ret = recv_all(fd, out_msg, payload_len);
        if (ret != (ssize_t)payload_len) return false;
        out_msg[payload_len] = '\0';
    } else {
        out_msg[0] = '\0';
    }
    return true;
}

/**
 * @function main: Điểm vào của chương trình client. Kiểm tra tham số dòng lệnh,
 *                 thiết lập kết nối TCP tới server và điều khiển giao diện menu tương tác.
 *
 * @param argc: Số lượng tham số dòng lệnh.
 * @param argv: Mảng các chuỗi tham số dòng lệnh (IP_Addr, Port_Number).
 *
 * @return: 0 nếu thoát bình thường, 1 nếu tham số không hợp lệ hoặc lỗi kết nối.
 */
int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <IP_Addr> <Port_Number>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        printf("Invalid port number: %s\n", argv[2]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_exit);
    signal(SIGTERM, handle_exit);

    g_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sockfd < 0) {
        perror("Socket creation failed");
        return 1;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, argv[1], &servaddr.sin_addr) <= 0) {
        printf("Invalid server IP address: %s\n", argv[1]);
        close(g_sockfd);
        return 1;
    }

    if (connect(g_sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Connection to server failed");
        close(g_sockfd);
        return 1;
    }

    printf("Connected to authentication server at %s:%d\n", argv[1], port);

    ClientState state = CLIENT_STATE_UNAUTHENTICATED;
    char current_user[MAX_USERNAME_LEN] = "";
    char line_buffer[BUFFER_SIZE];

    while (1) {
        printf("------------------------------------\n");
        printf("     USER MANAGEMENT SYSTEM\n");
        printf("------------------------------------\n");
        if (state == CLIENT_STATE_AUTHENTICATED) {
            printf(" Status: Logged in as [%s]\n", current_user);
        } else {
            printf(" Status: Not logged in\n");
        }
        printf("------------------------------------\n");
        printf("1. Log in\n");
        printf("2. Log out\n");
        printf("3. Exit\n");
        printf("Your choice: ");
        fflush(stdout);

        if (fgets(line_buffer, sizeof(line_buffer), stdin) == NULL) break;
        trim_crlf(line_buffer);
        if (strlen(line_buffer) == 0) continue;

        int choice = atoi(line_buffer);

        switch (choice) {
            case 1: { // Log in
                if (state == CLIENT_STATE_AUTHENTICATED) {
                    printf("\n[Notice] You have already logged in as '%s'. Please log out first.\n\n", current_user);
                    break;
                }

                char username[MAX_USERNAME_LEN] = "";
                char password[MAX_PASSWORD_LEN] = "";

                printf("Username: ");
                fflush(stdout);
                if (fgets(username, sizeof(username), stdin) == NULL) break;
                trim_crlf(username);

                printf("Password: ");
                fflush(stdout);
                if (fgets(password, sizeof(password), stdin) == NULL) break;
                trim_crlf(password);

                if (strlen(username) == 0 || strlen(password) == 0) {
                    printf("\n[Error] Username and password cannot be empty!\n\n");
                    break;
                }

                char payload[BUFFER_SIZE];
                snprintf(payload, sizeof(payload), "%s\n%s", username, password);

                if (!send_request(g_sockfd, CMD_LOGIN, payload)) {
                    printf("\n[Error] Failed to send request to server.\n\n");
                    handle_exit(0);
                }

                ResponseStatus status;
                char response_msg[MAX_MESSAGE_LEN];
                if (!receive_response(g_sockfd, &status, response_msg, sizeof(response_msg))) {
                    printf("\n[Error] Server disconnected or response failed.\n\n");
                    handle_exit(0);
                }

                printf("\n%s\n\n", response_msg);

                if (status == STATUS_SUCCESS) {
                    state = CLIENT_STATE_AUTHENTICATED;
                    strncpy(current_user, username, sizeof(current_user) - 1);
                    current_user[sizeof(current_user) - 1] = '\0';
                }
                break;
            }

            case 2: { // Log out
                if (state == CLIENT_STATE_UNAUTHENTICATED) {
                    printf("\n[Notice] You have not logged in.\n\n");
                    break;
                }

                if (!send_request(g_sockfd, CMD_LOGOUT, NULL)) {
                    printf("\n[Error] Failed to send request to server.\n\n");
                    handle_exit(0);
                }

                ResponseStatus status;
                char response_msg[MAX_MESSAGE_LEN];
                if (!receive_response(g_sockfd, &status, response_msg, sizeof(response_msg))) {
                    printf("\n[Error] Server disconnected or response failed.\n\n");
                    handle_exit(0);
                }

                printf("\n%s\n\n", response_msg);

                if (status == STATUS_SUCCESS) {
                    state = CLIENT_STATE_UNAUTHENTICATED;
                    current_user[0] = '\0';
                }
                break;
            }

            case 3: { // Exit
                handle_exit(0);
                break;
            }

            default: {
                printf("\n[Error] Invalid choice! Please select 1, 2, or 3.\n\n");
                break;
            }
        }
    }

    handle_exit(0);
    return 0;
}
