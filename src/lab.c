#include "lab.h"
#include <string.h>

uint16_t compute_checksum(const uint8_t *buffer, size_t length) {
    uint32_t sum = 0;
    size_t i;
    
    // Process the buffer in 16-bit network byte order words
    for (i = 0; i < length - 1; i += 2) {
        sum += (buffer[i] << 8) | buffer[i + 1];
    }
    
    // If there is an odd number of bytes, pad with a zero byte for the calculation[cite: 1]
    if (i < length) {
        sum += (buffer[i] << 8);
    }
    
    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    // Return the 16-bit one's complement[cite: 1]
    return (uint16_t)(~sum);
}

size_t encode_packet(const Packet *pkt, uint8_t *buffer) {
    buffer[0] = pkt->type;
    buffer[1] = pkt->reserved; // Always 0[cite: 1]
    
    // Set checksum to 0 temporarily for calculation[cite: 1]
    buffer[2] = 0;
    buffer[3] = 0;
    
    // Pack 32-bit sequence number (Network byte order / Big-Endian)[cite: 1]
    buffer[4] = (pkt->seq >> 24) & 0xFF;
    buffer[5] = (pkt->seq >> 16) & 0xFF;
    buffer[6] = (pkt->seq >> 8)  & 0xFF;
    buffer[7] = pkt->seq         & 0xFF;
    
    // Pack 16-bit length[cite: 1]
    buffer[8] = (pkt->length >> 8) & 0xFF;
    buffer[9] = pkt->length        & 0xFF;
    
    // Copy payload if any
    if (pkt->length > 0) {
        memcpy(&buffer[10], pkt->payload, pkt->length);
    }
    
    size_t total_length = 10 + pkt->length;
    
    // Calculate final checksum and insert it into the header[cite: 1]
    uint16_t checksum = compute_checksum(buffer, total_length);
    buffer[2] = (checksum >> 8) & 0xFF;
    buffer[3] = checksum        & 0xFF;
    
    return total_length;
}

bool decode_and_validate_packet(const uint8_t *buffer, size_t length, Packet *pkt) {
    // 1. Datagram must be at least 10 bytes[cite: 1]
    if (length < 10) return false;
    
    // Extract length field safely[cite: 1]
    uint16_t payload_len = (buffer[8] << 8) | buffer[9];
    
    // 2. Size validation: length field must match remaining buffer, at most 1024[cite: 1]
    if (payload_len > MAX_PAYLOAD_SIZE) return false;
    if (10 + payload_len != length) return false;
    
    // 3. Type and reserved validation[cite: 1]
    if (buffer[0] > 2) return false; // Only 0, 1, or 2 are valid
    if (buffer[1] != 0) return false;
    
    // 4. Checksum verification
    // An undamaged packet should sum to 0xFFFF before the bitwise NOT, 
    // which makes compute_checksum return 0x0000[cite: 1]
    if (compute_checksum(buffer, length) != 0x0000) return false;
    
    // If we passed all checks, populate the Packet struct
    pkt->type = buffer[0];
    pkt->reserved = buffer[1];
    pkt->checksum = (buffer[2] << 8) | buffer[3];
    pkt->seq = (buffer[4] << 24) | (buffer[5] << 16) | (buffer[6] << 8) | buffer[7];
    pkt->length = payload_len;
    
    if (pkt->length > 0) {
        memcpy(pkt->payload, &buffer[10], pkt->length);
    }
    
    return true;
}

void receiver_init(Receiver *rx) {
    rx->expected = 0;
    rx->linger_active = false;
    rx->linger_start = 0;
}

void receiver_process_packet(Receiver *rx, const Packet *in_pkt, uint64_t now_ms, 
                             bool *out_send_ack, Packet *out_ack, bool *out_write_payload) {
    *out_send_ack = false;
    *out_write_payload = false;

    // Default ACK response is for the expected packet[cite: 1]
    out_ack->type = 1; // ACK
    out_ack->reserved = 0;
    out_ack->seq = rx->expected;
    out_ack->length = 0;

    if (in_pkt->seq == rx->expected) {
        if (in_pkt->type == 0) { // DATA
            *out_write_payload = true;
            rx->expected++;
            out_ack->seq = rx->expected; // Update ACK to the new expected seq[cite: 1]
            *out_send_ack = true;
        } else if (in_pkt->type == 2) { // FIN
            rx->expected++;
            out_ack->seq = rx->expected;
            *out_send_ack = true;
            
            // Trigger 2-second linger state on FIN[cite: 1]
            if (!rx->linger_active) {
                rx->linger_active = true;
                rx->linger_start = now_ms;
            }
        }
    } else {
        // Out of order, duplicate, or repeated FIN: discard payload, re-send expected ACK[cite: 1]
        *out_send_ack = true;
    }
}

bool receiver_is_linger_done(const Receiver *rx, uint64_t now_ms) {
    if (!rx->linger_active) return false;
    // Linger for 2 seconds (2000 ms)[cite: 1]
    return (now_ms - rx->linger_start) >= 2000;
}

void sender_init(Sender *tx, uint32_t window_size, uint32_t timeout_ms) {
    tx->base = 0;
    tx->next = 0;
    tx->window_size = window_size;
    tx->timeout_ms = timeout_ms;
    tx->timer_running = false;
    tx->timer_start = 0;
    tx->timeout_count = 0;
}

bool sender_handle_ack(Sender *tx, const Packet *ack, uint64_t now_ms) {
    // If seq > base, this is a cumulative ACK that slides the window[cite: 1]
    if (ack->seq > tx->base) {
        tx->base = ack->seq;
        tx->timeout_count = 0; // Reset fruitless timeout counter[cite: 1]
        
        if (tx->base == tx->next) {
            // Everything is acknowledged, stop timer[cite: 1]
            tx->timer_running = false;
        } else {
            // Unacknowledged packets remain, restart timer[cite: 1]
            tx->timer_running = true;
            tx->timer_start = now_ms;
        }
        return true;
    }
    // Duplicate ACK (seq <= base): ignore it[cite: 1]
    return false;
}

uint32_t sender_handle_timeout(Sender *tx, uint64_t now_ms, Packet *out_packets) {
    tx->timeout_count++;
    
    // Restart timer on timeout[cite: 1]
    tx->timer_start = now_ms;
    
    uint32_t count = 0;
    // Go-Back-N: resend every packet from base to next - 1[cite: 1]
    for (uint32_t i = tx->base; i < tx->next; i++) {
        // flight_window is a ring buffer to handle wrapping, bounded by MAX_WINDOW_SIZE
        out_packets[count++] = tx->flight_window[i % MAX_WINDOW_SIZE];
    }
    return count;
}

bool sender_window_open(const Sender *tx) {
    return tx->next < (tx->base + tx->window_size); //[cite: 1]
}

void sender_enqueue_packet(Sender *tx, const Packet *pkt, uint64_t now_ms) {
    tx->flight_window[tx->next % MAX_WINDOW_SIZE] = *pkt;
    tx->next++;
    
    // Start timer if it was not running[cite: 1]
    if (!tx->timer_running) {
        tx->timer_running = true;
        tx->timer_start = now_ms;
    }
}

int64_t sender_time_until_timeout(const Sender *tx, uint64_t now_ms) {
    if (!tx->timer_running) return -1;
    
    int64_t elapsed = now_ms - tx->timer_start;
    int64_t remaining = tx->timeout_ms - elapsed;
    return (remaining > 0) ? remaining : 0;
}

bool sender_has_failed(const Sender *tx) {
    return tx->timeout_count >= MAX_TIMEOUTS; // 10 fruitless timeouts[cite: 1]
}