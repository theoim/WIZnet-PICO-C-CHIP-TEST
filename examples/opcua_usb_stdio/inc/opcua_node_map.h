#ifndef OPCUA_NODE_MAP_H
#define OPCUA_NODE_MAP_H

/*
 * Planned namespace-1 node map for the UAExpert-visible prototype.
 *
 * Objects
 *   Sensor_Node                 ns=1;i=1000
 *     Device                   ns=1;i=1001
 *       DeviceName             ns=1;s="Device.Name"
 *       IPAddress              ns=1;s="Device.IP"
 *       FwVersion              ns=1;s="Device.FwVer"
 *       Uptime_s               ns=1;s="Device.Uptime"
 *     USB_Stdio                ns=1;i=2000
 *       RawFrame               ns=1;s="Input.RawFrame"
 *       Channel_1              ns=1;s="Input.Ch1"
 *       Channel_2              ns=1;s="Input.Ch2"
 *       Channel_3              ns=1;s="Input.Ch3"
 *       FrameCount             ns=1;s="Input.FrameCount"
 *       ParseErrorCount        ns=1;s="Input.ParseErrorCount"
 */

#define OPCUA_NODE_ROOT_NUMERIC_ID 1000u
#define OPCUA_NODE_DEVICE_NUMERIC_ID 1001u
#define OPCUA_NODE_INPUT_NUMERIC_ID 2000u

#define OPCUA_NODE_DEVICE_NAME "Device.Name"
#define OPCUA_NODE_DEVICE_IP "Device.IP"
#define OPCUA_NODE_DEVICE_FW_VERSION "Device.FwVer"
#define OPCUA_NODE_DEVICE_UPTIME "Device.Uptime"

#define OPCUA_NODE_INPUT_RAW_FRAME "Input.RawFrame"
#define OPCUA_NODE_INPUT_CH1 "Input.Ch1"
#define OPCUA_NODE_INPUT_CH2 "Input.Ch2"
#define OPCUA_NODE_INPUT_CH3 "Input.Ch3"
#define OPCUA_NODE_INPUT_FRAME_COUNT "Input.FrameCount"
#define OPCUA_NODE_INPUT_PARSE_ERROR_COUNT "Input.ParseErrorCount"

#endif /* OPCUA_NODE_MAP_H */
