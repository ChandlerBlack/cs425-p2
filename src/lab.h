#ifndef LAB_H
#define LAB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_PAYLOAD_SIZE 1024
#define MAX_WINDOW_SIZE 64
#define MAX_TIMEOUTS 10

// Layer 1: In-memory Packet Representation
typedef struct {
    uint8_t type;       // 0 = DATA, 1 = ACK, 2 = FIN
    uint8_t reserved;   // Always 0
    uint16_t checksum;
    uint32_t seq;
    uint16_t length;
    uint8_t payload[MAX_PAYLOAD_SIZE]; 
} Packet;

// Layer 2: Receiver State Machine
typedef struct {
    uint32_t expected;       // Index of the next packet it wants
    bool linger_active;      // True if the FIN was received and we are lingering
    uint64_t linger_start;   // Timestamp (in ms) when the linger state began
} Receiver;

// Layer 2: Sender State Machine
typedef struct {
    uint32_t base;           // Oldest packet not yet acknowledged
    uint32_t next;           // Next packet not yet sent
    uint32_t window_size;    // Configured window size
    uint32_t timeout_ms;     // Configured timeout in ms
    
    // Timer tracking
    bool timer_running;
    uint64_t timer_start;    // Timestamp (in ms) when the timer was started
    uint8_t timeout_count;   // Tracks fruitless timeouts
    
    // Retransmission buffer
    Packet flight_window[MAX_WINDOW_SIZE]; 
} Sender;

// Layer 1: Packet Processing (Pure functions)
uint16_t compute_checksum(const uint8_t *buffer, size_t length);
size_t encode_packet(const Packet *pkt, uint8_t *buffer);
bool decode_and_validate_packet(const uint8_t *buffer, size_t length, Packet *pkt);

// Layer 2: Receiver Events
void receiver_init(Receiver *rx);
void receiver_process_packet(Receiver *rx, const Packet *in_pkt, uint64_t now_ms, 
                             bool *out_send_ack, Packet *out_ack, 
                             bool *out_write_payload);
bool receiver_is_linger_done(const Receiver *rx, uint64_t now_ms);

// Layer 2: Sender Events
void sender_init(Sender *tx, uint32_t window_size, uint32_t timeout_ms);
bool sender_handle_ack(Sender *tx, const Packet *ack, uint64_t now_ms);
uint32_t sender_handle_timeout(Sender *tx, uint64_t now_ms, Packet *out_packets);
bool sender_window_open(const Sender *tx);
void sender_enqueue_packet(Sender *tx, const Packet *pkt, uint64_t now_ms);
int64_t sender_time_until_timeout(const Sender *tx, uint64_t now_ms);
bool sender_has_failed(const Sender *tx);

#endif // LAB_H