# cam_web_streaming

MJPEG video from the OV3660 on this board's FFC, served over the W6300 to any
browser. The device hosts its own control page; nothing is needed on the PC
side.

Only builds for `W6300_RP2354B_CAM`. The pinout, the PIO GPIO window it needs,
and the 48-GPIO part are all specific to this board.

```
GET /                  the page
GET /stream            multipart/x-mixed-replace MJPEG
GET /api/start         begin streaming        -> status JSON
GET /api/stop          stop streaming         -> status JSON
GET /api/res?v=WxH     change resolution      -> status JSON
GET /api/quality?v=N   JPEG quality, 4..63    -> status JSON
GET /api/reset         re-apply sensor config -> status JSON
GET /api/status        current state          -> status JSON
```

Default address is `192.168.11.3`, static, set in `main.c`.

## How a frame gets from the sensor to the wire

The OV3660 compresses to JPEG itself. That is not a convenience, it is the
reason the board works at all: there is no image codec on RP2350, and an
uncompressed 3 MP frame does not fit in 520 KB of SRAM. So the firmware never
touches pixels - it finds where the JPEG ends and puts it on the socket.

```
OV3660 ── 8-bit DVP ──> PIO (pio2) ──> DMA ──> g_frame_buf ──> W6300 socket
          GPIO24..33                          200 KB
```

`cam/dvp.pio` samples one byte per PCLK while HREF is high, autopushing 32-bit
words. DMA moves those words into the frame buffer in chunks while the CPU
walks the JPEG marker segments to find the real end of the image.

That walk is the part worth reading (`cam/dvp_capture.c`). The sensor keeps
driving the bus for the whole active frame period, long after the picture has
finished, so a capture that simply reads until VSYNC drops overruns the buffer
at the larger frame sizes. Scanning blindly for `FF D9` is not safe either -
byte stuffing applies only to entropy-coded data, so a quantisation or Huffman
table can legally contain that pair. The parser walks segments by their
declared length until SOS and only then hunts for the end marker.

## Pins

From `port/board/w6300_rp2354b_cam_pins.h`:

| Signal | GPIO | Note |
|---|---|---|
| D0..D7 | 24..31 | consecutive and in order |
| PCLK | 32 | PIO pin 8 |
| HREF | 33 | PIO pin 9 |
| VSYNC | 34 | polled by the CPU, not in the PIO program |
| XCLK | 35 | PWM |
| SIOD / SIOC | 36 / 37 | I2C0 |
| RESET / PWDN | none | RC on the FFC - see below |

The ten bus pins landing consecutively in exactly the order the PIO program
wants is what lets a single `in pins, 8` do the capture.

## Three things that are specific to this board

**PIO GPIO base 16.** An RP2350B PIO block reaches 32 pins at a time, either
0-31 or 16-47. PCLK and HREF are GPIO32 and 33, so the low window cannot see
them. The camera runs on `pio2` with base 16; the W6300 keeps pio0/pio1 at the
default base, which its own pins (16-23) are reachable from either way.

**XCLK is 25 MHz, not the usual 20.** RP2350 cannot make 20 MHz cleanly from a
150 MHz system clock - 150/20 is 7.5, and PWM would fall back to its fractional
divider, which skips cycles rather than dividing. That jitter lands straight on
the sensor's PLL input. 150/6 is exactly 25 MHz, and 25 MHz is inside the
OV3660's 6-27 MHz range. Everything downstream scales: the PLL settings the
driver picks for JPEG describe 10 MHz PCLK at 20 MHz XCLK, so here they give
12.5 MHz. The system clock is left at the SDK default of 150 MHz for the same
reason - moving it would also move the W6300's PIO SPI off the divider the
board was brought up on.

**There is no sensor reset.** RESET and PWDN reach the sensor only through RC
networks on the FFC; no GPIO touches either (`CAM_PIN_RESET` and
`CAM_PIN_PWDN` are -1). `/api/reset` rewrites the mode registers, which is all
firmware can do. A sensor wedged deeper than that needs a board reset.

## The vendor driver

`cam/ov3660.c`, `ov3660.h`, `ov3660_regs.h`, `ov3660_settings.h` and `sensor.h`
are carried unmodified from espressif/esp32-camera 2.1.7. Everything they
expect from ESP-IDF is supplied locally:

| Needed | Supplied by |
|---|---|
| `sccb.h` - `SCCB_Read16`/`Write16` | `cam/sccb.c`, over I2C0 |
| `xclk.h` - `xclk_timer_conf` | `cam/xclk.c`, over PWM |
| `esp_attr.h` - `DRAM_ATTR` | `cam/esp_shim/esp_attr.h`, expands to nothing |
| `esp_log.h` | `cam/esp_shim/esp_log.h`, printf |
| `freertos/task.h` - `vTaskDelay` | `cam/esp_shim/freertos/task.h`, `sleep_ms` |
| `resolution[]` | `cam/sensor_res.c` |

Keeping the driver byte-identical means re-syncing with upstream is a file
copy, not a merge. `cam/cam_sensor.c` holds everything that is ours.

Swapping in another esp32-camera sensor is mostly a matter of copying its four
files and changing the detect/init calls in `cam_sensor.c` - provided it can
emit JPEG, which is not optional here.

## Reading the numbers on the page

The page splits the frame period into three measured parts, so the bottleneck
is identified rather than guessed:

```
1000 / fps  ~=  vsync_ms + read_ms + send_ms
```

- **vsync_ms** high: the device finishes early and idles until the sensor
  starts the next frame. The rate is quantised by the sensor mode; a smaller
  frame size is the lever.
- **read_ms** high: pulling the JPEG off the bus dominates. A coarser quality
  makes frames smaller.
- **send_ms** high: the network is the limit. This is the number that differs
  between a hardwired stack and a software one.

## Status

Builds clean: `cam_web_streaming.uf2`, 212 KB. 111 KB flash, 209 KB of SRAM
(the 200 KB frame buffer dominates), out of 2 MB and 520 KB.

**Blocked on a board fault. The camera has never streamed.** Bring-up stops at
sensor detection, and the cause is on the board, not in this code:

```
VCC_2V8   measures 2.8 V   correct
VCC_1V8   measures 1.5-1.6 V   should be 1.8 V
```

U6 is an `SGM2036S-1.8XXDH4G`, a fixed 1.8 V part. U6 and U7 share the same
EN and VIN net, and U7 is correct, so the input and enable path are fine and
the low rail is U6 itself - most likely a 1.5 V variant fitted by mistake, as
the part is distinguished only by its marking code. With the sensor core rail
low the sensor never completes its power-on sequence, which is exactly what is
observed: it drives no clock and acknowledges no SCCB address.

Reported to hardware. Nothing further can be proven here until a board with a
correct 1V8 rail is available.

### What bring-up did establish

- pin mapping, checked against the schematic by coordinate: SDA=GPIO36,
  SCL=GPIO37, XCLK=GPIO35, VSYNC=GPIO34, HREF=GPIO33
- PIO GPIO base 16 on pio2, state machine and DMA all claimed
- XCLK generated at 25 MHz on GPIO35 (PWM slice 9, channel B)
- the SCCB lines are neither open nor shorted - they return high when released
- the sensor is an OV3660 (printed on the flex), not the OV5640 the reference
  design annotates

### Still untested, in the order it will be hit

- whether the OV3660 answers on SCCB
- VSYNC polarity. The capture assumes active-high framing; a sensor that frames
  the other way round reports `VSYNC stuck`, not a bad picture
- the whole capture path - PIO sampling, DMA, and the JPEG walk have never seen
  a real frame

## Diagnostics

When `cam_sensor_init` cannot find the sensor it runs `sccb_diagnose()` before
giving up, and that output is the fastest way back into the problem:

```
[W xclk] GPIO35 = 25000000 Hz (slice 9 ch B, wrap 5)
[W cam] SCCB rise time: SDA 9 us, SCL 3 us (1 us is the I2C limit)
[W cam] XCLK pad on GPIO35: N edges in 5 ms (non-zero = toggling)
[W cam] DVP activity over 50 ms: PCLK=0 HREF=0 VSYNC=0 edges
[W cam] SCCB: nothing acked on the whole bus
```

- **DVP activity all zero** is the important one. An OV part drives its timing
  outputs from XCLK alone, with no register writes, so zero here means the
  fault is upstream of the register bus: no module, no rails, or no XCLK.
  Non-zero would narrow it to SCCB.
- **Rise time** measures the bus RC. There are no pull-up resistors on
  SIOD/SIOC anywhere on this board, only the RP2350 internal 50k-80k, so the
  measured 9 us and 3 us are far outside the 1 us that standard mode I2C
  allows. `CAM_SCCB_HZ` is set to 20 kHz to live with that. External 2.2k-4.7k
  pull-ups to VCC_2V8 would be the proper fix and would allow 100 kHz back.
- **XCLK pad** reads the pin back through the pad, so a zero separates "the PWM
  was never routed" from "the sensor has a clock but no power".

Raise `CAM_LOG_LEVEL` in `cam/esp_shim/esp_log.h` to 3 to get the vendor
driver's own info lines, including the PID it read when detection fails.
