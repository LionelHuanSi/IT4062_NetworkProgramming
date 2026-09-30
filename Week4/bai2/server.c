#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define STORAGE_DIR "storage"
#define CHUNK_SIZE 65536
#define MAX_FILENAME_LEN 512

// Protocol Status Codes
#define STATUS_READY 0
#define STATUS_FILE_EXISTS 1
#define STATUS_ERROR 2
#define STATUS_SUCCESS 0
#define STATUS_INTERRUPTED 1

/**
 * @function recv_all: Receives exactly len bytes from a stream socket,
 *                     handling short reads and TCP byte-stream fragmentation.
 *
 * @param fd: The connected socket file descriptor.
 * @param buf: Pointer to destination buffer.
 * @param len: Total number of bytes to receive.
 *
 * @return: Number of bytes received on success (len), 0 on orderly shutdown
 * (EOF), or -1 on error.
 */
ssize_t recv_all(int fd, void *buf, size_t len) {
  size_t nLeft = len;
  char *ptr = (char *)buf;

  while (nLeft > 0) {
    ssize_t nRead = recv(fd, ptr, nLeft, 0);
    if (nRead < 0) {
      return -1;
    } else if (nRead == 0) {
      return (ssize_t)(len - nLeft);
    }
    nLeft -= nRead;
    ptr += nRead;
  }

  return (ssize_t)len;
}

/**
 * @function send_all: Transmits exactly len bytes over a stream socket,
 *                     handling short writes and socket buffer limitations.
 *
 * @param fd: The connected socket file descriptor.
 * @param buf: Pointer to the source data buffer.
 * @param len: Total number of bytes to transmit.
 *
 * @return: Number of bytes sent on success (len), or -1 on error.
 */
ssize_t send_all(int fd, const void *buf, size_t len) {
  size_t nLeft = len;
  const char *ptr = (const char *)buf;

  while (nLeft > 0) {
    ssize_t nWritten = send(fd, ptr, nLeft, 0);
    if (nWritten <= 0) {
      return -1;
    }
    nLeft -= nWritten;
    ptr += nWritten;
  }

  return (ssize_t)len;
}

/**
 * @function unpack_upload_header: Deserializes the 16-byte upload request
 * header from network byte order.
 *
 * @param buf: Pointer to the 16-byte raw buffer.
 * @param op: Pointer to store operation code.
 * @param name_len: Pointer to store filename string length.
 * @param file_size: Pointer to store 64-bit file size.
 *
 * @return: None.
 */
void unpack_upload_header(const uint8_t *buf, uint32_t *op, uint32_t *name_len,
                          uint64_t *file_size) {
  uint32_t net_op, net_name_len;
  memcpy(&net_op, buf, 4);
  memcpy(&net_name_len, buf + 4, 4);
  *op = ntohl(net_op);
  *name_len = ntohl(net_name_len);

  uint64_t sz = 0;
  for (int i = 0; i < 8; i++) {
    sz = (sz << 8) | buf[8 + i];
  }
  *file_size = sz;
}

/**
 * @function sanitizeFilename: Strips any leading directory paths and ensures
 *                             the filename is safe from directory traversal.
 *
 * @param filename: A pointer to the raw filename string.
 *
 * @return: Pointer to sanitized basename.
 */
const char *sanitizeFilename(const char *filename) {
  const char *p = filename;
  const char *last = filename;

  while (*p != '\0') {
    if (*p == '/' || *p == '\\') {
      last = p + 1;
    }
    p++;
  }

  // Disallow empty or parent directory navigation
  if (*last == '\0' || strcmp(last, ".") == 0 || strcmp(last, "..") == 0) {
    return "unnamed_file";
  }

  return last;
}

/**
 * @function main: Server entry point for TCP File Receiver.
 *                 Listens on a given TCP port, manages the storage repository,
 *                 detects duplicate files, and reliably streams files up to
 * 100MB+.
 *
 * @param argc: Number of command-line arguments.
 * @param argv: Array of command-line argument strings.
 *
 * @return: 0 on success, 1 on argument/socket error.
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

  signal(SIGPIPE, SIG_IGN);

  // Create storage directory if it does not already exist
  if (mkdir(STORAGE_DIR, 0755) < 0 && errno != EEXIST) {
    perror("Failed to create storage directory");
    return 1;
  }

  int listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenfd < 0) {
    perror("socket creation failed");
    return 1;
  }

  int opt = 1;
  if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("setsockopt failed");
    close(listenfd);
    return 1;
  }

  struct sockaddr_in servaddr;
  memset(&servaddr, 0, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  servaddr.sin_port = htons((uint16_t)port);

  if (bind(listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
    perror("bind failed");
    close(listenfd);
    return 1;
  }

  if (listen(listenfd, 10) < 0) {
    perror("listen failed");
    close(listenfd);
    return 1;
  }

  printf("TCP File Transfer Server is running on port %d...\n", port);
  printf("Storage directory: %s/\n", STORAGE_DIR);

  while (1) {
    struct sockaddr_in cliaddr;
    socklen_t clilen = sizeof(cliaddr);
    int connfd = accept(listenfd, (struct sockaddr *)&cliaddr, &clilen);
    if (connfd < 0) {
      perror("accept error");
      continue;
    }

    char clientIP[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cliaddr.sin_addr, clientIP, sizeof(clientIP));
    int clientPort = ntohs(cliaddr.sin_port);

    printf("[+] Client [%s:%d] connected.\n", clientIP, clientPort);

    while (1) {
      uint8_t headerBuf[16];
      // Read 16-byte upload header
      ssize_t ret = recv_all(connfd, headerBuf, sizeof(headerBuf));
      if (ret <= 0) {
        // Client closed connection
        break;
      }

      uint32_t op = 0;
      uint32_t name_len = 0;
      uint64_t file_size = 0;
      unpack_upload_header(headerBuf, &op, &name_len, &file_size);

      if (name_len == 0 || name_len >= MAX_FILENAME_LEN) {
        printf("[-] Invalid filename length: %u\n", name_len);
        break;
      }

      char rawFilename[MAX_FILENAME_LEN];
      ret = recv_all(connfd, rawFilename, name_len);
      if (ret != (ssize_t)name_len) {
        printf("[-] Incomplete filename received from [%s:%d]\n", clientIP,
               clientPort);
        break;
      }
      rawFilename[name_len] = '\0';

      const char *cleanName = sanitizeFilename(rawFilename);
      printf("[+] Upload request from [%s:%d]: File \"%s\" (%llu bytes)\n",
             clientIP, clientPort, cleanName, (unsigned long long)file_size);

      char targetPath[1024];
      snprintf(targetPath, sizeof(targetPath), "%s/%s", STORAGE_DIR, cleanName);

      // Check if file already exists in storage directory
      if (access(targetPath, F_OK) == 0) {
        printf("[!] File \"%s\" already exists on server.\n", cleanName);
        int32_t netStatus = htonl(STATUS_FILE_EXISTS);
        send_all(connfd, &netStatus, sizeof(netStatus));
        continue;
      }

      // Create temporary file for atomic write and integrity preservation
      char tmpPath[1024];
      snprintf(tmpPath, sizeof(tmpPath), "%s/.tmp_%d_%s", STORAGE_DIR, connfd,
               cleanName);

      FILE *fp = fopen(tmpPath, "wb");
      if (fp == NULL) {
        perror("Cannot create temporary file");
        int32_t netStatus = htonl(STATUS_ERROR);
        send_all(connfd, &netStatus, sizeof(netStatus));
        continue;
      }

      // Server is ready to receive file data
      int32_t netStatus = htonl(STATUS_READY);
      if (send_all(connfd, &netStatus, sizeof(netStatus)) < 0) {
        fclose(fp);
        unlink(tmpPath);
        break;
      }

      // Stream file data in chunks
      uint64_t totalReceived = 0;
      bool transferFailed = false;
      char chunk[CHUNK_SIZE];

      while (totalReceived < file_size) {
        size_t toRead = sizeof(chunk);
        if (file_size - totalReceived < toRead) {
          toRead = (size_t)(file_size - totalReceived);
        }

        ssize_t nRead = recv(connfd, chunk, toRead, 0);
        if (nRead <= 0) {
          transferFailed = true;
          break;
        }

        if (fwrite(chunk, 1, (size_t)nRead, fp) != (size_t)nRead) {
          transferFailed = true;
          break;
        }

        totalReceived += (uint64_t)nRead;
      }

      fclose(fp);

      if (transferFailed || totalReceived != file_size) {
        printf("[-] Transfer interrupted from [%s:%d]. Cleaning up partial "
               "file.\n",
               clientIP, clientPort);
        unlink(tmpPath);
        int32_t netResult = htonl(STATUS_INTERRUPTED);
        send_all(connfd, &netResult, sizeof(netResult));
        break;
      }

      // Rename temporary file to official destination
      if (rename(tmpPath, targetPath) < 0) {
        perror("rename temporary file failed");
        unlink(tmpPath);
        int32_t netResult = htonl(STATUS_ERROR);
        send_all(connfd, &netResult, sizeof(netResult));
      } else {
        printf("[+] Successfully received \"%s\" (%llu bytes) from [%s:%d]\n",
               cleanName, (unsigned long long)file_size, clientIP, clientPort);
        int32_t netResult = htonl(STATUS_SUCCESS);
        send_all(connfd, &netResult, sizeof(netResult));
      }
    }

    printf("[-] Client [%s:%d] disconnected.\n", clientIP, clientPort);
    close(connfd);
  }

  close(listenfd);
  return 0;
}
