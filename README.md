# QR Kiosk Scanner

A real-time QR code scanner that captures video from a camera, decodes QR codes, and sends them to a server via HTTP POST.

## Platform Support

This project supports two build modes:
- **Development Mode** (Linux): Uses `ffmpeg` with V4L2 for camera capture
- **Production Mode** (Raspberry Pi): Uses `rpicam-vid` for hardware-accelerated capture

---

## Dependencies

### Linux Development (Arch Linux)
```bash
sudo pacman -S gcc libjpeg-turbo zbar curl ffmpeg v4l-utils
```

### Raspberry Pi Production
```bash
sudo apt install build-essential libjpeg-dev libzbar-dev libcurl4-openssl-dev
```
(Note: `rpicam-vid` is pre-installed on modern Raspberry Pi OS)

---

## Building

### Quick Start

**For Linux Development:**
```bash
make dev
```

**For Raspberry Pi Production:**
```bash
make prod
```

### Manual Compilation

#### Development Build
```bash
gcc -std=c11 -O3 -Wall -Wextra -pthread \
    -o qr_cam \
    src/main.c src/mjpeg_grabber.c src/server_communication.c \
    -lpthread -ljpeg -lzbar -lcurl
```

#### Production Build
```bash
gcc -std=c11 -O3 -Wall -Wextra -pthread \
    -DUSE_RPI_CAM -march=armv8-a -mtune=cortex-a53 \
    -o qr_cam \
    src/main.c src/mjpeg_grabber.c src/server_communication.c \
    -lpthread -ljpeg -lzbar -lcurl
```

---

## Configuration

The scanner requires these environment variables:

1. **QR_SERVER_URL**: The endpoint to send QR scan data to
2. **TEMPLE_ID**: Unique identifier for the temple
3. **KIOSK_ID**: Unique identifier for this specific kiosk machine
4. **PRINTER_PATH**: Device path for the thermal printer (optional)

### Setup

1. Copy the example environment file:
   ```bash
   cp example.env .env
   ```

2. Edit `.env` with your configuration:
   ```bash
   QR_SERVER_URL=http://localhost:3000/api/kiosk/v1/qrscan
   TEMPLE_ID=temple_001
   KIOSK_ID=kiosk_main_entrance
   PRINTER_PATH=/dev/usb/lp0
   ```

3. Run using the helper script (automatically loads `.env`):
   ```bash
   ./run.sh
   ```

Or manually export the variables:
```bash
export QR_SERVER_URL=http://your-server.com/api/kiosk/v1/qrscan
export TEMPLE_ID=your_temple_id
export KIOSK_ID=your_kiosk_id
./qr_cam
```

### JSON Payload

The scanner sends QR data to the server as JSON:
```json
{
  "qrData": "scanned_code_here",
  "templeId": "temple_001",
  "kioskId": "kiosk_main_entrance"
}
```

The server response should include a `data` field, which will be:
- Printed to stdout
- Saved to `qrscan_data.json` in the current directory

### Thermal Printer Integration

If `PRINTER_PATH` is set, the scanner will automatically call the thermal printer after saving the response:

```bash
./printer qrscan_data.json /dev/usb/lp0
```

The printer executable should accept:
1. **First argument**: Path to the JSON file containing print data
2. **Second argument**: Printer device path

If printing fails or `PRINTER_PATH` is not set, the scanner will continue operating normally.

---

## Running

Using the helper script (recommended):
```bash
./run.sh
```

Or manually:
```bash
./qr_cam
```

The application will:
1. Start the camera stream (640×480 @ 5fps)
2. Continuously scan for QR codes
3. Send detected QR data to the server with temple and kiosk IDs
4. Save the server's `data` response to `qrscan_data.json`
5. Log all activity to stdout

Press `Ctrl+C` to stop gracefully.

---

## Testing Camera Setup

### On Linux (Development)

**List available cameras:**
```bash
v4l2-ctl --list-devices
```

**Test camera with ffmpeg:**
```bash
# List supported formats
ffmpeg -f v4l2 -list_formats all -i /dev/video0

# Test MJPEG capture
ffmpeg -f v4l2 -video_size 640x480 -framerate 5 -i /dev/video0 -f mjpeg test.jpg
```

**If your camera is not /dev/video0**, modify `src/mjpeg_grabber.c` line that says `-i /dev/video0` to your device.

### On Raspberry Pi (Production)

**Test rpicam-vid:**
```bash
rpicam-vid -t 5000 --codec mjpeg -o test.mjpeg
```

---

## Architecture

```
Camera → mjpeg_grabber → JPEG frames → main.c (QR scanner) → server_communication → HTTP POST
```

### Modules

- **`mjpeg_grabber.c/h`**: Camera abstraction layer with platform-specific backends
- **`main.c`**: QR scanning loop using zbar
- **`server_communication.c/h`**: HTTP client for sending QR data

### Camera Backends

The `mjpeg_grabber` module uses conditional compilation:

**Linux (Development):** Spawns `ffmpeg` with V4L2 input
```bash
ffmpeg -f v4l2 -video_size 640x480 -framerate 5 -i /dev/video0 -f mjpeg -
```

**Raspberry Pi (Production):** Spawns `rpicam-vid` with hardware encoding
```bash
rpicam-vid -t 0 -n --width 640 --height 480 --framerate 5 --codec mjpeg -o -
```

Both output MJPEG to stdout, which is parsed by the same reader thread.

---

## API

### `mjpeg_grabber.h`

```c
int stream_start(int width, int height, int fps);
void stream_stop(void);
int get_latest_jpeg_copy(uint8_t **out_buf, size_t *out_sz);
int grab_jpeg(const char *path);  // legacy file saver
```

### `server_communication.h`

```c
int server_comm_init(const char *server_url);
int send_qr_to_server(const char *qr_data);
void server_comm_cleanup(void);
```

---

## Troubleshooting

**Camera not found:**
- Linux: Check `ls -l /dev/video*` for available devices
- Raspberry Pi: Ensure camera is enabled in `raspi-config`

**Permission denied:**
```bash
sudo usermod -a -G video $USER
# Log out and back in
```

**ffmpeg/rpicam-vid not found:**
- Install missing dependencies (see Dependencies section)

**QR codes not detected:**
- Ensure good lighting
- Hold QR code steady and in focus
- Check console for "QR data: ..." messages

---

## License

MIT
