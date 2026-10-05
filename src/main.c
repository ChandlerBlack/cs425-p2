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

// Phase 5: Relay Registration
void register_with_relay(int sockfd, struct addrinfo *res, bool is_sender, 
                         const char *session, float loss, float corrupt, float dup) {
    char hello_msg[256];
    if (is_sender) {
        // Sender format: HELLO <session> send <loss> <corrupt> <dup>[cite: 1]
        snprintf(hello_msg, sizeof(hello_msg), "HELLO %s send %f %f %f", 
                 session, loss, corrupt, dup);
    } else {
        // Receiver format: HELLO <session> recv[cite: 1]
        snprintf(hello_msg, sizeof(hello_msg), "HELLO %s recv", session);
    }

    struct pollfd pfd = { .fd = sockfd, .events = POLLIN };
    
    // Attempt registration up to 5 times[cite: 1]
    for (int attempt = 0; attempt < 5; attempt++) {
        sendto(sockfd, hello_msg, strlen(hello_msg), 0, res->ai_addr, res->ai_addrlen);
        
        // Wait up to 1 second for a reply[cite: 1]
        int ready = poll(&pfd, 1, 1000); 
        if (ready > 0) {
            char reply[256];
            ssize_t n = recvfrom(sockfd, reply, sizeof(reply) - 1, 0, NULL, NULL);
            if (n > 0) {
                reply[n] = '\0'; // Null-terminate for string comparison
                
                if (strncmp(reply, "OK", 2) == 0) {
                    return; // Successfully registered[cite: 1]
                } else if (strncmp(reply, "ERR", 3) == 0) {
                    fprintf(stderr, "%s\n", reply);
                    exit(2); // Relay refused[cite: 1]
                }
            }
        }
    }
    
    // If we exit the loop, the relay never replied
    fprintf(stderr, "ERR relay did not respond after 5 attempts\n");
    exit(2); // Network failure[cite: 1]
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

    // Create the single socket used for the whole run
    int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd < 0) {
        fprintf(stderr, "Failed to create socket\n");
        exit(2); 
    }

    // Register with the relay
    register_with_relay(sockfd, res, is_sender, session, loss, corrupt, dup);

    // Open the file
    FILE *file = NULL;
    if (is_sender) {
        file = fopen(filename, "rb");
        if (!file) {
            fprintf(stderr, "Failed to open input file\n");
            exit(1);
        }
    } else {
        file = fopen(filename, "wb");
        if (!file) {
            fprintf(stderr, "Failed to open output file\n");
            exit(1);
        }
    }

    // Initialize state machines[cite: 1]
    Sender tx;
    Receiver rx;
    if (is_sender) {
        sender_init(&tx, window, timeout_ms);
    } else {
        receiver_init(&rx);
    }


    struct pollfd pfd;
    // pfd.fd = sockfd;
    pfd.events = POLLIN;

    bool transfer_active = true;
    uint64_t receiver_last_recv = get_time_ms();
    bool file_eof = false;
    uint32_t seq_num = 0;
    uint8_t file_buf[MAX_PAYLOAD_SIZE];

    while (transfer_active) {
        uint64_t now = get_time_ms();

        // Receiver: Give up if nothing valid arrives for 30 seconds
        if (!is_sender && (now - receiver_last_recv) > 30000) {
            fprintf(stderr, "Receiver timed out after 30 seconds of silence\n");
            exit(2);
        }

        // Receiver: Exit cleanly if the 2-second linger is done
        if (!is_sender && receiver_is_linger_done(&rx, now)) {
            break; 
        }

        // Sender: While window is open and file has data, send new packets[cite: 1]
        if (is_sender && !file_eof && sender_window_open(&tx)) {
            size_t bytes_read = fread(file_buf, 1, MAX_PAYLOAD_SIZE, file);
            Packet p = {0};
            p.seq = seq_num++;
            
            if (bytes_read > 0) {
                p.type = 0; // DATA
                p.length = bytes_read;
                memcpy(p.payload, file_buf, bytes_read);
            } else if (feof(file)) {
                p.type = 2; // FIN
                p.length = 0;
                file_eof = true;
            }
            
            sender_enqueue_packet(&tx, &p, now);
            
            uint8_t wire_buf[MAX_PAYLOAD_SIZE + 10];
            size_t wire_len = encode_packet(&p, wire_buf);
            sendto(sockfd, wire_buf, wire_len, 0, res->ai_addr, res->ai_addrlen);
            continue; // Skip poll to immediately fill the rest of the open window
        }

        // Determine poll timeout
        int poll_timeout = -1; 
        if (is_sender) {
            poll_timeout = sender_time_until_timeout(&tx, now);
        } else if (rx.linger_active) {
            poll_timeout = 2000 - (now - rx.linger_start);
            if (poll_timeout < 0) poll_timeout = 0;
        } else {
            poll_timeout = 30000 - (now - receiver_last_recv); 
        }

        int ready = poll(&pfd, 1, poll_timeout);

        if (ready > 0 && (pfd.revents & POLLIN)) {
            uint8_t recv_buf[2048];
            // Check length against size actually returned to prevent buffer overflows[cite: 1]
            ssize_t n = recvfrom(sockfd, recv_buf, sizeof(recv_buf), 0, NULL, NULL);
            if (n < 0) continue;

            Packet in_pkt;
            // Silent discard if validation fails[cite: 1]
            if (!decode_and_validate_packet(recv_buf, n, &in_pkt)) continue;

            if (is_sender) {
                if (sender_handle_ack(&tx, &in_pkt, now)) {
                    // If the file is done and the timer stopped (all ACKs received), we are finished[cite: 1]
                    if (file_eof && !tx.timer_running) transfer_active = false;
                }
            } else {
                receiver_last_recv = now;
                bool send_ack, write_payload;
                Packet out_ack;
                
                receiver_process_packet(&rx, &in_pkt, now, &send_ack, &out_ack, &write_payload);
                
                if (write_payload && in_pkt.length > 0) {
                    fwrite(in_pkt.payload, 1, in_pkt.length, file);
                }
                
                if (send_ack) {
                    uint8_t wire_buf[10];
                    size_t wire_len = encode_packet(&out_ack, wire_buf);
                    sendto(sockfd, wire_buf, wire_len, 0, res->ai_addr, res->ai_addrlen);
                }
            }
        } else if (ready == 0 && is_sender) {
            // Timeout event[cite: 1]
            Packet retransmit[MAX_WINDOW_SIZE];
            uint32_t count = sender_handle_timeout(&tx, now, retransmit);
            
            // Give up after 10 fruitless timeouts[cite: 1]
            if (sender_has_failed(&tx)) {
                fprintf(stderr, "Sender gave up after 10 timeouts\n");
                exit(2); 
            }
            
            // Resend window[cite: 1]
            for (uint32_t i = 0; i < count; i++) {
                uint8_t wire_buf[MAX_PAYLOAD_SIZE + 10];
                size_t wire_len = encode_packet(&retransmit[i], wire_buf);
                sendto(sockfd, wire_buf, wire_len, 0, res->ai_addr, res->ai_addrlen);
            }
        }
    }

    fclose(file);
    freeaddrinfo(res);
    return 0; // Exit 0 on successful transfer[cite: 1]
}