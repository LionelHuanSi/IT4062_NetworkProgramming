#ifndef RFTP_PROTOCOL_H
#define RFTP_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define RFTP_MAGIC 0x5246 // "RF"
#define RFTP_PAYLOAD_SIZE 1024
#define RFTP_TIMEOUT_SEC 1
#define RFTP_MAX_RETRIES 5
#define RFTP_DEFAULT_PORT 6600

// Opcodes
typedef enum {
    OP_WRQ   = 0x01, // Write / Upload Request
    OP_RRQ   = 0x02, // Read / Download Request
    OP_DATA  = 0x03, // Data block
    OP_ACK   = 0x04, // Acknowledgment
    OP_ERROR = 0x05, // Error notice
    OP_FIN   = 0x06  // Final block confirmation
} RFTPOpcode;

// Error Codes
typedef enum {
    ERR_FILE_NOT_FOUND     = 100,
    ERR_ACCESS_DENIED      = 101,
    ERR_FILE_EXISTS        = 102,
    ERR_CHECKSUM_MISMATCH  = 103,
    ERR_TIMEOUT            = 104
} RFTPErrorCode;

// Packed 12-byte Header
typedef struct {
    uint16_t magic;         // RFTP_MAGIC (0x5246)
    uint8_t  opcode;        // RFTPOpcode
    uint8_t  reserved;      // Reserved (0x00)
    uint32_t block_number;  // 32-bit block / sequence number
    uint16_t payload_len;   // 0 - 1024
    uint16_t checksum;      // 16-bit internet checksum
} __attribute__((packed)) RFTPHeader;

typedef struct {
    RFTPHeader header;
    uint8_t    data[RFTP_PAYLOAD_SIZE];
} __attribute__((packed)) RFTPPacket;

/**
 * @function compute_checksum: Tính toán mã kiểm tra bù 1 16-bit Internet Checksum (theo RFC 1071).
 *
 * @param data: Con trỏ tới vùng dữ liệu cần tính checksum.
 * @param len: Tổng số bytes của vùng dữ liệu.
 *
 * @return: Giá trị checksum 16-bit đã tính toán.
 */
static inline uint16_t compute_checksum(const void *data, size_t len) {
    const uint16_t *buf = (const uint16_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *buf++;
        len -= 2;
    }

    if (len == 1) {
        sum += *(const uint8_t *)buf;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}

#endif // RFTP_PROTOCOL_H
