//
//  MicrophoneStream.c
//  voidlink-c
//
//  Originally created by qiin2333@github
//  Modified by True砖家 on 2025.9.7
//  All rights reserved.

#include "Limelight-internal.h"
#include "PlatformSockets.h"
#include "opus.h"
#include "opus_defines.h"
#include <_time.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MIC_PACKET_TYPE_OPUS 0x61 // 'a'
#define IDX_MIC_DATA 0x5504

static SOCKET micSocket = INVALID_SOCKET;
static PPLT_CRYPTO_CONTEXT micEncryptionCtx;

static uint16_t g_sequenceNumber = 0;
static uint32_t g_ssrc = 0x12345678;  // 启动时随机化

typedef struct _MICROPHONE_PACKET_HEADER {
    uint8_t flags;
    uint8_t packetType;
    uint16_t sequenceNumber;
    uint32_t timestamp;
    uint32_t ssrc;
} MICROPHONE_PACKET_HEADER, *PMICROPHONE_PACKET_HEADER;

void init_ssrc(void) {
    srand((unsigned)time(NULL));
    g_ssrc = ((uint32_t)rand() << 16) | (uint32_t)rand();
}

uint32_t GetCurrentTimestampMs(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)((tv.tv_sec * 1000ULL) + (tv.tv_usec / 1000ULL));
}

// 初始化麦克风流（保留）
int initializeMicrophoneStream(void) {
    init_ssrc();

    if (micSocket != INVALID_SOCKET) {
        // 已经初始化
        return 0;
    }

    micEncryptionCtx = PltCreateCryptoContext();
    if (micEncryptionCtx == NULL) {
        return -1;
    }

    // 创建UDP socket（使用你的 bindUdpSocket 封装）
    micSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen, 0, SOCK_QOS_TYPE_AUDIO);
    if (micSocket == INVALID_SOCKET) {
        PltDestroyCryptoContext(micEncryptionCtx);
        micEncryptionCtx = NULL;
        return LastSocketFail();
    }
    
    return 0;
}

// 关闭麦克风流（保留）
void destroyMicrophoneStream(void) {
    // 销毁 socket
    if (micSocket != INVALID_SOCKET) {
        closeSocket(micSocket);
        micSocket = INVALID_SOCKET;
    }

    if (micEncryptionCtx != NULL) {
        PltDestroyCryptoContext(micEncryptionCtx);
        micEncryptionCtx = NULL;
    }
}

int sendMicrophoneData(const char* data, int length) {
    LC_SOCKADDR saddr;
    int err;

    if (micSocket == INVALID_SOCKET) {
        return -1;
    }

    // === 构造头部 ===
    MICROPHONE_PACKET_HEADER header;
    memset(&header, 0, sizeof(header));
    header.flags = 0x00;
    header.packetType = MIC_PACKET_TYPE_OPUS;

    // 使用全局序列号（主机为 little-endian 的情况下直接写入；如果需要网络序请转换）
    header.sequenceNumber = (uint16_t)(g_sequenceNumber++ & 0xFFFF);

    // 时间戳
    header.timestamp = (uint32_t)(GetCurrentTimestampMs() & 0xFFFFFFFF);

    header.ssrc = g_ssrc;

    // === 拼接数据 ===
    char sendBuf[1500]; // MTU 内即可，根据需要调整
    int totalLen = (int)sizeof(header) + length;
    if (totalLen > (int)sizeof(sendBuf)) {
        return -2; // 数据过大
    }

    memcpy(sendBuf, &header, sizeof(header));
    memcpy(sendBuf + sizeof(header), data, length);

    // === 发包 ===
    memcpy(&saddr, &RemoteAddr, sizeof(saddr));
    SET_PORT(&saddr, MicPortNumber);

    err = sendto(micSocket, sendBuf, totalLen, 0,
                 (struct sockaddr*)&saddr, AddrLen);
    if (err < 0) {
        return LastSocketError();
    }

    return err; // 返回实际发送的字节数
}

int opus_encoder_ctl_wrapper(OpusEncoder *enc, int request, int value) {
    if(value == -1) return opus_encoder_ctl(enc, request, value);
    return opus_encoder_ctl(enc, request, value);
}


