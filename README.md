# P2

- Name: Chandler Black
- Email: chandlerblack@u.boisestate.edu
- Class: CS425

# Reliable Data Transfer (Go-Back-N)

## Design
To achieve complete testability without relying on a flaky network or wall clocks, the protocol logic is separated into three distinct layers:
1. **Packets**: Pure functions to encode, decode, and validate packets, including RFC 1071 checksum calculations. This completely decouples the in-memory structs from network byte order and padding issues.
2. **State Machines**: Pure Go-Back-N protocol logic for the Sender and Receiver. Time is passed in as a parameter (`now_ms`), and these functions return what to do next without ever touching a socket, a file handle, or calling `clock_gettime`. 
3. **I/O Integration**: The `main.c` layer that handles CLI parsing, socket creation, the `poll` event loop, and file I/O, routing bytes to and from the pure state machines.

## Results
| Window | Loss | Corrupt | Dup | Mean Time (s) | Throughput (KiB/s) |
|--------|------|---------|-----|---------------|--------------------|
| 1      | 0    | 0       | 0   | 104.675       | 9.78               |
| 16     | 0    | 0       | 0   | 6.792         | 150.77             |
| 1      | 0.05 | 0       | 0   | 130.052       | 7.87               |
| 16     | 0.05 | 0       | 0   | 25.225        | 40.59              |

*(Throughput = 1024 KiB / Mean Time)*

## Analysis

**Round Trip Time**
The 1 MiB file requires exactly 1025 packets (1024 DATA + 1 FIN). In the Window 1 / No Loss run, the sender waits exactly one round trip for every single packet. By dividing the total Mean Time (104.675s) by 1025, we get an estimated round trip time of ~102 ms. The relay explicitly adds 100 ms of this delay. The remaining 2 milliseconds come from OS loopback network overhead, CPU context switching between the three processes, and general program execution time.

**Window 16 Speedup**
The Window 16 run is ~15.4 times faster than Window 1. Instead of sitting idle for a full 100 ms round trip after every single packet, a window size of 16 allows the sender to keep the network pipe constantly full. By having 16 packets in flight simultaneously, the 100ms network delay is completely hidden, driving throughput up to 150 KiB/s.

**The Cost of 5% Loss**
While Window 16 remained faster overall than Window 1 under a 5% loss rate (25.2s vs 130.0s), its bandwidth efficiency plummeted. This perfectly highlights the core flaw of Go-Back-N: when a timeout occurs, the sender must resend the *entire* flight window. 

For Window 1, a drop meant resending exactly 1 packet (the relay logged 1074 packets sent to deliver 1024). For Window 16, a single dropped packet forced the sender to resend up to 16 packets, wasting massive bandwidth on data the receiver had already seen. To successfully deliver the 1024 packets, the Window 16 sender had to transmit 1,853 packets—nearly doubling the network traffic just to survive the 5% drop rate.
