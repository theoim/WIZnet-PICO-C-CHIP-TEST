# pico_rmii_gateway

L4 TCP gateway on the **W55RP20_EVB_PICO** board.  
The W55RP20 TOE (standard RJ45 port) accepts incoming TCP connections and
bridges them transparently to a remote server reachable via the PIO MII
(10BASE-T1L SPE port) + lwIP stack.  Data flows in both directions through
lock-free ring buffers shared between the two RP2040 cores.

## Why this example exists

W55RP20 integrates an RP2040 and a hardware TCP/IP offload engine (TOE) in a
single package.  Adding a PIO MII interface with an external YT8111 PHY gives
the board a **second, physically separate Ethernet port**.

`pico_rmii_gateway` routes TCP streams between those two ports entirely in
firmware — no external router needed.  It demonstrates:

- Dual-stack operation: TOE hardware offload on one side, lwIP software stack
  on the other
- Multicore design: each core owns exactly one network stack, no shared SPI bus
- Lock-free inter-core data transfer via ring buffers

## Use cases

- IoT edge gateway — field devices (SPE/10BASE-T1L) ↔ cloud (standard Ethernet)
- Industrial network bridge — isolate two Ethernet segments at the TCP layer
- Protocol translation middlebox (add processing in the ring-buffer drain loop)
- Embedded NAT / port-forward appliance prototype

## Architecture

```
       Standard RJ45                  10BASE-T1L SPE
     (W55RP20 TOE)                     (YT8111 PHY)
     192.168.11.13                    192.168.11.12
     listen: port 5000               connect -> 192.168.11.5:5001
           |                                |
           | HW TCP/IP offload              | MII -- GPIO 4-15
           |                                |
  +--------+--------------------------------+-------+
  |                W55RP20_EVB_PICO                 |
  |                                                 |
  |   Core 0                     Core 1             |
  |  +----------------+         +----------------+  |
  |  |  W55RP20 TOE   | g_rb_   |    RP2040      |  |
  |  |  server FSM    |--w2m--> |  PIO MII (pio0)|  |
  |  |  (session      |         |  + lwIP        |  |
  |  |   state owns   |<--m2w-- |  tcp_write /   |  |
  |  |   TOE SPI)     | g_rb_   |  recv callback |  |
  |  +----------------+         +----------------+  |
  |                                                  |
  |  g_rb_w2m : TOE -> MII  (Core 0 produces,       |
  |                           Core 1 consumes)       |
  |  g_rb_m2w : MII -> TOE  (Core 1 produces,       |
  |                           Core 0 consumes)       |
  +-------------------------------------------------+
```

## Data flow

```
RJ45 client --> TOE recv --> g_rb_w2m --> lwIP tcp_write --> SPE server
SPE server  --> lwIP recv cb --> g_rb_m2w --> TOE send --> RJ45 client
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

| Interface        | IP              | Role                                             |
|------------------|-----------------|--------------------------------------------------|
| W55RP20 TOE      | 192.168.11.13   | TCP server — listens on port 5000                |
| PIO MII + lwIP   | 192.168.11.12   | TCP client — connects to `GW_MII_TARGET_IP:5001` |

Edit `GW_MII_TARGET_IP` in `main.c` to point to the SPE-side server.

## How to test

1. Flash the board and open a serial monitor (115200 baud).
2. Run a TCP echo server on the SPE network at `192.168.11.5:5001`
   (e.g. `ncat -l 5001 --keep-open --exec "/bin/cat"`).
3. Connect to the TOE port from the RJ45 network:

```bash
nc 192.168.11.13 5000
```

Data flows: RJ45 client → TOE → ring buffer → lwIP → SPE server → ring buffer → TOE → RJ45 client.

## Multicore split

| Core   | Owns                          | Responsibility                                                |
|--------|-------------------------------|---------------------------------------------------------------|
| Core 0 | TOE SPI, `g_session`          | W55RP20 TOE server FSM, ring-buffer produce/consume (w2m/m2w) |
| Core 1 | MII PIO, lwIP, `mii_pcb`      | MII Ethernet poll, lwIP tcp callbacks, ring-buffer drain       |

Neither core touches the other's network stack after initialisation.
