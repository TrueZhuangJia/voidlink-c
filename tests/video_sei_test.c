// Include the production depacketizer to exercise the exact Annex-B NAL
// scanning and slow path used by IDR reassembly. Linker dead stripping removes
// the network and queue paths from this harness.
#include "../src/VideoDepacketizer.c"

#include <stdio.h>
#include <stdlib.h>

int NegotiatedVideoFormat;
DECODER_RENDERER_CALLBACKS VideoCallbacks;
int AppVersionQuad[4] = {7, 1, 450, -1};
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;

static bool splitIdrSubmitted;

static int captureSplitIdr(PDECODE_UNIT decodeUnit) {
    splitIdrSubmitted = decodeUnit->frameType == FRAME_TYPE_IDR;
    return DR_OK;
}

static void sendSyntheticPacket(uint32_t streamPacketIndex, uint8_t flags,
                                const char *payload, size_t payloadLength) {
    size_t packetLength = sizeof(NV_VIDEO_PACKET) + payloadLength;
    char *allocation = calloc(1, sizeof(LENTRY_INTERNAL) + packetLength);
    PLENTRY_INTERNAL holder = (PLENTRY_INTERNAL)allocation;
    PNV_VIDEO_PACKET packet = (PNV_VIDEO_PACKET)(allocation + sizeof(LENTRY_INTERNAL));
    holder->allocPtr = allocation;
    packet->streamPacketIndex = streamPacketIndex << 8;
    packet->frameIndex = 1;
    packet->flags = flags;
    memcpy(packet + 1, payload, payloadLength);
    PLENTRY_INTERNAL existingEntry = holder;
    processRtpPayload(packet, (int)packetLength, 1, 0, 90000, &existingEntry);
    if (existingEntry != NULL) {
        free(existingEntry->allocPtr);
    }
}

uint64_t PltGetMicroseconds(void) {
    return 1;
}

void notifyKeyFrameReceived(void) {
}

void connectionReceivedCompleteFrame(uint32_t frameIndex, bool frameIsLTR) {
    (void)frameIndex;
    (void)frameIsLTR;
}

void connectionDetectedFrameLoss(uint32_t startFrame, uint32_t endFrame) {
    (void)startFrame;
    (void)endFrame;
}

void LiRequestIdrFrame(void) {
}

bool LiGetCurrentHostDisplayHdrMode(void) {
    return false;
}

void BbInitializeWrappedBuffer(PBYTE_BUFFER buffer, char *data, int offset,
                               int length, int byteOrder) {
    buffer->buffer = data;
    buffer->position = (unsigned int)offset;
    buffer->length = (unsigned int)length;
    buffer->byteOrder = (unsigned int)byteOrder;
}

bool BbGet16(PBYTE_BUFFER buffer, uint16_t *value) {
    if (buffer->position + 2 > buffer->length) {
        return false;
    }
    const uint8_t *data = (const uint8_t *)buffer->buffer + buffer->position;
    *value = buffer->byteOrder == BYTE_ORDER_BIG ?
        (uint16_t)((data[0] << 8) | data[1]) :
        (uint16_t)(data[0] | (data[1] << 8));
    buffer->position += 2;
    return true;
}

int LbqOfferQueueItem(PLINKED_BLOCKING_QUEUE queue, void *data,
                      PLINKED_BLOCKING_QUEUE_ENTRY entry) {
    (void)queue;
    (void)data;
    (void)entry;
    return LBQ_SUCCESS;
}

PLINKED_BLOCKING_QUEUE_ENTRY LbqFlushQueueItems(PLINKED_BLOCKING_QUEUE queue) {
    (void)queue;
    return NULL;
}

int main(void) {
    // Prefix SEI (type 39), followed by the VPS (type 32) that starts an IDR.
    // This is the layout emitted by hosts carrying HDR10+ metadata.
    char data[] = {
        0, 0, 0, 1, (char)(39 << 1), 0x01,
        0, 0, 0, 1, (char)(32 << 1), 0x01,
    };
    BUFFER_DESC descriptor = { data, 0, (unsigned int)sizeof(data) };

    NegotiatedVideoFormat = VIDEO_FORMAT_H265;
    VideoCallbacks.capabilities = CAPABILITY_PRESERVE_HEVC_SEI;
    if (!isIdrFrameStartAfterPreservedSei(&descriptor)) {
        fprintf(stderr, "FAIL: preserved HEVC SEI must not hide IDR VPS\n");
        return EXIT_FAILURE;
    }

    data[4] = (char)(40 << 1);
    if (!shouldPreserveHevcSei(&descriptor)) {
        fprintf(stderr, "FAIL: suffix HEVC SEI should be preserved\n");
        return EXIT_FAILURE;
    }

    // A preserved suffix-SEI may share the RTP payload with the picture NAL.
    // Exercise the slow path so debug builds cover the same-packet assertion.
    char packet[] = {
        0, 0, 0, 1, (char)(32 << 1), 0x01, // VPS
        0, 0, 0, 1, (char)(33 << 1), 0x01, // SPS
        0, 0, 0, 1, (char)(34 << 1), 0x01, // PPS
        0, 0, 0, 1, (char)(19 << 1), 0x01, // IDR slice
        0, 0, 0, 1, (char)(40 << 1), 0x01, // suffix-SEI
    };
    BUFFER_DESC packetDescriptor = { packet, 0, (unsigned int)sizeof(packet) };
    processAvcHevcRtpPayloadSlow(&packetDescriptor, NULL);
    if (nalChainTail == NULL || nalChainTail->length < 6) {
        fprintf(stderr, "FAIL: slow path dropped picture with suffix HEVC SEI\n");
        return EXIT_FAILURE;
    }
    bool suffixFound = false;
    unsigned int tailLength = (unsigned int)nalChainTail->length;
    for (unsigned int i = 0; i + 5 < tailLength; i++) {
        if ((unsigned char)nalChainTail->data[i] == 0 &&
            (unsigned char)nalChainTail->data[i + 1] == 0 &&
            (unsigned char)nalChainTail->data[i + 2] == 0 &&
            (unsigned char)nalChainTail->data[i + 3] == 1 &&
            (((unsigned char)nalChainTail->data[i + 4] & 0x7E) >> 1) == 40) {
            suffixFound = true;
            break;
        }
    }
    if (!suffixFound) {
        fprintf(stderr, "FAIL: slow path did not retain suffix HEVC SEI\n");
        return EXIT_FAILURE;
    }
    while (nalChainHead != NULL) {
        PLENTRY_INTERNAL entry = (PLENTRY_INTERNAL)nalChainHead;
        nalChainHead = entry->entry.next;
        free(entry->allocPtr);
    }
    nalChainTail = NULL;
    nalChainDataLength = 0;

    // Preserved prefix-SEI is also legal between HEVC parameter sets. The
    // structural validator must ignore it while retaining it in the chain.
    char vps[] = {0, 0, 0, 1, (char)(32 << 1), 0x01};
    char seiA[] = {0, 0, 0, 1, (char)(39 << 1), 0x01};
    char sps[] = {0, 0, 0, 1, (char)(33 << 1), 0x01};
    char seiB[] = {0, 0, 0, 1, (char)(40 << 1), 0x01};
    char pps[] = {0, 0, 0, 1, (char)(34 << 1), 0x01};
    char slice[] = {0, 0, 0, 1, (char)(19 << 1), 0x01};
    LENTRY entries[6] = {
        {.data = vps, .length = (int)sizeof(vps)},
        {.data = seiA, .length = (int)sizeof(seiA)},
        {.data = sps, .length = (int)sizeof(sps)},
        {.data = seiB, .length = (int)sizeof(seiB)},
        {.data = pps, .length = (int)sizeof(pps)},
        {.data = slice, .length = (int)sizeof(slice)},
    };
    for (size_t i = 0; i < 6; i++) {
        entries[i].next = i + 1 < 6 ? &entries[i + 1] : NULL;
        entries[i].bufferType = getBufferFlags(entries[i].data, entries[i].length);
    }
    DECODE_UNIT decodeUnit = {0};
    decodeUnit.bufferList = &entries[0];
    decodeUnit.fullLength = sizeof(vps) + sizeof(seiA) + sizeof(sps) +
                            sizeof(seiB) + sizeof(pps) + sizeof(slice);
    decodeUnit.frameType = FRAME_TYPE_IDR;
    validateDecodeUnitForPlayback(&decodeUnit);

    // Reproduce a real split frame: the first RTP payload contains only a
    // preserved prefix-SEI, while the next payload contains VPS/SPS/PPS and
    // the IDR slice. The production RTP path must carry the parse state across
    // packets and submit the resulting IDR instead of dropping it.
    StreamConfig.colorSpace = 0;
    VideoCallbacks.capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PRESERVE_HEVC_SEI;
    VideoCallbacks.submitDecodeUnit = captureSplitIdr;
    NegotiatedVideoFormat = VIDEO_FORMAT_H265;
    nextFrameNumber = 1;
    startFrameNumber = 0;
    waitingForIdrFrame = true;
    waitingForRefInvalFrame = false;
    waitingForNextSuccessfulFrame = false;
    decodingFrame = false;
    lastPacketInStream = UINT32_MAX;
    nalChainHead = nalChainTail = NULL;
    nalChainDataLength = 0;
    pendingHevcIdrParse = false;

    const char firstPayload[] = {
        1, 0, 0, 2, 0, 0, 0, 0, // HEVC IDR frame header
        0, 0, 0, 1, (char)(39 << 1), 0x01, // prefix-SEI only
    };
    const char secondPayload[] = {
        0, 0, 0, 1, (char)(32 << 1), 0x01, // VPS
        0, 0, 0, 1, (char)(33 << 1), 0x01, // SPS
        0, 0, 0, 1, (char)(34 << 1), 0x01, // PPS
        0, 0, 0, 1, (char)(19 << 1), 0x01, // IDR slice
    };

    sendSyntheticPacket(0, FLAG_SOF, firstPayload, sizeof(firstPayload));
    sendSyntheticPacket(1, FLAG_EOF, secondPayload, sizeof(secondPayload));
    if (!splitIdrSubmitted || waitingForIdrFrame || pendingHevcIdrParse) {
        fprintf(stderr, "FAIL: split HEVC IDR with prefix-SEI was not submitted\n");
        return EXIT_FAILURE;
    }

    VideoCallbacks.capabilities = 0;
    if (shouldPreserveHevcSei(&descriptor)) {
        fprintf(stderr, "FAIL: renderer without opt-in must not preserve SEI\n");
        return EXIT_FAILURE;
    }

    puts("HEVC SEI tests passed");
    return EXIT_SUCCESS;
}
