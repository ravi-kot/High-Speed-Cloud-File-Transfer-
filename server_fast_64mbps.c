/*
    Fast transfer Server 65mbps on 2025-09-13

*/

// server_optimized.c — Mathematically Optimized Server
// Build: gcc -O3 -march=native -D_FILE_OFFSET_BITS=64 server_optimized.c -o server_opt
// Run: ./server_opt <data_file> <client_ip> [rate_mbit]

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
# include <endian.h>
#else
# include <libkern/OSByteOrder.h>
# define htobe64(x) OSSwapHostToBigInt64(x)
# define be64toh(x) OSSwapBigToHostInt64(x)
#endif

#define TCP_PORT    5000
#define UDP_PORT    5001

#define DATA_SIZE   1024    // 1024 bytes data payload
#define APP_HEADER  8       // seq(4) + len(4)
#define BURST_SIZE  50000   // 50,000 packets per burst
#define CLIENT_EFFICIENCY 0.76  // Client processes at 76% of server rate
#define PROCESSING_OVERHEAD_MS 400  // 400ms for bitmap creation

// Structure for our UDP packets
typedef struct __attribute__((packed)) {
    uint32_t seq_net;      // Network byte order sequence number
    uint32_t len_net;      // Network byte order data length
    uint8_t  data[DATA_SIZE];
} UdpPkt;

// ===== Utility Functions =====
static void die(const char *m) { perror(m); exit(EXIT_FAILURE); }

static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static inline uint64_t now_ms(void) {
    return now_us() / 1000;
}

static inline void sleep_us(uint64_t us) {         
    if (us <= 0) return;
    struct timespec ts;
    ts.tv_sec = (time_t)(us / 1000000);
    ts.tv_nsec = (long)((us % 1000000) * 1000);
    nanosleep(&ts, NULL);
}

static void send_all(int s, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t*)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(s, p + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("send");
        }
        off += (size_t)n;
    }
}

static ssize_t recv_all(int s, void *buf, size_t len) {
    uint8_t *p = (uint8_t*)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(s, p + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        off += (size_t)n;
    }
    return (ssize_t)off;
}

// ===== Bitmap Functions =====
static inline void bm_set_missing(uint8_t *bm, uint32_t i) {
    bm[i >> 3] |= (1u << (i & 7u));
}

static inline int bm_is_missing(const uint8_t *bm, uint32_t i) {
    return ((bm[i >> 3] >> (i & 7u)) & 1u) != 0;
}

static inline void bm_clear(uint8_t *bm, uint32_t i) {
    bm[i >> 3] &= (uint8_t)~(1u << (i & 7u));
}

static inline uint64_t bm_count_missing(const uint8_t *bm, uint32_t NUM_PKT) {
    uint32_t BYTES = (NUM_PKT + 7u) / 8u;
    uint64_t miss = 0;
    for (uint32_t j = 0; j < BYTES; ++j) {
        miss += (uint64_t)__builtin_popcount((unsigned)bm[j]);
    }
    return miss;
}

// ===== Socket Setup =====
static int tcp_listen_any(uint16_t port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) die("socket TCP");
    
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) < 0) die("bind TCP");
    if (listen(s, 128) < 0) die("listen TCP");
    
    return s;
}

static int udp_tx_init(const char *dest_ip, int pacing_mbit) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) die("socket UDP");
    
    int sndbuf = 64 * 1024 * 1024;  // 64MB send buffer
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    return s;
}

// ===== Main Server =====
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <data_file> <client_ip> [rate_mbit]\n", argv[0]);
        return 1;
    }
    
    const char *data_path = argv[1];
    const char *client_ip = argv[2];
    int pacing_mbit = (argc >= 4) ? atoi(argv[3]) : 100;
    
    printf("=== Mathematically Optimized Server (DATA_SIZE=%d) ===\n", DATA_SIZE);
    printf("[CONFIG] Data file: %s\n", data_path);
    printf("[CONFIG] Client: %s\n", client_ip);
    printf("[CONFIG] Target rate: %d Mbit/s\n", pacing_mbit);
    printf("[CONFIG] Burst size: %d packets\n", BURST_SIZE);
    printf("[CONFIG] Client efficiency: %.1f%%\n", CLIENT_EFFICIENCY * 100);
    
    // Open and analyze data file
    int fd = open(data_path, O_RDONLY);
    if (fd < 0) die("open data");
    
    struct stat st;
    if (fstat(fd, &st) < 0) die("fstat");
    const uint64_t TOTAL = (uint64_t)st.st_size;
    const uint32_t NUM_PKT = (uint32_t)((TOTAL + DATA_SIZE - 1) / DATA_SIZE);
    const uint32_t LAST_VALID = (TOTAL % DATA_SIZE) ? (TOTAL % DATA_SIZE) : DATA_SIZE;
    const uint32_t BITMAP_BYTES = (NUM_PKT + 7u) / 8u;
    
    printf("[META] File size: %" PRIu64 " bytes\n", TOTAL);
    printf("[META] Packets: %u (last valid: %u bytes)\n", NUM_PKT, LAST_VALID);
    printf("[META] Bitmap size: %u bytes\n", BITMAP_BYTES);
    
    // Calculate optimal timing
    double packet_size_bytes = DATA_SIZE + APP_HEADER;
    double burst_size_bytes = BURST_SIZE * packet_size_bytes;
    double server_send_time = (burst_size_bytes * 8) / (pacing_mbit * 1e6);
    double client_process_time = (burst_size_bytes * 8) / (pacing_mbit * CLIENT_EFFICIENCY * 1e6);
    double processing_overhead = PROCESSING_OVERHEAD_MS / 1000.0;
    double total_client_time = client_process_time + processing_overhead;
    double wait_time = total_client_time - server_send_time;
    
    printf("[TIMING] Server send time: %.3f s\n", server_send_time);
    printf("[TIMING] Client process time: %.3f s\n", client_process_time);
    printf("[TIMING] Processing overhead: %.3f s\n", processing_overhead);
    printf("[TIMING] Total client time: %.3f s\n", total_client_time);
    printf("[TIMING] Server wait time: %.3f s\n", wait_time);
    
    // TCP control connection
    int tl = tcp_listen_any(TCP_PORT);
    printf("[TCP] Listening on port %d\n", TCP_PORT);
    
    struct sockaddr_in cli;
    socklen_t clen = sizeof(cli);
    int tcps = accept(tl, (struct sockaddr*)&cli, &clen);
    if (tcps < 0) die("accept");
    
    char cli_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cli.sin_addr, cli_ip, sizeof(cli_ip));
    printf("[TCP] Client %s:%d connected\n", cli_ip, ntohs(cli.sin_port));
    
    // Send session header
    uint64_t total_be = htobe64(TOTAL);
    uint32_t npkt_be = htonl(NUM_PKT);
    send_all(tcps, &total_be, sizeof(total_be));
    send_all(tcps, &npkt_be, sizeof(npkt_be));
    printf("[TCP] Session header sent\n");
    
    // UDP socket for data transfer
    int udps = udp_tx_init(client_ip, pacing_mbit);
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(UDP_PORT);
    if (inet_pton(AF_INET, client_ip, &dst.sin_addr) != 1) die("inet_pton");
    
    printf("[UDP] Target: %s:%d, packet size=%zu bytes\n", client_ip, UDP_PORT, sizeof(UdpPkt));
    
    // ===== Phase 1: Burst-based Transmission =====
    printf("[PHASE1] Starting burst-based transmission...\n");
    uint64_t start_time = now_us();
    
    UdpPkt pkt;
    uint32_t sent = 0;
    uint32_t burst_count = 0;
    
    for (uint32_t seq = 0; seq < NUM_PKT; seq += BURST_SIZE) {
        uint32_t burst_end = (seq + BURST_SIZE > NUM_PKT) ? NUM_PKT : seq + BURST_SIZE;
        uint32_t burst_size = burst_end - seq;
        burst_count++;
        
        printf("[BURST %d] Sending packets %u-%u (%u packets)...\n", 
               burst_count, seq, burst_end - 1, burst_size);
        
        uint64_t burst_start = now_us();
        
        // Send burst at full rate
        for (uint32_t i = seq; i < burst_end; ++i) {
            memset(&pkt, 0, sizeof(pkt));
            pkt.seq_net = htonl(i);
            
            uint32_t valid = (i == NUM_PKT - 1) ? LAST_VALID : DATA_SIZE;
            pkt.len_net = htonl(valid);
            
            ssize_t n = pread(fd, pkt.data, valid, (off_t)i * DATA_SIZE);
            if (n < 0) die("pread");
            if ((uint32_t)n < valid) {
                memset(pkt.data + n, 0, valid - (uint32_t)n);
            }
            
            sendto(udps, &pkt, sizeof(pkt), 0, (struct sockaddr*)&dst, sizeof(dst));
            sent++;
        }
        
        uint64_t burst_time = now_us() - burst_start;
        double burst_throughput = (8.0 * burst_size * packet_size_bytes) / (burst_time / 1e6) / 1e6;
        
        printf("[BURST %d] Sent %u packets in %.3f ms (%.2f Mbit/s)\n", 
               burst_count, burst_size, burst_time / 1000.0, burst_throughput);
        
        // Wait for client to process (except for last burst)
        if (burst_end < NUM_PKT) {
            uint64_t wait_us = (uint64_t)(wait_time * 1e6);
            printf("[BURST %d] Waiting %.3f s for client processing...\n", 
                   burst_count, wait_time);
            sleep_us(wait_us);
        }
    }
    
    uint64_t phase1_time = now_us() - start_time;
    double phase1_throughput = (8.0 * (double)TOTAL) / (phase1_time / 1e6) / 1e6;
    printf("[PHASE1] Complete! Sent %u packets in %.3f s (%.2f Mbit/s)\n", 
           sent, phase1_time / 1e6, phase1_throughput);
    
    // ===== Phase 2: NACK-based Retransmission =====
    printf("[PHASE2] Starting NACK rounds...\n");
    
    uint8_t *bitmap = (uint8_t*)malloc(BITMAP_BYTES);
    if (!bitmap) die("malloc bitmap");
    
    int round = 0;
    for (;;) {
        // Receive bitmap from client with timeout
        printf("[ROUND %d] Waiting for bitmap from client...\n", round + 1);
        
        struct timeval tv;
        tv.tv_sec = 10;  // 10 second timeout
        tv.tv_usec = 0;
        setsockopt(tcps, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        ssize_t received = recv_all(tcps, bitmap, BITMAP_BYTES);
        if (received != (ssize_t)BITMAP_BYTES) {
            printf("[ROUND %d] Client disconnected or bitmap incomplete\n", round + 1);
            break;
        }
        
        uint64_t missing = bm_count_missing(bitmap, NUM_PKT);
        round++;
        printf("[ROUND %d] Missing packets: %" PRIu64 " (%.1f%% loss)\n", 
               round, missing, (float)missing * 100.0 / NUM_PKT);
        
        // Send missing count to client
        uint64_t missing_be = htobe64(missing);
        send_all(tcps, &missing_be, sizeof(missing_be));
        
        if (missing == 0) {
            // Send EOR (End of Round)
            const char eor[] = "EOR";
            send_all(tcps, eor, 3);
            printf("[DONE] All packets received successfully!\n");
            break;
        }
        
        // Retransmit missing packets in smaller bursts
        printf("[ROUND %d] Retransmitting %" PRIu64 " missing packets...\n", round, missing);
        uint64_t retrans_start = now_us();
        uint32_t retransmitted = 0;
        
        for (uint32_t i = 0; i < NUM_PKT; ++i) {
            if (!bm_is_missing(bitmap, i)) continue;
            
            memset(&pkt, 0, sizeof(pkt));
            pkt.seq_net = htonl(i);
            
            uint32_t valid = (i == NUM_PKT - 1) ? LAST_VALID : DATA_SIZE;
            pkt.len_net = htonl(valid);
            
            ssize_t n = pread(fd, pkt.data, valid, (off_t)i * DATA_SIZE);
            if (n < 0) die("pread retrans");
            if ((uint32_t)n < valid) {
                memset(pkt.data + n, 0, valid - (uint32_t)n);
            }
            
            sendto(udps, &pkt, sizeof(pkt), 0, (struct sockaddr*)&dst, sizeof(dst));
            retransmitted++;
            
            // Small delay every 1000 packets
            if (retransmitted % 1000 == 0) {
                sleep_us(1000); // 1ms delay
            }
        }
        
        uint64_t retrans_time = now_us() - retrans_start;
        printf("[ROUND %d] Retransmitted %u packets in %.3f s\n", 
               round, retransmitted, retrans_time / 1e6);
        
        // Send EOR
        const char eor[] = "EOR";
        send_all(tcps, eor, 3);
    }
    
    uint64_t total_time = now_us() - start_time;
    double total_throughput = (8.0 * (double)TOTAL) / (total_time / 1e6) / 1e6;
    printf("[FINAL] Total transfer time: %.3f s, throughput: %.2f Mbit/s\n", 
           total_time / 1e6, total_throughput);
    
    // Cleanup
    free(bitmap);
    close(udps);
    close(tcps);
    close(tl);
    close(fd);
    
    printf("[SERVER] Shutdown complete\n");
    return 0;
}