/*
 * The frame size table ov3660.c indexes with a framesize_t.
 *
 * Lifted from esp32-camera's driver/sensor.c. Only this table is carried over.
 * The camera_sensor[] roster and esp_camera_sensor_get_info() that sit beside
 * it there enumerate seventeen sensors and pull in every sensor's PID header;
 * this build talks to one sensor, chosen at compile time, so probing a list
 * would only add flash.
 *
 * The rows must stay in framesize_t order - the driver indexes, it does not
 * search.
 */
#include "sensor.h"

const resolution_info_t resolution[FRAMESIZE_INVALID] = {
    {   96,   96, ASPECT_RATIO_1X1   }, /* 96x96   */
    {  160,  120, ASPECT_RATIO_4X3   }, /* QQVGA   */
    {  128,  128, ASPECT_RATIO_1X1   }, /* 128x128 */
    {  176,  144, ASPECT_RATIO_5X4   }, /* QCIF    */
    {  240,  176, ASPECT_RATIO_4X3   }, /* HQVGA   */
    {  240,  240, ASPECT_RATIO_1X1   }, /* 240x240 */
    {  320,  240, ASPECT_RATIO_4X3   }, /* QVGA    */
    {  320,  320, ASPECT_RATIO_1X1   }, /* 320x320 */
    {  400,  296, ASPECT_RATIO_4X3   }, /* CIF     */
    {  480,  320, ASPECT_RATIO_3X2   }, /* HVGA    */
    {  640,  480, ASPECT_RATIO_4X3   }, /* VGA     */
    {  800,  600, ASPECT_RATIO_4X3   }, /* SVGA    */
    { 1024,  768, ASPECT_RATIO_4X3   }, /* XGA     */
    { 1280,  720, ASPECT_RATIO_16X9  }, /* HD      */
    { 1280, 1024, ASPECT_RATIO_5X4   }, /* SXGA    */
    { 1600, 1200, ASPECT_RATIO_4X3   }, /* UXGA    */
    /* 3MP sensors - the OV3660 tops out here, at QXGA. */
    { 1920, 1080, ASPECT_RATIO_16X9  }, /* FHD     */
    {  720, 1280, ASPECT_RATIO_9X16  }, /* Portrait HD  */
    {  864, 1536, ASPECT_RATIO_9X16  }, /* Portrait 3MP */
    { 2048, 1536, ASPECT_RATIO_4X3   }, /* QXGA    */
    /* 5MP sensors - listed to keep the indices right, not reachable here. */
    { 2560, 1440, ASPECT_RATIO_16X9  }, /* QHD     */
    { 2560, 1600, ASPECT_RATIO_16X10 }, /* WQXGA   */
    { 1088, 1920, ASPECT_RATIO_9X16  }, /* Portrait FHD */
    { 2560, 1920, ASPECT_RATIO_4X3   }, /* QSXGA   */
    { 2592, 1944, ASPECT_RATIO_4X3   }, /* 5MP     */
};
