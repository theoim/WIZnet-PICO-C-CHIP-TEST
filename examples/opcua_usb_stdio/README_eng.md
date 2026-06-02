# OPC UA USB stdio Example

This example runs an OPC UA server on a WIZnet W6300 + RP2350 board.
Sensor data is sent via USB serial (Tera Term) and monitored in real time using UAExpert.

---

## Required Software

| Software | Purpose | Download |
|----------|---------|---------|
| Tera Term | USB serial communication | https://teratermproject.github.io |
| UaExpert | OPC UA client | https://www.unified-automation.com/downloads/opc-ua-clients.html |

---

## 1. Upload Firmware

Upload the generated `opcua_usb_stdio.uf2` file to the board.

1. Hold the **BOOTSEL button** while connecting the USB cable
   (or hold BOOTSEL and press RESET)
2. A drive will appear on your PC — drag and drop `opcua_usb_stdio.uf2` onto it
3. The board restarts automatically

### Verify with Tera Term

1. Open Tera Term → select the COM port → connect
2. The following log confirms normal operation

```
=== WIZnet OPC UA USB stdio prototype ===
USB CDC input is active on the RP2350 USB-C port.
[WIZnet] initializing WIZnet network
...
[OPC UA] state=RUNNING endpoint=opc.tcp://192.168.11.2:4840
```

### Echo and Line Ending Setup

Configure Tera Term so typed commands are echoed back to the screen.

Go to **Setup → Terminal**:
- **Transmit**: change `CR` → `CR+LF`
- Check the **Local echo** checkbox

<img src="images/opcua_explain_1.png" width="50%">

---

## 2. Add Server in UaExpert

1. Click **Server → Add...** in the UaExpert menu bar

<img src="images/opcua_explain_2.png" width="50%">

2. Double-click `< Double click to Add Server... >` under **Custom Discovery**

3. Enter the server address and press Enter

```
opc.tcp://192.168.11.2:4840
```

<img src="images/opcua_explain_3.png" width="50%">

---

## 3. Select None Security and Connect

From the discovered server list:

1. Select **None - None (uatcp-uasc-uabinary)** under `WIZnet OPC UA USB stdio`
2. Click **OK**

> This version runs without security (SecurityPolicy: None)

<img src="images/opcua_explain_4.png" width="50%">

---

## 4. View Nodes in Data Access View

After connecting, the **ADDRESS SPACE** panel on the left shows the node tree.

```
Root
└── Objects
    └── Sensor_Node
        ├── Device
        │   ├── DeviceName
        │   ├── IPAddress
        │   ├── FwVersion
        │   └── Uptime_s
        └── USB_Stdio
            ├── Channel_1
            ├── Channel_2
            ├── Channel_3
            ├── RawFrame
            ├── FrameCount
            └── ParseErrorCount
```

1. Click the **DATA ACCESS VIEW** tab at the top
2. Expand `USB_Stdio`
3. **Drag** `Channel_1`, `Channel_2`, `Channel_3` into the DATA ACCESS VIEW panel

<img src="images/opcua_explain_5.png" width="80%">

---

## 5. Send Data from Tera Term

Type the following command in Tera Term:

```
$DATA:23.50,101.32,65.20
```

<img src="images/opcua_explain_6.png" width="40%">

---

## 6. Verify Values in UaExpert

The Channel_1/2/3 values update in real time in the DATA ACCESS VIEW panel.

<img src="images/opcua_explain_7.png" width="80%">

---

## Available Commands

| Command | Description |
|---------|-------------|
| `$DATA:<ch1>,<ch2>,<ch3>` | Set channel values (decimal supported) |
| `GET` | Print current channel values |
| `NET` | Print network status |
| `OPCUA` | Print OPC UA server status |
| `CLEAR` | Reset channel values |
| `HELP` | Print command list |
