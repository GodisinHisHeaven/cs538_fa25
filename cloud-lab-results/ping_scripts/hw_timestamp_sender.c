
// SENDER: Sends packet and measures round-trip time with HW timestamps
// gcc -Wall -o hw_timestamp_sender hw_timestamp_sender.c
// sudo ./hw_timestamp_sender <receiver-ip> <port> <iface-name>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/errqueue.h>
#include <arpa/inet.h>

static void extract_ts(struct msghdr *msg, struct timespec *ts) {
    struct cmsghdr *cmsg;
    for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type  == SO_TIMESTAMPING) {
            struct timespec *ts_arr = (struct timespec *) CMSG_DATA(cmsg);
            // ts_arr[2] = hardware timestamp
            if (ts_arr[2].tv_sec != 0 || ts_arr[2].tv_nsec != 0) {
                *ts = ts_arr[2];
                return;
            }
        }
    }
    ts->tv_sec = ts->tv_nsec = 0;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <dst_ip> <port> <iface>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *dst_ip = argv[1];
    int port = atoi(argv[2]);
    const char *iface = argv[3];

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); exit(EXIT_FAILURE); }

    // Bind to specific interface
    if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, iface, strlen(iface)) < 0) {
        perror("SO_BINDTODEVICE");
        exit(EXIT_FAILURE);
    }

    // Configure hardware timestamping on the interface
    struct ifreq ifr;
    struct hwtstamp_config hwts_config;
    
    memset(&ifr, 0, sizeof(ifr));
    memset(&hwts_config, 0, sizeof(hwts_config));
    
    strncpy(ifr.ifr_name, iface, sizeof(ifr.ifr_name) - 1);
    hwts_config.tx_type = HWTSTAMP_TX_ON;
    hwts_config.rx_filter = HWTSTAMP_FILTER_ALL;
    
    ifr.ifr_data = (char *)&hwts_config;
    
    if (ioctl(sock, SIOCSHWTSTAMP, &ifr) < 0) {
        perror("SIOCSHWTSTAMP");
        fprintf(stderr, "Hardware timestamping not supported on %s\n", iface);
        exit(EXIT_FAILURE);
    }

    // Enable hardware timestamping on socket
    int timestamping_flags =
          SOF_TIMESTAMPING_TX_HARDWARE
        | SOF_TIMESTAMPING_RX_HARDWARE
        | SOF_TIMESTAMPING_RAW_HARDWARE;

    if (setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPING,
                   &timestamping_flags, sizeof(timestamping_flags)) < 0) {
        perror("SO_TIMESTAMPING");
        exit(EXIT_FAILURE);
    }

    // Set receive timeout
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Destination address
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port)
    };
    inet_pton(AF_INET, dst_ip, &addr.sin_addr);

    char buf[64];
    snprintf(buf, sizeof(buf), "PING %ld", time(NULL));
    char ctrl[2048];
    char data[2048];

    struct timespec tx_hw = {0}, rx_hw = {0};

    printf("Sending packet to %s:%d via %s...\n", dst_ip, port, iface);
    
    // Send packet
    if (sendto(sock, buf, strlen(buf), 0,
               (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("sendto");
        exit(EXIT_FAILURE);
    }

    printf("Retrieving TX timestamp from error queue...\n");
    
    // Receive TX timestamp from error queue
    {
        struct msghdr msg = {0};
        struct iovec iov = { .iov_base = data, .iov_len = sizeof(data) };
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof(ctrl);

        int ret = recvmsg(sock, &msg, MSG_ERRQUEUE);
        if (ret >= 0) {
            extract_ts(&msg, &tx_hw);
            if (tx_hw.tv_sec != 0 || tx_hw.tv_nsec != 0) {
                printf("✓ Got TX hardware timestamp\n");
            } else {
                printf("✗ TX timestamp is zero (hardware may not be enabled)\n");
            }
        } else {
            perror("recvmsg MSG_ERRQUEUE");
            printf("✗ Failed to get TX timestamp\n");
        }
    }

    printf("Waiting for echo reply...\n");
    
    // Receive echo reply with RX timestamp
    {
        struct msghdr msg = {0};
        struct iovec iov = { .iov_base = data, .iov_len = sizeof(data) };
        struct sockaddr_in src_addr;
        
        msg.msg_name = &src_addr;
        msg.msg_namelen = sizeof(src_addr);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof(ctrl);

        int ret = recvmsg(sock, &msg, 0);
        if (ret >= 0) {
            data[ret] = '\0';
            printf("✓ Received: %s\n", data);
            extract_ts(&msg, &rx_hw);
            if (rx_hw.tv_sec != 0 || rx_hw.tv_nsec != 0) {
                printf("✓ Got RX hardware timestamp\n");
            } else {
                printf("✗ RX timestamp is zero\n");
            }
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("✗ Timeout waiting for reply\n");
            } else {
                perror("recvmsg");
            }
            printf("Make sure the receiver is running!\n");
        }
    }

    printf("\n=== RESULTS ===\n");
    
    // Check if we got valid timestamps
    if (tx_hw.tv_sec == 0 && tx_hw.tv_nsec == 0) {
        printf("ERROR: No TX hardware timestamp\n");
    } else {
        printf("TX HW timestamp: %ld.%09ld\n", tx_hw.tv_sec, tx_hw.tv_nsec);
    }
    
    if (rx_hw.tv_sec == 0 && rx_hw.tv_nsec == 0) {
        printf("ERROR: No RX hardware timestamp\n");
    } else {
        printf("RX HW timestamp: %ld.%09ld\n", rx_hw.tv_sec, rx_hw.tv_nsec);
    }

    // Compute latency from timestamps (ns precision)
    if ((tx_hw.tv_sec != 0 || tx_hw.tv_nsec != 0) &&
        (rx_hw.tv_sec != 0 || rx_hw.tv_nsec != 0)) {
        long long latency_ns = (long long)(rx_hw.tv_sec - tx_hw.tv_sec) * 1000000000LL +
                               (long long)(rx_hw.tv_nsec - tx_hw.tv_nsec);

        printf("\nROUND-TRIP LATENCY:\n");
        printf("  %lld nanoseconds\n", latency_ns);
        printf("  %.3f microseconds\n", latency_ns / 1000.0);
        printf("  %.6f milliseconds\n", latency_ns / 1000000.0);
    }

    close(sock);
    return 0;
}