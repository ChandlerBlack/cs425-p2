#ifndef LAB_H
#define LAB_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_PAYLOAD_SIZE 1024
#define MAX_WINDOW_SIZE 64
#define MAX_TIMEOUTS 10

// Layer 1: In-memory Packet Representation
// Do not memcpy this directly to a socket. Use encode/decode functions to handle byte order and padding
typedef struct {
    uint8_t type;       // 0 = DATA, 1 = ACK, 2 = FIN
    uint8_t reserved;   // Always 0
    uint16_t checksum;
    uint32_t seq;
    uint16_t length;
    uint8_t payload[MAX_PAYLOAD_SIZE]; 
} Packet;

// Layer 1: Packet Processing (Pure functions, no sockets)

// Computes the RFC 1071 Internet checksum over a raw buffer
uint16_t compute_checksum(const uint8_t *buffer, size_t length);

// Encodes a Packet struct into a network-ready byte buffer.
// Returns the total number of bytes written (10 + payload length).
size_t encode_packet(const Packet *pkt, uint8_t *buffer);

// Decodes a raw byte buffer into a Packet struct and validates it.
// Returns true if the packet passes all validation rules, false otherwise.
bool decode_and_validate_packet(const uint8_t *buffer, size_t length, Packet *pkt);

// Layer 2: Receiver State Machine
typedef struct {
    uint32_t expected;       // Index of the next packet it wants
    bool linger_active;      // True if the FIN was received and we are in the 2-second linger state
    uint64_t linger_start;   // Timestamp (in ms) when the linger state began
} Receiver;

// Layer 2: Sender State Machine
typedef struct {
    uint32_t base;           // Oldest packet not yet acknowledged
    uint32_t next;           // Next packet not yet sent
    uint32_t window_size;    // Configured window size (1 to 64)
    uint32_t timeout_ms;     // Configured timeout in milliseconds
    
    // Timer tracking
    bool timer_running;
    uint64_t timer_start;    // Timestamp (in ms) when the timer was started
    uint8_t timeout_count;   // Tracks fruitless timeouts (gives up at 10)
    
    // Retransmission buffer
    // Holds copies of packets currently in flight (from base to next - 1)
    Packet flight_window[MAX_WINDOW_SIZE]; 
} Sender;

// Layer 2: State Machine Initialization
void receiver_init(Receiver *rx);
void sender_init(Sender *tx, uint32_t window_size, uint32_t timeout_ms);

// Layer 2: Receiver Events
// Processes an incoming packet. Sets out_send_ack to true if an ACK should be sent,
// and out_write_payload to true if the payload should be written to disk.
void receiver_process_packet(Receiver *rx, const Packet *in_pkt, uint64_t now_ms, 
                             bool *out_send_ack, Packet *out_ack, 
                             bool *out_write_payload);

// Checks if the 2-second linger period has finished.
bool receiver_is_linger_done(const Receiver *rx, uint64_t now_ms);

// Layer 2: Sender Events
// Processes an incoming ACK. Returns true if it was cumulative and advanced the window[cite: 1].
bool sender_handle_ack(Sender *tx, const Packet *ack, uint64_t now_ms);

// Handles a timeout event. Populates out_packets with the window to retransmit.
// Returns the number of packets that need to be resent[cite: 1].
uint32_t sender_handle_timeout(Sender *tx, uint64_t now_ms, Packet *out_packets);

// Returns true if the window has room for new packets (next < base + window)[cite: 1].
bool sender_window_open(const Sender *tx);

// Enqueues a new packet into the flight window and starts the timer if needed[cite: 1].
void sender_enqueue_packet(Sender *tx, const Packet *pkt, uint64_t now_ms);

// Calculates milliseconds remaining on the timer. Returns -1 if stopped.
int64_t sender_time_until_timeout(const Sender *tx, uint64_t now_ms);

// Returns true if 10 consecutive fruitless timeouts have occurred[cite: 1].
bool sender_has_failed(const Sender *tx);

#endif // LAB_H