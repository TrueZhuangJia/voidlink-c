#include "Limelight-internal.h"
#include "PlatformSockets.h"

static SOCKET micSocket = INVALID_SOCKET;
static PLT_THREAD micThread;
static bool micThreadStarted = false;
static PPLT_CRYPTO_CONTEXT micEncryptionCtx;

typedef struct _MICROPHONE_PACKET_HEADER {
    uint8_t flags;
    uint8_t packetType;
    uint16_t sequenceNumber;
    uint32_t timestamp;
    uint32_t ssrc;
} MICROPHONE_PACKET_HEADER, *PMICROPHONE_PACKET_HEADER;

#define MIC_PACKET_TYPE_OPUS 0x61 // 'a'

// 初始化麦克风流
int initializeMicrophoneStream(void) {
    // 如果已经初始化，直接返回成功
    if (micSocket != INVALID_SOCKET) {
        return 0;
    }
    
    micEncryptionCtx = PltCreateCryptoContext();
    if (micEncryptionCtx == NULL) {
        return -1;
    }
    
    // 创建UDP socket
    micSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen, 0, SOCK_QOS_TYPE_AUDIO);
    if (micSocket == INVALID_SOCKET) {
        PltDestroyCryptoContext(micEncryptionCtx);
        return LastSocketFail();
    }
    
    return 0;
}

// 关闭麦克风流
void destroyMicrophoneStream(void) {
    if (micSocket != INVALID_SOCKET) {
        closeSocket(micSocket);
        micSocket = INVALID_SOCKET;
    }
    
    if (micEncryptionCtx != NULL) {
        PltDestroyCryptoContext(micEncryptionCtx);
        micEncryptionCtx = NULL;
    }
}

// 发送麦克风数据
int sendMicrophoneData(const char* data, int length) {
    LC_SOCKADDR saddr;
    int err;
    
    if (micSocket == INVALID_SOCKET) {
        return -1;
    }
    
    memcpy(&saddr, &RemoteAddr, sizeof(saddr));
    SET_PORT(&saddr, MicPortNumber);
    
    err = sendto(micSocket, data, length, 0, (struct sockaddr*)&saddr, AddrLen);
    if (err < 0) {
        return LastSocketError();
    }
    
    return err;
}
