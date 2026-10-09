// Include the production depacketizer to exercise the exact Annex-B NAL
// scanning and slow path used by IDR reassembly. Linker dead stripping removes
// the network and queue paths from this harness.
#include "../src/VideoDepacketizer.c"

#include <stdio.h>
#include <stdlib.h>

int NegotiatedVideoFormat;
DECODER_RENDERER_CALLBACKS VideoCallbacks;

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

    VideoCallbacks.capabilities = 0;
    if (shouldPreserveHevcSei(&descriptor)) {
        fprintf(stderr, "FAIL: renderer without opt-in must not preserve SEI\n");
        return EXIT_FAILURE;
    }

    puts("HEVC SEI tests passed");
    return EXIT_SUCCESS;
}
