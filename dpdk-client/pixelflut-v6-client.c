#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <locale.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <argp.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

#include "image.h"

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

#define MAX(x, y) (((x) > (y)) ? (x) : (y))

#define STATS_INTERVAL_MS 1000

static struct argp_option options[] = {
    {"image", 'i', "<image-file>", 0,  "Path to image to flut"},
    {"pingxelflut", 'p', "<ipv6-target>", 0, "Use pingxelflut protocol instead of pixelflut v6, fluting to the target IPv6 address. IPv4 is currently not supported"},
    {0}
};
struct arguments {
    char *image_file;
    bool use_pingxelflut;
    struct in6_addr pingxelflut_target;
};

static struct rte_ether_addr parse_mac(char *mac_str) {
    struct rte_ether_addr mac_addr;
    if (sscanf(mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac_addr.addr_bytes[0], &mac_addr.addr_bytes[1],
        &mac_addr.addr_bytes[2], &mac_addr.addr_bytes[3],
        &mac_addr.addr_bytes[4], &mac_addr.addr_bytes[5]) != 6)
    {
        fprintf(stderr, "Could not parse MAC address %s\n", mac_str);
        // TODO: Better error handling
    }

    return mac_addr;
}

static struct in6_addr parse_ipv6(char *ipv6_str) {
    struct in6_addr ipv6_addr;
    if (inet_pton(AF_INET6, ipv6_str, &ipv6_addr) == 1) {
        return ipv6_addr;
    } else {
        fprintf(stderr, "Could not parse IPv6 address %s\n", ipv6_str);
        // TODO: Better error handling
    }
}

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
  // Get the input argument from argp_parse, which we know is a pointer to our arguments structure
  struct arguments *arguments = state->input;

  switch (key)
    {
    case 'i':
      arguments->image_file = arg;
      break;
    case 'p':
      arguments->use_pingxelflut = true;
      arguments->pingxelflut_target = parse_ipv6(arg);
      break;

    case ARGP_KEY_END:
        if (arguments->image_file == NULL) {
            argp_failure(state, 1, 0, "--image required. See --help for more information");
            exit(ARGP_ERR_UNKNOWN);
      }

    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

const char *argp_program_version = "pixelflut-v6-client 0.1.0";
static char doc[] = "Fast pixelflut v6 or pingxelflut client using DPDK";
static char args_doc[] = "--image <image-file>";
static struct argp argp = { options, parse_opt, args_doc, doc };

// Main functional part of port initialization
static inline int port_init(uint16_t port, struct rte_mempool *mbuf_pool) {
    struct rte_eth_conf port_conf;
    const uint16_t rx_rings = 1, tx_rings = 1;
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    int retval;
    uint16_t q;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_txconf txconf;

    if (!rte_eth_dev_is_valid_port(port))
        return -1;

    memset(&port_conf, 0, sizeof(struct rte_eth_conf));

    retval = rte_eth_dev_info_get(port, &dev_info);
    if (retval != 0) {
        printf("Error during getting device (port %u) info: %s\n", port, strerror(-retval));
        return retval;
    }

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        port_conf.txmode.offloads |=
            RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    // Configure the Ethernet device
    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0)
        return retval;

    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval != 0)
        return retval;

    // Allocate and set up 1 RX queue per Ethernet port
    for (q = 0; q < rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
                rte_eth_dev_socket_id(port), NULL, mbuf_pool);
        if (retval < 0)
            return retval;
    }

    txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;
    // Allocate and set up 1 TX queue per Ethernet port
    for (q = 0; q < tx_rings; q++) {
        retval = rte_eth_tx_queue_setup(port, q, nb_txd,
                rte_eth_dev_socket_id(port), &txconf);
        if (retval < 0)
            return retval;
    }

    // Starting Ethernet port
    retval = rte_eth_dev_start(port);
    if (retval < 0)
        return retval;

    // Display the port MAC address
    struct rte_ether_addr addr;
    retval = rte_eth_macaddr_get(port, &addr);
    if (retval != 0)
        return retval;

    printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8" %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
            port, RTE_ETHER_ADDR_BYTES(&addr));

    // Enable RX in promiscuous mode for the Ethernet device
    retval = rte_eth_promiscuous_enable(port);
    if (retval != 0)
        return retval;

    return 0;
}

struct main_thread_args {
    struct fluter_image *fluter_image;
    bool use_pingxelflut;
    struct in6_addr pingxelflut_target;
    struct rte_mempool *mbuf_pool;
    int port_id;
};

static __rte_noreturn void lcore_main(struct main_thread_args *args) {
    // Read args
    struct fluter_image *fluter_image = args->fluter_image;
    struct rte_mempool *mbuf_pool = args->mbuf_pool;
    int port_id = args->port_id;

    int width = fluter_image->width;
    int height = fluter_image->height;

    uint16_t port, nb_tx;

    uint32_t stats_loop_counter = 0;
    struct timeval last_stats_report, now;
    long elapsed_millis;

    struct rte_ether_addr dst_mac_addr = parse_mac("44:f4:77:5b:00:60");
    struct rte_ether_addr src_mac_addr = parse_mac("14:a0:f8:8b:1e:e3");
    struct in6_addr src_addr = parse_ipv6("fd69:1daa:beb8:cafe::42");
    struct in6_addr dst_net = parse_ipv6("fd69:1daa:beb8:2::");

    char src_addr_str[INET6_ADDRSTRLEN];
    char dst_addr_str[INET6_ADDRSTRLEN];
    if (!args->use_pingxelflut) {
        inet_ntop(AF_INET6, &src_addr, src_addr_str, INET6_ADDRSTRLEN);
        inet_ntop(AF_INET6, &dst_net, dst_addr_str, INET6_ADDRSTRLEN);
        printf("Using pixelflut v6 protocol to flut from %s to %s/64", src_addr_str, dst_addr_str);
    } else {
        inet_ntop(AF_INET6, &src_addr, src_addr_str, INET6_ADDRSTRLEN);
        inet_ntop(AF_INET6, &args->pingxelflut_target, dst_addr_str, INET6_ADDRSTRLEN);
        printf("Using pingxelflut protocol to flut from %s to %s", src_addr_str, dst_addr_str);
    }

    gettimeofday(&last_stats_report, NULL);

    /*
     * Check that the port is on the same NUMA node as the polling thread for best performance.
     */
    RTE_ETH_FOREACH_DEV(port)
        if (rte_eth_dev_socket_id(port) >= 0 && rte_eth_dev_socket_id(port) != (int)rte_socket_id())
            printf("WARNING, port %u is on remote NUMA node to polling thread.\n"
                "\tPerformance will not be optimal.\n", port);

    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv6_hdr *ipv6_hdr;
    struct rte_udp_hdr *udp_hdr;
    struct rte_icmp_hdr *icmp_hdr;

    uint16_t pkt_size;
    if (!args->use_pingxelflut) {
        pkt_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv6_hdr) + sizeof(struct rte_udp_hdr);
    } else {
        // 8 bytes for command, x, y, r, g and b (note we don't send a [alpha])
        pkt_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv6_hdr) + sizeof(struct rte_icmp_hdr) + 8;
    }
    // The minimum packet size sent/received through Ethernet is always 64 bytes according to Ethernet specification
    pkt_size = MAX(64, pkt_size);

    uint16_t x = 0;
    uint16_t y = 0;
    int pixel_index = 0;

    printf("\nCore %u sending %u byte packets on port %u. [Ctrl+C to quit]\n", rte_lcore_id(), pkt_size, port_id);

    struct rte_mbuf * pkt[BURST_SIZE];
    int i;
    for (;;) {
        for(i = 0; i < BURST_SIZE; i++) {
            pkt[i] = rte_pktmbuf_alloc(mbuf_pool);

            eth_hdr = rte_pktmbuf_mtod(pkt[i], struct rte_ether_hdr*);
            eth_hdr->dst_addr = dst_mac_addr;
            eth_hdr->src_addr = src_mac_addr;
            eth_hdr->ether_type = htons(RTE_ETHER_TYPE_IPV6);
            ipv6_hdr = rte_pktmbuf_mtod_offset(pkt[i], struct rte_ipv6_hdr*, sizeof(struct rte_ether_hdr));

            if (!args->use_pingxelflut) {
                ipv6_hdr->vtc_flow = htonl(6 << 28); // IP version 6
                ipv6_hdr->hop_limits = 0xff;
                ipv6_hdr->proto = 0x11; // UDP
                ipv6_hdr->payload_len = htonl(8); // TODO: Can we set this to sizeof(struct rte_udp_hdr)?

                // Set the whole source IP (128 bit - 16 bytes)
                memcpy(ipv6_hdr->src_addr, &src_addr, 16);
                // We only set the /64 network, hence the first 8 bytes
                memcpy(ipv6_hdr->dst_addr, &dst_net, 8);

                // X Coordinate
                ipv6_hdr->dst_addr[8] = x >> 8;
                ipv6_hdr->dst_addr[9] = x;

                // Y Coordinate
                ipv6_hdr->dst_addr[10] = y >> 8;
                ipv6_hdr->dst_addr[11] = y;

                // Color in rgb
                pixel_index = y * width + x;
                ipv6_hdr->dst_addr[12] = fluter_image->pixels[pixel_index] >> 0;
                ipv6_hdr->dst_addr[13] = fluter_image->pixels[pixel_index] >> 8;
                ipv6_hdr->dst_addr[14] = fluter_image->pixels[pixel_index] >> 16;
                ipv6_hdr->dst_addr[15] = fluter_image->pixels[pixel_index] >> 24;

                udp_hdr = rte_pktmbuf_mtod_offset(pkt[i], struct rte_udp_hdr*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv6_hdr));
                udp_hdr->src_port = htons(1337);
                udp_hdr->src_port = htons(1234);
                udp_hdr->dgram_len = 0;
                // This seems to be fine. Either the hardware offloading of the NIC takes care of this or I currently don't
                // notice any problems, as I'm only sending NIC -> NIC and nothing is actually checking the checksum.
                // Hopefully routers only care about the IP header and leave checking the UDP header to the receiving
                // application or kernel.
                udp_hdr->dgram_cksum = 0;
            } else {
                ipv6_hdr->vtc_flow = htonl(6 << 28); // IP version 6
                ipv6_hdr->hop_limits = 0xff;
                ipv6_hdr->proto = 58; // ICMPv6
                ipv6_hdr->payload_len = htonl(sizeof(struct rte_icmp_hdr) + 8); // 8 bytes for command, x, y, r, g and b (note we don't send a [alpha])

                memcpy(ipv6_hdr->src_addr, &src_addr, sizeof(struct in6_addr));
                memcpy(ipv6_hdr->dst_addr, &args->pingxelflut_target, sizeof(struct in6_addr));

                icmp_hdr = rte_pktmbuf_mtod_offset(pkt[i], struct rte_icmp_hdr*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv6_hdr));
                // Note: In older (or newer?) DPDK versions the constant was called RTE_ICMP6_ECHO_REQUEST
                icmp_hdr->icmp_type = RTE_IP_ICMP_ECHO_REQUEST;
                icmp_hdr->icmp_code = 0;
                // Let's see how it goes
                icmp_hdr->icmp_cksum = 0;
                // Set message type to command to set pixel
                *rte_pktmbuf_mtod_offset(pkt[i], uint8_t*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv6_hdr) + sizeof(struct rte_icmp_hdr)) = 0xcc;
                *rte_pktmbuf_mtod_offset(pkt[i], uint16_t*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv6_hdr) + sizeof(struct rte_icmp_hdr) + 1) = htons(x);
                *rte_pktmbuf_mtod_offset(pkt[i], uint16_t*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv6_hdr) + sizeof(struct rte_icmp_hdr) + 3) = htons(y);

                pixel_index = y * width + x;
                // Please note that we write 4 bytes, but we only use 3 bytes for the packet, the last byte should just
                // write into the buffer, but not get send over the network
                *rte_pktmbuf_mtod_offset(pkt[i], uint32_t*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv6_hdr) + sizeof(struct rte_icmp_hdr) + 5) = fluter_image->pixels[pixel_index];
            }

            pkt[i]->data_len = pkt_size;
            pkt[i]->pkt_len = pkt_size;

            x++;
            if (x >= width) {
                x = 0;
                y++;
                if (y >= height) {
                    y = 0;
                }
            }
        }

        do {
            nb_tx = rte_eth_tx_burst(port_id, /* queue_id */ 0, pkt, BURST_SIZE);
        } while(nb_tx == 0);


        if (unlikely(nb_tx < BURST_SIZE)) {
            printf("ERROR: Couldn't send %u packets.\n", BURST_SIZE - nb_tx);
            uint16_t buf;

            for (buf = nb_tx; buf < BURST_SIZE; buf++)
                rte_pktmbuf_free(pkt[buf]);
        }

        for(i = 0; i < BURST_SIZE; i++) {
            rte_pktmbuf_free(pkt[i]);
        }

        // I assume reading the system time is a expensive operation, so let's not do that every loop...
        stats_loop_counter++;
        if (unlikely(stats_loop_counter > 10000)) {
            stats_loop_counter = 0;

            gettimeofday(&now, NULL);
            elapsed_millis = (now.tv_sec - last_stats_report.tv_sec) * 1000.0;
            elapsed_millis += (now.tv_usec - last_stats_report.tv_usec) / 1000.0;

            if (elapsed_millis >= STATS_INTERVAL_MS) {
                last_stats_report = now;

                struct rte_eth_stats eth_stats;
                rte_eth_stats_get(port_id, &eth_stats);
                setlocale(LC_NUMERIC, "");
                printf("Total number of packets for port %u: send %'lu packets (%'lu bytes), "
                    "received %'lu packets (%'lu bytes), dropped rx %'lu, ierrors %'lu, rx_nombuf %'lu, q_ipackets %'lu\n",
                    port_id, eth_stats.opackets, eth_stats.obytes, eth_stats.ipackets, eth_stats.ibytes, eth_stats.imissed,
                    eth_stats.ierrors, eth_stats.rx_nombuf, eth_stats.q_ipackets[0]);
            }
        }
    }

    // Closing and releasing resources
    // rte_flow_flush(port_id, &error);
    rte_eth_dev_stop(port_id);
    rte_eth_dev_close(port_id);
}

int main(int argc, char *argv[]) {
    // Initializing the Environment Abstraction Layer (EAL)
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

    argc -= ret;
    argv += ret;

    // Parse command arguments (after the EAL ones)
    struct arguments arguments = {0};
    // Set defaults
    arguments.use_pingxelflut = false;
    // Parse actual arguments. I think we don't need to check the return code, as the function will error out on wrong
    // arguments(?)
    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    int err = 0;

    struct fluter_image* fluter_image;
    if((err = load_image(&fluter_image, arguments.image_file))) {
		fprintf(stderr, "Failed to load image from %s: %s\n", arguments.image_file, strerror(-err));
		return err;
	}

    struct rte_mempool *mbuf_pool;
    unsigned nb_ports;
    uint16_t port_id;

    nb_ports = rte_eth_dev_count_avail();
    printf("Detected %u ports\n", nb_ports);
    if (nb_ports != 1)
        rte_exit(EXIT_FAILURE, "Error: currently only a single port is supported, you have %d ports\n", nb_ports);

    // Allocates mempool to hold the mbufs
    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

    // Initializing all ports
    RTE_ETH_FOREACH_DEV(port_id)
        if (port_init(port_id, mbuf_pool) != 0)
            rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n",
                    port_id);

    if (rte_lcore_count() > 1)
        printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");

    // Call lcore_main on the main core only. Called on single lcore
    struct main_thread_args args;
    args.fluter_image = fluter_image;
    args.use_pingxelflut = arguments.use_pingxelflut;
    args.pingxelflut_target = arguments.pingxelflut_target;
    args.mbuf_pool = mbuf_pool;
    // FIXME: Currently this only works with a single port
    args.port_id = 0;

    lcore_main(&args);

    // Clean up the EAL
    rte_eal_cleanup();

    return 0;
}
