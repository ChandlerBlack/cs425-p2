#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "harness/unity.h"
#include "../src/lab.h"


void setUp(void) {
  printf("Setting up tests...\n");
}

void tearDown(void) {
  printf("Tearing down tests...\n");
}

// Test the specific RFC 1071 worked example from the assignment
void test_rfc1071_checksum_even_length(void) {
    // The bytes 00 01 f2 03 f4 f5 f6 f7 sum to 0xddf2, so their checksum is 0x220d
    uint8_t buffer[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    uint16_t checksum = compute_checksum(buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_HEX16(0x220d, checksum);
}

// Test checksum on an odd length to verify the zero-padding logic
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
        .type = 0, // DATA
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
    
    // Flip a single bit
    buffer[5] ^= 0x01; 
    
    Packet decoded;
    bool valid = decode_and_validate_packet(buffer, 10, &decoded);
    TEST_ASSERT_FALSE(valid);
}

void test_validation_rejects_short_datagram(void) {
    Packet decoded;
    uint8_t buffer[9] = {0}; // Less than 10 bytes
    
    bool valid = decode_and_validate_packet(buffer, 9, &decoded);
    TEST_ASSERT_FALSE(valid);
}

void test_validation_rejects_length_mismatch(void) {
    Packet pkt = { .type = 0, .reserved = 0, .seq = 1, .length = 5, .payload = {1,2,3,4,5} };
    uint8_t buffer[20];
    size_t encoded_len = encode_packet(&pkt, buffer);
    
    Packet decoded;
    // Tell the decoder the datagram size is smaller than 10 + length
    bool valid = decode_and_validate_packet(buffer, encoded_len - 1, &decoded);
    TEST_ASSERT_FALSE(valid);
}

void test_validation_rejects_unknown_type(void) {
    Packet pkt = { .type = 3, .reserved = 0, .seq = 1, .length = 0 }; // Type 3 is invalid
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
    TEST_ASSERT_FALSE(write_payload); // Discard duplicate
    TEST_ASSERT_EQUAL(1, rx.expected);
    TEST_ASSERT_EQUAL(1, ack_out.seq); // Re-send expected ACK
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
    
    TEST_ASSERT_FALSE(receiver_is_linger_done(&rx, 2999)); // Under 2 seconds
    TEST_ASSERT_TRUE(receiver_is_linger_done(&rx, 3000));  // Exactly 2 seconds
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
    
    Packet ack = { .type = 1, .seq = 2 }; // Acknowledges 0 and 1
    bool advanced = sender_handle_ack(&tx, &ack, 1050);
    
    TEST_ASSERT_TRUE(advanced);
    TEST_ASSERT_EQUAL(2, tx.base); // Slid window by multiple packets
    TEST_ASSERT_TRUE(tx.timer_running); // Packet 2 is still unacked
    TEST_ASSERT_EQUAL(1050, tx.timer_start); // Timer restarted
}

void test_sender_duplicate_ack_ignored(void) {
    Sender tx;
    sender_init(&tx, 4, 250);
    tx.base = 2;
    tx.next = 3;
    tx.timer_start = 1000;
    tx.timer_running = true;
    
    Packet ack = { .type = 1, .seq = 1 }; // Duplicate ACK
    bool advanced = sender_handle_ack(&tx, &ack, 1050);
    
    TEST_ASSERT_FALSE(advanced);
    TEST_ASSERT_EQUAL(2, tx.base); // Window did not slide
    TEST_ASSERT_EQUAL(1000, tx.timer_start); // Timer did NOT restart
}

void test_sender_timeout_resends_window(void) {
    Sender tx;
    sender_init(&tx, 4, 250);
    
    Packet p1 = { .seq = 5 };
    Packet p2 = { .seq = 6 };
    
    tx.base = 5;
    tx.next = 5;
    sender_enqueue_packet(&tx, &p1, 1000);
    sender_enqueue_packet(&tx, &p2, 1010);
    
    Packet out_pkts[MAX_WINDOW_SIZE];
    uint32_t count = sender_handle_timeout(&tx, 1300, out_pkts);
    
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL(5, out_pkts[0].seq);
    TEST_ASSERT_EQUAL(6, out_pkts[1].seq);
    TEST_ASSERT_EQUAL(1300, tx.timer_start); // Restarted timer
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
    TEST_ASSERT_TRUE(sender_has_failed(&tx)); // Fails on 10th consecutive timeout
}

#include <stdlib.h>

void test_lossy_end_to_end_transfer(void) {
    Sender tx;
    Receiver rx;
    sender_init(&tx, 8, 250);
    receiver_init(&rx);
    
    srand(42); // Fixed seed for reproducibility
    uint64_t now = 1000;
    
    // Simulate a short file (e.g., 3 DATA packets + 1 FIN)
    Packet source_packets[4];
    for (int i = 0; i < 3; i++) {
        source_packets[i].type = 0;
        source_packets[i].seq = i;
        source_packets[i].length = 5;
        memcpy(source_packets[i].payload, "12345", 5);
    }
    source_packets[3].type = 2; // FIN
    source_packets[3].seq = 3;
    source_packets[3].length = 0;
    
    int packets_sent = 0;
    bool transfer_active = true;
    
    // In-memory channel queues
    Packet network_to_rx[100];
    int to_rx_count = 0;
    
    Packet network_to_tx[100];
    int to_tx_count = 0;

    // Simulate event loop
    while (transfer_active && now < 20000) {
        now += 10; // Advance clock
        
        // Sender: Push new packets if window open
        while (packets_sent < 4 && sender_window_open(&tx)) {
            sender_enqueue_packet(&tx, &source_packets[packets_sent], now);
            // 20% loss/corruption simulation
            if (rand() % 100 >= 20) {
                network_to_rx[to_rx_count++] = source_packets[packets_sent];
            }
            packets_sent++;
        }
        
        // Receiver process network
        for (int i = 0; i < to_rx_count; i++) {
            bool send_ack, write_payload;
            Packet ack_out;
            receiver_process_packet(&rx, &network_to_rx[i], now, &send_ack, &ack_out, &write_payload);
            
            if (send_ack) {
                // 20% loss/corruption on ACKs
                if (rand() % 100 >= 20) {
                    network_to_tx[to_tx_count++] = ack_out;
                }
            }
        }
        to_rx_count = 0; // Clear queue
        
        // Sender process ACKs
        for (int i = 0; i < to_tx_count; i++) {
            sender_handle_ack(&tx, &network_to_tx[i], now);
            if (packets_sent == 4 && !tx.timer_running) {
                transfer_active = false; // FIN acked
            }
        }
        to_tx_count = 0; // Clear queue
        
        // Sender Handle Timeout
        if (sender_time_until_timeout(&tx, now) == 0) {
            Packet retransmits[MAX_WINDOW_SIZE];
            uint32_t count = sender_handle_timeout(&tx, now, retransmits);
            for (uint32_t i = 0; i < count; i++) {
                if (rand() % 100 >= 20) { // 20% loss on retransmits
                    network_to_rx[to_rx_count++] = retransmits[i];
                }
            }
        }
    }
    
    // Assert transfer succeeded despite the 20% drop rate
    TEST_ASSERT_FALSE(transfer_active);
    TEST_ASSERT_EQUAL(4, rx.expected); // 3 DATA + 1 FIN
}


void test_sender_time_until_timeout_not_running(void) {
    Sender tx;
    sender_init(&tx, 4, 250);
    TEST_ASSERT_EQUAL_INT64(-1, sender_time_until_timeout(&tx, 1000));
}

void test_sender_time_until_timeout_remaining(void) {
    Sender tx;
    sender_init(&tx, 4, 250);
    Packet p1 = { .seq = 0 };
    sender_enqueue_packet(&tx, &p1, 1000);
    
    // 100ms has elapsed on a 250ms timer, 150ms should be remaining
    TEST_ASSERT_EQUAL_INT64(150, sender_time_until_timeout(&tx, 1100));
}

void test_validation_rejects_oversized_payload(void) {
    uint8_t buffer[15] = {0};
    // Falsely claim a 2000-byte payload in the length header
    buffer[8] = (2000 >> 8) & 0xFF; 
    buffer[9] = 2000 & 0xFF;
    
    Packet decoded;
    TEST_ASSERT_FALSE(decode_and_validate_packet(buffer, 15, &decoded));
}

void test_validation_rejects_nonzero_reserved(void) {
    Packet pkt = { .type = 0, .reserved = 1, .seq = 1, .length = 0 };
    uint8_t buffer[10];
    encode_packet(&pkt, buffer);
    
    Packet decoded;
    TEST_ASSERT_FALSE(decode_and_validate_packet(buffer, 10, &decoded));
}



int main(void) {
    UNITY_BEGIN();
    
    RUN_TEST(test_rfc1071_checksum_even_length);
    RUN_TEST(test_rfc1071_checksum_odd_length);
    RUN_TEST(test_encode_and_decode_valid_packet);
    RUN_TEST(test_validation_rejects_flipped_bit);
    RUN_TEST(test_validation_rejects_short_datagram);
    RUN_TEST(test_validation_rejects_length_mismatch);
    RUN_TEST(test_validation_rejects_unknown_type);
    
    RUN_TEST(test_receiver_in_order_data);
    RUN_TEST(test_receiver_duplicate_packet);
    RUN_TEST(test_receiver_fin_triggers_linger);
    
    RUN_TEST(test_sender_cumulative_ack_slides_window);
    RUN_TEST(test_sender_duplicate_ack_ignored);
    RUN_TEST(test_sender_timeout_resends_window);
    RUN_TEST(test_sender_gives_up_after_10_timeouts);
    
    RUN_TEST(test_lossy_end_to_end_transfer);

    RUN_TEST(test_sender_time_until_timeout_not_running);
    RUN_TEST(test_sender_time_until_timeout_remaining);
    RUN_TEST(test_validation_rejects_oversized_payload);
    RUN_TEST(test_validation_rejects_nonzero_reserved);
    
    return UNITY_END();
}