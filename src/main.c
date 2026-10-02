#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <netdb.h>
#include <poll.h>
#include <time.h>
#include "lab.h"

void print_usage() {
    printf("Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss]\n");
    printf("                  [-c corrupt] [-d dup] [-p port] <relay> <file>\n");
    printf("       myapp recv -s <session> [-p port] <relay> <file>\n");
}

// Utility to get current time in milliseconds using CLOCK_MONOTONIC
uint64_t get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000) + (uint64_t)(ts.tv_nsec / 1000000);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return 0; // Clean exit path for make leak[cite: 1]
    }

    bool is_sender = false;
    if (strcmp(argv[1], "send") == 0) {
        is_sender = true;
    } else if (strcmp(argv[1], "recv") != 0) {
        print_usage();
        return 1; // Wrong command line[cite: 1]
    }

    // Default configuration[cite: 1]
    char *session = NULL;
    int window = 8;
    int timeout_ms = 250;
    float loss = 0.0, corrupt = 0.0, dup = 0.0;
    char *port = "4250";

    // Shift arguments to let getopt parse starting from argv[1]
    argv++;
    argc--;

    int opt;
    while ((opt = getopt(argc, argv, "s:w:T:l:c:d:p:")) != -1) {
        switch (opt) {
            case 's': session = optarg; break;
            case 'w': window = atoi(optarg); break;
            case 'T': timeout_ms = atoi(optarg); break;
            case 'l': loss = atof(optarg); break;
            case 'c': corrupt = atof(optarg); break;
            case 'd': dup = atof(optarg); break;
            case 'p': port = optarg; break;
            default:
                print_usage();
                return 1;
        }
    }

    if (!session || optind + 2 > argc) {
        print_usage();
        return 1;
    }

    char *relay_host = argv[optind];
    char *filename = argv[optind + 1];

    // Resolve relay address using getaddrinfo (do not assume dotted quad)[cite: 1]
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    
    if (getaddrinfo(relay_host, port, &hints, &res) != 0) {
        fprintf(stderr, "Failed to resolve relay address\n");
        return 2; // Network failure[cite: 1]
    }

    // TODO: Create socket using res->ai_family, res->ai_socktype, res->ai_protocol

    struct pollfd pfd;
    // pfd.fd = sockfd;
    pfd.events = POLLIN;

    // Main event loop template
    bool transfer_active = true;
    while (transfer_active) {
        uint64_t now = get_time_ms();
        int poll_timeout = -1; // Wait forever by default

        if (is_sender) {
            // poll_timeout = sender_time_until_timeout(&tx, now);
        }

        int ready = poll(&pfd, 1, poll_timeout);

        if (ready > 0 && (pfd.revents & POLLIN)) {
            // TODO: recvfrom(), decode_and_validate_packet(), pass to state machine
        } else if (ready == 0) {
            // TODO: Timeout occurred, call sender_handle_timeout() and sendto()
        }
        
        // TODO: Break loop on transfer complete or failure
    }

    freeaddrinfo(res);
    return 0;
}