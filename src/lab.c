#include "lab.h"
#include <string.h>

uint16_t compute_checksum(const uint8_t *buffer, size_t length) {
    uint32_t sum = 0;
    size_t i;
    
    for (i = 0; i < length - 1; i += 2) {
        sum += (buffer[i] << 8) | buffer[i + 1];
    }
    
    if (i < length) {
        sum += (buffer[i] << 8);
    }
    
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return (uint16_t)(~sum);
}

size_t encode_packet(const Packet *pkt, uint8_t *buffer) {
    buffer[0] = pkt->type;
    buffer[1] = pkt->reserved; 
    
    buffer[2] = 0;
    buffer[3] = 0;
    
    buffer[4] = (pkt->seq >> 24) & 0xFF;
    buffer[5] = (pkt->seq >> 16) & 0xFF;
    buffer[6] = (pkt->seq >> 8)  & 0xFF;
    buffer[7] = pkt->seq         & 0xFF;
    
    buffer[8] = (pkt->length >> 8) & 0xFF;
    buffer[9] = pkt->length        & 0xFF;
    
    if (pkt->length > 0) {
        memcpy(&buffer[10], pkt->payload, pkt->length);
    }
    
    size_t total_length = 10 + pkt->length;
    
    uint16_t checksum = compute_checksum(buffer, total_length);
    buffer[2] = (checksum >> 8) & 0xFF;
    buffer[3] = checksum        & 0xFF;
    
    return total_length;
}

bool decode_and_validate_packet(const uint8_t *buffer, size_t length, Packet *pkt) {
    if (length < 10) return false;
    
    uint16_t payload_len = (buffer[8] << 8) | buffer[9];
    
    if (payload_len > MAX_PAYLOAD_SIZE) return false;
    if (10 + payload_len != length) return false;
    
    if (buffer[0] > 2) return false; 
    if (buffer[1] != 0) return false;
    
    if (compute_checksum(buffer, length) != 0x0000) return false;
    
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

    out_ack->type = 1; 
    out_ack->reserved = 0;
    out_ack->seq = rx->expected;
    out_ack->length = 0;

    if (in_pkt->seq == rx->expected) {
        if (in_pkt->type == 0) { 
            *out_write_payload = true;
            rx->expected++;
            out_ack->seq = rx->expected; 
            *out_send_ack = true;
        } else if (in_pkt->type == 2) { 
            rx->expected++;
            out_ack->seq = rx->expected;
            *out_send_ack = true;
            
            if (!rx->linger_active) {
                rx->linger_active = true;
                rx->linger_start = now_ms;
            }
        }
    } else {
        *out_send_ack = true;
    }
}

bool receiver_is_linger_done(const Receiver *rx, uint64_t now_ms) {
    if (!rx->linger_active) return false;
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
    if (ack->seq > tx->base) {
        tx->base = ack->seq;
        tx->timeout_count = 0; 
        
        if (tx->base == tx->next) {
            tx->timer_running = false;
        } else {
            tx->timer_running = true;
            tx->timer_start = now_ms;
        }
        return true;
    }
    return false;
}

uint32_t sender_handle_timeout(Sender *tx, uint64_t now_ms, Packet *out_packets) {
    tx->timeout_count++;
    
    tx->timer_start = now_ms;
    
    uint32_t count = 0;
    for (uint32_t i = tx->base; i < tx->next; i++) {
        out_packets[count++] = tx->flight_window[i % MAX_WINDOW_SIZE];
    }
    return count;
}

bool sender_window_open(const Sender *tx) {
    return tx->next < (tx->base + tx->window_size);
}

void sender_enqueue_packet(Sender *tx, const Packet *pkt, uint64_t now_ms) {
    tx->flight_window[tx->next % MAX_WINDOW_SIZE] = *pkt;
    tx->next++;
    
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
    return tx->timeout_count >= MAX_TIMEOUTS; 
}