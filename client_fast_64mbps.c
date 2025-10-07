/*
   Fast transfer Client 65mbps on 2025-09-13
*/

// client_optimized.c — Mathematically Optimized Client
// Build: gcc -O3 -march=native -D_FILE_OFFSET_BITS=64 -pthread client_optimized.c -o client_opt
// Run: ./client_opt <server_ip> <output_file> [idle_ms] [rate_mbit] [margin_ms]

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
# include <endian.h>
#else
# include <libkern/OSByteOrder.h>
# define be64toh(x) OSSwapBigToHostInt64(x)
#endif

#define TCP_PORT    5000
#define UDP_PORT    5001

#define DATA_SIZE   1024    // 1024 bytes data payload
#define APP_HEADER  8       // seq(4) + len(4)

// Structure for our UDP packets
typedef struct __attribute__((packed)) {
    uint32_t seq_net;      // Network byte order sequence number
    uint32_t len_net;      // Network byte order data length
    uint8_t  data[DATA_SIZE];
} UdpPkt;

// Global variables for threading
static uint8_t *g_base = NULL;
static uint8_t *g_bitmap = NULL;
static uint32_t g_NUM_PKT = 0;
static uint32_t g_LAST_VALID = 0;
static uint64_t g_total_received = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_writer_active = false;

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
static inline void bm_init_all_missing(uint8_t *bm, uint32_t NUM_PKT) {
    uint32_t BYTES = (NUM_PKT + 7u) / 8u;
    memset(bm, 0xFF, BYTES);
    uint32_t rem = NUM_PKT & 7u;
    if (rem) bm[BYTES - 1] &= (uint8_t)((1u << rem) - 1u);
}

static inline int bm_is_missing(const uint8_t *bm, uint32_t i) {
    return ((bm[i >> 3] >> (i & 7u)) & 1u) != 0;
}

static inline void bm_clear(uint8_t *bm, uint32_t i) {
    bm[i >> 3] &= (uint8_t)~(1u << (i & 7u));
}

static inline int bm_all_received(const uint8_t *bm, uint32_t NUM_PKT) {
    uint32_t BYTES = (NUM_PKT + 7u) / 8u;
    for (uint32_t j = 0; j < BYTES; ++j) if (bm[j]) return 0;
    return 1;
}

static inline uint64_t bm_count_missing(const uint8_t *bm, uint32_t NUM_PKT) {
    uint32_t BYTES = (NUM_PKT + 7u) / 8u;
    uint64_t miss = 0;
    for (uint32_t j = 0; j < BYTES; ++j) {
        miss += (uint64_t)__builtin_popcount((unsigned)bm[j]);
    }
    return miss;
}

// ===== Writer Thread =====
static void* writer_thread(void *arg) {
    (void)arg;
    printf("[WRITER] Writer thread started\n");
    
    uint64_t last_written = 0;
    uint64_t last_time = now_ms();
    
    while (g_writer_active) {
        pthread_mutex_lock(&g_mutex);
        uint64_t current_received = g_total_received;
        pthread_mutex_unlock(&g_mutex);
        
        if (current_received > last_written) {
            uint64_t current_time = now_ms();
            double rate = (current_received - last_written) * 1000.0 / (current_time - last_time);
            
            printf("[WRITER] Written %llu packets (%.1f%%)\n", 
                   (unsigned long long)current_received, 
                   (float)current_received * 100.0 / g_NUM_PKT);
            
            last_written = current_received;
            last_time = current_time;
        }
        
        usleep(100000); // 100ms sleep
    }
    
    printf("[WRITER] Writer thread finished, wrote %llu packets\n", 
           (unsigned long long)g_total_received);
    return NULL;
}

// ===== Main Client =====
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <server_ip> <output_file> [idle_ms] [rate_mbit] [margin_ms]\n", argv[0]);
        return 1;
    }
    
    const char *server_ip = argv[1];
    const char *output_file = argv[2];
    uint64_t idle_ms = (argc >= 4) ? strtoull(argv[3], NULL, 10) : 5000;
    int pacing_mbit = (argc >= 5) ? atoi(argv[4]) : 100;
    uint64_t margin_ms = (argc >= 6) ? strtoull(argv[5], NULL, 10) : 2000;
    
    printf("=== Mathematically Optimized Client (DATA_SIZE=%d) ===\n", DATA_SIZE);
    printf("[CONFIG] Server: %s\n", server_ip);
    printf("[CONFIG] Output: %s\n", output_file);
    printf("[CONFIG] Idle timeout: %llu ms\n", (unsigned long long)idle_ms);
    printf("[CONFIG] Pacing: %d Mbit/s\n", pacing_mbit);
    printf("[CONFIG] Margin: %llu ms\n", (unsigned long long)margin_ms);
    
    // UDP socket setup
    printf("[UDP] Setting up UDP socket on port %d...\n", UDP_PORT);
    int udps = socket(AF_INET, SOCK_DGRAM, 0);
    if (udps < 0) die("socket UDP");
    
    int rcvbuf = 128 * 1024 * 1024;  // 128MB receive buffer
    setsockopt(udps, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    
    struct sockaddr_in ua;
    memset(&ua, 0, sizeof(ua));
    ua.sin_family = AF_INET;
    ua.sin_port = htons(UDP_PORT);
    ua.sin_addr.s_addr = INADDR_ANY;
    if (bind(udps, (struct sockaddr*)&ua, sizeof(ua)) < 0) die("bind UDP");
    
    // Non-blocking UDP
    int fl = fcntl(udps, F_GETFL, 0);
    if (fl < 0) die("fcntl get");
    if (fcntl(udps, F_SETFL, fl | O_NONBLOCK) < 0) die("fcntl set");
    printf("[UDP] UDP socket ready on port %d (rcvbuf=%d MB)\n", UDP_PORT, rcvbuf / (1024*1024));
    
    // TCP control connection
    printf("[TCP] Connecting to %s:%d...\n", server_ip, TCP_PORT);
    int tcps = socket(AF_INET, SOCK_STREAM, 0);
    if (tcps < 0) die("socket TCP");
    
    int nodelay = 1;
    setsockopt(tcps, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    
    struct sockaddr_in ta;
    memset(&ta, 0, sizeof(ta));
    ta.sin_family = AF_INET;
    ta.sin_port = htons(TCP_PORT);
    if (inet_pton(AF_INET, server_ip, &ta.sin_addr) != 1) die("inet_pton");
    
    if (connect(tcps, (struct sockaddr*)&ta, sizeof(ta)) < 0) {
        printf("[ERROR] Failed to connect to server %s:%d\n", server_ip, TCP_PORT);
        die("connect TCP");
    }
    printf("[TCP] Connected to %s:%d successfully!\n", server_ip, TCP_PORT);
    
    // Receive session header
    printf("[TCP] Receiving session header...\n");
    uint64_t total_be = 0;
    uint32_t npkt_be = 0;
    if (recv_all(tcps, &total_be, sizeof(total_be)) != (ssize_t)sizeof(total_be)) die("recv total");
    if (recv_all(tcps, &npkt_be, sizeof(npkt_be)) != (ssize_t)sizeof(npkt_be)) die("recv npkt");
    
    uint64_t TOTAL = be64toh(total_be);
    uint32_t NUM_PKT = ntohl(npkt_be);
    uint32_t LAST_VALID = (TOTAL % DATA_SIZE) ? (TOTAL % DATA_SIZE) : DATA_SIZE;
    uint32_t BITMAP_BYTES = (NUM_PKT + 7u) / 8u;
    
    // Set global variables
    g_NUM_PKT = NUM_PKT;
    g_LAST_VALID = LAST_VALID;
    
    printf("[META] File size: %" PRIu64 " bytes\n", TOTAL);
    printf("[META] Packets: %u (last valid: %u bytes)\n", NUM_PKT, LAST_VALID);
    printf("[META] Bitmap size: %u bytes\n", BITMAP_BYTES);
    
    // Create output file
    printf("[FILE] Creating output file: %s\n", output_file);
    int fd = open(output_file, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("open output");
    if (ftruncate(fd, (off_t)TOTAL) != 0) die("ftruncate");
    
    g_base = mmap(NULL, TOTAL, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_base == MAP_FAILED) die("mmap");
    printf("[FILE] Output file ready (%" PRIu64 " bytes)\n", TOTAL);
    
    // Initialize bitmap
    g_bitmap = (uint8_t*)malloc(BITMAP_BYTES);
    if (!g_bitmap) die("malloc bitmap");
    bm_init_all_missing(g_bitmap, NUM_PKT);
    
    // Start writer thread
    g_writer_active = true;
    pthread_t writer_tid;
    if (pthread_create(&writer_tid, NULL, writer_thread, NULL) != 0) {
        die("pthread_create");
    }
    
    // ===== Phase 1: Initial UDP Receive =====
    printf("[PHASE1] Starting UDP receive (idle cutoff %llu ms)\n", (unsigned long long)idle_ms);
    uint64_t start_time = now_us();
    uint64_t last_rx = now_ms();
    uint64_t checksum_errors = 0;
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    
    for (;;) {
        UdpPkt pkt;
        ssize_t n = recvfrom(udps, &pkt, sizeof(pkt), 0, (struct sockaddr*)&src, &slen);
        
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (now_ms() - last_rx > idle_ms) break;
                struct timespec ts = {0, 100000}; // 100us
                nanosleep(&ts, NULL);
                continue;
            }
            if (errno == EINTR) continue;
            die("recvfrom phase1");
        }
        
        if (n != (ssize_t)sizeof(pkt)) continue;
        
        uint32_t seq = ntohl(pkt.seq_net);
        uint32_t len = ntohl(pkt.len_net);
        
        if (seq >= NUM_PKT) continue;
        
        // Validate packet length
        uint32_t expected_len = (seq == NUM_PKT - 1) ? LAST_VALID : DATA_SIZE;
        if (len != expected_len) {
            checksum_errors++;
            continue;
        }
        
        if (bm_is_missing(g_bitmap, seq)) {
            pthread_mutex_lock(&g_mutex);
            memcpy(g_base + (size_t)seq * DATA_SIZE, pkt.data, len);
            bm_clear(g_bitmap, seq);
            g_total_received++;
            pthread_mutex_unlock(&g_mutex);
            last_rx = now_ms();
            
            if (g_total_received % 25000 == 0) {
                printf("[PHASE1] Progress: %llu/%u packets (%.1f%%)\n", 
                       (unsigned long long)g_total_received, NUM_PKT, 
                       (float)g_total_received * 100.0 / NUM_PKT);
            }
        }
    }
    
    double loss_rate = (float)(NUM_PKT - g_total_received) * 100.0 / NUM_PKT;
    printf("[PHASE1] Complete! Received %llu/%u packets (%.1f%% loss)\n", 
           (unsigned long long)g_total_received, NUM_PKT, loss_rate);
    printf("[PHASE1] Checksum errors: %llu\n", (unsigned long long)checksum_errors);
    
    // ===== Phase 2: NACK Rounds =====
    printf("[PHASE2] Starting NACK rounds...\n");
    int round = 0;
    
    while (!bm_all_received(g_bitmap, NUM_PKT)) {
        // Send bitmap to server
        printf("[ROUND %d] Sending bitmap (%u bytes)...\n", round + 1, BITMAP_BYTES);
        send_all(tcps, g_bitmap, BITMAP_BYTES);
        
        // Receive missing count from server
        uint64_t missing_be = 0;
        if (recv_all(tcps, &missing_be, 8) != 8) die("recv missing count");
        uint64_t missing = be64toh(missing_be);
        round++;
        
        printf("[ROUND %d] Server reports %" PRIu64 " missing packets\n", round, missing);
        
        if (missing == 0) {
            // Receive EOR
            char eor[3];
            if (recv_all(tcps, eor, 3) != 3) die("recv EOR");
            printf("[ROUND %d] All packets received!\n", round);
            break;
        }
        
        // Receive window
        uint64_t window_ms = 30000; // 30 second window
        if (missing > 100000) window_ms = 60000; // 1 minute for high loss
        
        printf("[ROUND %d] Receiving window: %llu ms\n", round, (unsigned long long)window_ms);
        
        uint64_t deadline = now_ms() + window_ms;
        size_t got_this_round = 0;
        int got_eor = 0;
        char tbuf[4];
        size_t tf = 0;
        
        while (now_ms() < deadline && !got_eor) {
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(udps, &rf);
            FD_SET(tcps, &rf);
            
            int maxfd = (udps > tcps ? udps : tcps) + 1;
            uint64_t rem = deadline - now_ms();
            struct timeval tv;
            tv.tv_sec = rem / 1000;
            tv.tv_usec = (rem % 1000) * 1000;
            
            int sel = select(maxfd, &rf, NULL, NULL, &tv);
            if (sel < 0) {
                if (errno == EINTR) continue;
                die("select");
            }
            if (sel == 0) {
                printf("[ROUND %d] Window timeout reached\n", round);
                break;
            }
            
            // Check for EOR on TCP
            if (FD_ISSET(tcps, &rf)) {
                ssize_t k = recv(tcps, tbuf + tf, sizeof(tbuf) - tf, MSG_DONTWAIT);
                if (k > 0) {
                    tf += (size_t)k;
                    if (tf >= 3 && tbuf[tf-3] == 'E' && tbuf[tf-2] == 'O' && tbuf[tf-1] == 'R') {
                        got_eor = 1;
                        printf("[ROUND %d] Received EOR signal\n", round);
                    }
                    if (tf > 3) {
                        memmove(tbuf, tbuf + tf - 3, 3);
                        tf = 3;
                    }
                } else if (k == 0) {
                    printf("[ROUND %d] TCP connection closed by server\n", round);
                    break;
                }
            }
            
            // Process UDP packets
            if (FD_ISSET(udps, &rf)) {
                UdpPkt pkt;
                ssize_t n = recvfrom(udps, &pkt, sizeof(pkt), 0, NULL, NULL);
                if (n == (ssize_t)sizeof(pkt)) {
                    uint32_t seq = ntohl(pkt.seq_net);
                    uint32_t len = ntohl(pkt.len_net);
                    
                    if (seq < NUM_PKT && bm_is_missing(g_bitmap, seq)) {
                        uint32_t expected_len = (seq == NUM_PKT - 1) ? LAST_VALID : DATA_SIZE;
                        if (len == expected_len) {
                            pthread_mutex_lock(&g_mutex);
                            memcpy(g_base + (size_t)seq * DATA_SIZE, pkt.data, len);
                            bm_clear(g_bitmap, seq);
                            g_total_received++;
                            pthread_mutex_unlock(&g_mutex);
                            got_this_round++;
                        }
                    }
                }
            }
        }
        
        // Ensure we get EOR if not already received
        if (!got_eor) {
            printf("[ROUND %d] Waiting for EOR signal...\n", round);
            char eor[3];
            if (recv_all(tcps, eor, 3) != 3) {
                printf("[ROUND %d] Failed to receive EOR: %s\n", round, strerror(errno));
                break;
            }
            printf("[ROUND %d] Received EOR signal\n", round);
        }
        
        printf("[ROUND %d] Received %zu packets\n", round, got_this_round);
    }
    
    // Stop writer thread
    g_writer_active = false;
    pthread_join(writer_tid, NULL);
    
    uint64_t total_time = now_us() - start_time;
    double total_throughput = (8.0 * (double)TOTAL) / (total_time / 1e6) / 1e6;
    printf("[FINAL] Transfer complete in %.3f s, throughput: %.2f Mbit/s\n", 
           total_time / 1e6, total_throughput);
    
    // Ensure data is written to disk
    printf("[FILE] Syncing data to disk...\n");
    msync(g_base, TOTAL, MS_SYNC);
    munmap(g_base, TOTAL);
    close(fd);
    close(udps);
    close(tcps);
    free(g_bitmap);
    
    printf("[CLIENT] Shutdown complete\n");
    return 0;
}