#include <stdlib.h>
#include <stdio.h>
#include "harness/unity.h"
#include "../src/lab.h"


void setUp(void) {
  printf("Setting up tests...\n");
}

void tearDown(void) {
  printf("Tearing down tests...\n");
}

// Test the specific RFC 1071 worked example from the assignment[cite: 1]
void test_rfc1071_checksum_even_length(void) {
    // The bytes 00 01 f2 03 f4 f5 f6 f7 sum to 0xddf2, so their checksum is 0x220d[cite: 1]
    uint8_t buffer[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    uint16_t checksum = compute_checksum(buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_HEX16(0x220d, checksum);
}

// Test checksum on an odd length to verify the zero-padding logic[cite: 1]
void test_rfc1071_checksum_odd_length(void) {
    // Appending 0x00 to the end mathematically simulates the padding. 
    uint8_t buffer_odd[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7, 0xAA};
    uint8_t buffer_padded[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7, 0xAA, 0x00};
    
    uint16_t checksum_odd = compute_checksum(buffer_odd, sizeof(buffer_odd));
    uint16_t checksum_padded = compute_checksum(buffer_padded, sizeof(buffer_padded));
    
    TEST_ASSERT_EQUAL_HEX16(checksum_padded, checksum_odd);
}

void test_encode_and_decode_valid_packet(void) {
    Packet pkt_in = {
        .type = 0, // DATA[cite: 1]
        .reserved = 0,
        .seq = 42,
        .length = 5,
        .payload = {'H', 'e', 'l', 'l', 'o'}
    };
    
    uint8_t buffer[MAX_PAYLOAD_SIZE + 10];
    size_t encoded_len = encode_packet(&pkt_in, buffer);
    TEST_ASSERT_EQUAL(15, encoded_len);
    
    Packet pkt_out;
    bool valid = decode_and_validate_packet(buffer, encoded_len, &pkt_out);
    
    TEST_ASSERT_TRUE(valid);
    TEST_ASSERT_EQUAL(0, pkt_out.type);
    TEST_ASSERT_EQUAL(42, pkt_out.seq);
    TEST_ASSERT_EQUAL(5, pkt_out.length);
    TEST_ASSERT_EQUAL_MEMORY("Hello", pkt_out.payload, 5);
}

void test_validation_rejects_flipped_bit(void) {
    Packet pkt = { .type = 0, .reserved = 0, .seq = 1, .length = 0 };
    uint8_t buffer[10];
    encode_packet(&pkt, buffer);
    
    // Flip a single bit[cite: 1]
    buffer[5] ^= 0x01; 
    
    Packet decoded;
    bool valid = decode_and_validate_packet(buffer, 10, &decoded);
    TEST_ASSERT_FALSE(valid);
}

void test_validation_rejects_short_datagram(void) {
    Packet decoded;
    uint8_t buffer[9] = {0}; // Less than 10 bytes[cite: 1]
    
    bool valid = decode_and_validate_packet(buffer, 9, &decoded);
    TEST_ASSERT_FALSE(valid);
}

void test_validation_rejects_length_mismatch(void) {
    Packet pkt = { .type = 0, .reserved = 0, .seq = 1, .length = 5, .payload = {1,2,3,4,5} };
    uint8_t buffer[20];
    size_t encoded_len = encode_packet(&pkt, buffer);
    
    Packet decoded;
    // Tell the decoder the datagram size is smaller than 10 + length[cite: 1]
    bool valid = decode_and_validate_packet(buffer, encoded_len - 1, &decoded);
    TEST_ASSERT_FALSE(valid);
}

void test_validation_rejects_unknown_type(void) {
    Packet pkt = { .type = 3, .reserved = 0, .seq = 1, .length = 0 }; // Type 3 is invalid[cite: 1]
    uint8_t buffer[10];
    encode_packet(&pkt, buffer);
    
    Packet decoded;
    bool valid = decode_and_validate_packet(buffer, 10, &decoded);
    TEST_ASSERT_FALSE(valid);
}


// --- Receiver Tests ---

void test_receiver_in_order_data(void) {
    Receiver rx;
    receiver_init(&rx);
    
    Packet pkt_in = { .type = 0, .seq = 0, .length = 5, .payload = "Hello" };
    Packet ack_out;
    bool send_ack, write_payload;
    
    receiver_process_packet(&rx, &pkt_in, 1000, &send_ack, &ack_out, &write_payload);
    
    TEST_ASSERT_TRUE(send_ack);
    TEST_ASSERT_TRUE(write_payload);
    TEST_ASSERT_EQUAL(1, rx.expected);
    TEST_ASSERT_EQUAL(1, ack_out.seq);
}

void test_receiver_duplicate_packet(void) {
    Receiver rx;
    receiver_init(&rx);
    rx.expected = 1;
    
    Packet pkt_in = { .type = 0, .seq = 0, .length = 5 }; // Old packet
    Packet ack_out;
    bool send_ack, write_payload;
    
    receiver_process_packet(&rx, &pkt_in, 1000, &send_ack, &ack_out, &write_payload);
    
    TEST_ASSERT_TRUE(send_ack);
    TEST_ASSERT_FALSE(write_payload); // Discard duplicate[cite: 1]
    TEST_ASSERT_EQUAL(1, rx.expected);
    TEST_ASSERT_EQUAL(1, ack_out.seq); // Re-send expected ACK[cite: 1]
}

void test_receiver_fin_triggers_linger(void) {
    Receiver rx;
    receiver_init(&rx);
    rx.expected = 3;
    
    Packet pkt_in = { .type = 2, .seq = 3, .length = 0 }; // FIN
    Packet ack_out;
    bool send_ack, write_payload;
    
    receiver_process_packet(&rx, &pkt_in, 1000, &send_ack, &ack_out, &write_payload);
    
    TEST_ASSERT_TRUE(send_ack);
    TEST_ASSERT_EQUAL(4, ack_out.seq);
    TEST_ASSERT_TRUE(rx.linger_active);
    TEST_ASSERT_EQUAL(1000, rx.linger_start);
    
    TEST_ASSERT_FALSE(receiver_is_linger_done(&rx, 2999)); // Under 2 seconds[cite: 1]
    TEST_ASSERT_TRUE(receiver_is_linger_done(&rx, 3000));  // Exactly 2 seconds[cite: 1]
}

// --- Sender Tests ---

void test_sender_cumulative_ack_slides_window(void) {
    Sender tx;
    sender_init(&tx, 4, 250);
    
    Packet p1 = { .seq = 0 };
    Packet p2 = { .seq = 1 };
    Packet p3 = { .seq = 2 };
    
    sender_enqueue_packet(&tx, &p1, 1000);
    sender_enqueue_packet(&tx, &p2, 1010);
    sender_enqueue_packet(&tx, &p3, 1020);
    
    TEST_ASSERT_EQUAL(0, tx.base);
    TEST_ASSERT_EQUAL(3, tx.next);
    TEST_ASSERT_TRUE(tx.timer_running);
    
    Packet ack = { .type = 1, .seq = 2 }; // Acknowledges 0 and 1[cite: 1]
    bool advanced = sender_handle_ack(&tx, &ack, 1050);
    
    TEST_ASSERT_TRUE(advanced);
    TEST_ASSERT_EQUAL(2, tx.base); // Slid window by multiple packets[cite: 1]
    TEST_ASSERT_TRUE(tx.timer_running); // Packet 2 is still unacked[cite: 1]
    TEST_ASSERT_EQUAL(1050, tx.timer_start); // Timer restarted[cite: 1]
}

void test_sender_duplicate_ack_ignored(void) {
    Sender tx;
    sender_init(&tx, 4, 250);
    tx.base = 2;
    tx.next = 3;
    tx.timer_start = 1000;
    tx.timer_running = true;
    
    Packet ack = { .type = 1, .seq = 1 }; // Duplicate ACK[cite: 1]
    bool advanced = sender_handle_ack(&tx, &ack, 1050);
    
    TEST_ASSERT_FALSE(advanced);
    TEST_ASSERT_EQUAL(2, tx.base); // Window did not slide[cite: 1]
    TEST_ASSERT_EQUAL(1000, tx.timer_start); // Timer did NOT restart[cite: 1]
}

void test_sender_timeout_resends_window(void) {
    Sender tx;
    sender_init(&tx, 4, 250);
    
    Packet p1 = { .seq = 5 };
    Packet p2 = { .seq = 6 };
    
    tx.base = 5;
    sender_enqueue_packet(&tx, &p1, 1000);
    sender_enqueue_packet(&tx, &p2, 1010);
    
    Packet out_pkts[MAX_WINDOW_SIZE];
    uint32_t count = sender_handle_timeout(&tx, 1300, out_pkts);
    
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL(5, out_pkts[0].seq);
    TEST_ASSERT_EQUAL(6, out_pkts[1].seq);
    TEST_ASSERT_EQUAL(1300, tx.timer_start); // Restarted timer[cite: 1]
    TEST_ASSERT_EQUAL(1, tx.timeout_count);
}

void test_sender_gives_up_after_10_timeouts(void) {
    Sender tx;
    sender_init(&tx, 4, 250);
    tx.base = 0;
    tx.next = 1;
    
    Packet out_pkts[MAX_WINDOW_SIZE];
    for (int i = 0; i < 9; i++) {
        sender_handle_timeout(&tx, 1000 + i * 250, out_pkts);
        TEST_ASSERT_FALSE(sender_has_failed(&tx));
    }
    
    sender_handle_timeout(&tx, 3250, out_pkts);
    TEST_ASSERT_TRUE(sender_has_failed(&tx)); // Fails on 10th consecutive timeout[cite: 1]
}