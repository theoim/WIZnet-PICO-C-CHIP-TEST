# pico_rmii_loopback

Dual-stack TCP loopback on the **W55RP20_EVB_PICO** board.  
Both the W55RP20 TOE interface and the PIO MII + YT8111 + lwIP interface run
independent TCP echo servers simultaneously, on separate IP addresses and
physical ports.

## Why this example exists

The regular WIZnet loopback runs entirely inside the TOE hardware — the
software MAC layer, PIO state machines, and lwIP stack are never exercised.

`pico_rmii_loopback` brings up **both stacks at the same time** and echoes TCP
traffic on each.  It answers the question: *does the PIO MII implementation
and lwIP stack work correctly alongside the TOE, on real hardware?*

## Use cases

- Verify PIO MII wiring and YT8111 PHY bring-up before building a gateway
- Validate lwIP port and multicore scheduling under a running TOE workload
- Signal-integrity test on the MII bus (GPIO 2–15) while the SPI bus is active
- Regression baseline: if both ports echo at wire speed, the board is healthy

## Architecture

```
       Standard RJ45                10BASE-T1L SPE
      (W55RP20 TOE)                  (YT8111 PHY)
      192.168.11.13                 192.168.11.12
      TCP port 5000                 TCP port 5000
            |                             |
            | HW TCP/IP                   | MII -- GPIO 4-15
            |                             |
  +---------+-----------------------------+---------+
  |                 W55RP20_EVB_PICO                |
  |                                                 |
  |   +---------------------+  +----------------+  |
  |   |    W55RP20 TOE      |  |     RP2040     |  |
  |   |  Hardware TCP/IP    |  |                |  |
  |   |  loopback_tcps()    |  |  Core 1        |  |
  |   |                     |  |  PIO MII(pio0) |  |
  |   |  Core 0             |  |  + lwIP        |  |
  |   |  (owns TOE SPI)     |  |  TCP echo      |  |
  |   +---------------------+  +----------------+  |
  |                                                 |
  +-------------------------------------------------+

  Both interfaces echo independently.
  Connect to either IP on port 5000 -- data sent is returned unchanged.
```

## Pin assignment

### MII interface — pio0

| GPIO | Signal  | Direction              |
|------|---------|------------------------|
| 2    | MDIO    | bidirectional          |
| 3    | MDC     | RP2040 → PHY           |
| 4    | TXD0    | RP2040 → PHY           |
| 5    | TXD1    | RP2040 → PHY           |
| 6    | TXD2    | RP2040 → PHY           |
| 7    | TXD3    | RP2040 → PHY           |
| 8    | TX-EN   | RP2040 → PHY           |
| 9    | TX-CLK  | PHY → RP2040 (2.5 MHz) |
| 10   | RXD0    | PHY → RP2040           |
| 11   | RXD1    | PHY → RP2040           |
| 12   | RXD2    | PHY → RP2040           |
| 13   | RXD3    | PHY → RP2040           |
| 14   | RX-DV   | PHY → RP2040           |
| 15   | RX-CLK  | PHY → RP2040 (2.5 MHz) |

### W55RP20 TOE SPI — pio1

| GPIO | Signal |
|------|--------|
| 20   | CS     |
| 21   | SCK    |
| 22   | MISO   |
| 23   | MOSI   |
| 24   | INT    |
| 25   | RST    |

## Network configuration

| Interface        | IP              | TCP port |
|------------------|-----------------|----------|
| W55RP20 TOE      | 192.168.11.13   | 5000     |
| PIO MII + lwIP   | 192.168.11.12   | 5000     |

## How to test

1. Flash the board and open a serial monitor (115200 baud).
2. Connect both Ethernet ports to the same LAN as the PC.
3. Open two TCP connections — one to each IP on port 5000.
4. Send any data; each connection independently echoes it back.

```bash
# Terminal 1 -- TOE interface
nc 192.168.11.13 5000

# Terminal 2 -- MII/lwIP interface
nc 192.168.11.12 5000
```

## Multicore split

| Core   | Responsibility                                              |
|--------|-------------------------------------------------------------|
| Core 0 | W55RP20 TOE init → YT8111 init → `loopback_tcps()` loop   |
| Core 1 | MII Ethernet poll (PIO + DMA + lwIP sys_check_timeouts)     |
