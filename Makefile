# Makefile for QR Kiosk Scanner
# Supports both Linux development and Raspberry Pi production builds

# Compiler and flags
CC = gcc
CFLAGS = -std=c11 -O3 -Wall -Wextra -pthread
LIBS = -lpthread -ljpeg -lzbar -lcurl

# Source files
SRC_DIR = src
SOURCES = $(SRC_DIR)/main.c $(SRC_DIR)/mjpeg_grabber.c $(SRC_DIR)/server_communication.c
HEADERS = $(SRC_DIR)/mjpeg_grabber.h $(SRC_DIR)/server_communication.h

# Output binary
TARGET = qr_cam

# Default target shows help
.PHONY: help
help:
	@echo "QR Kiosk Scanner - Build Targets:"
	@echo ""
	@echo "  make dev      - Build for Linux development (uses ffmpeg)"
	@echo "  make prod     - Build for Raspberry Pi production (uses rpicam-vid)"
	@echo "  make clean    - Remove build artifacts"
	@echo "  make check    - Check dependencies"
	@echo ""

# Development build (Linux with ffmpeg)
.PHONY: dev
dev: $(SOURCES) $(HEADERS)
	@echo "Building for Linux Development (ffmpeg)..."
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES) $(LIBS)
	@echo "✓ Build complete: ./$(TARGET)"
	@echo ""
	@echo "Run with: export QR_SERVER_URL=http://your-server/api/qr && ./$(TARGET)"

# Production build (Raspberry Pi with rpicam-vid)
# Note: ARM optimization flags only work when compiling on Raspberry Pi
.PHONY: prod
prod: $(SOURCES) $(HEADERS)
	@echo "Building for Raspberry Pi (rpicam-vid)..."
	$(CC) $(CFLAGS) -DUSE_RPI_CAM -o $(TARGET) $(SOURCES) $(LIBS)
	@echo "✓ Build complete: ./$(TARGET)"
	@echo ""
	@echo "Run with: export QR_SERVER_URL=http://your-server/api/qr && ./$(TARGET)"
	@echo ""
	@echo "Note: For Raspberry Pi optimization, compile on the Pi itself."

# Clean build artifacts
.PHONY: clean
clean:
	@echo "Cleaning build artifacts..."
	rm -f $(TARGET)
	@echo "✓ Clean complete"

# Check if required dependencies are available
.PHONY: check
check:
	@echo "Checking dependencies..."
	@echo ""
	@echo "Required libraries:"
	@which $(CC) > /dev/null && echo "  ✓ gcc found" || echo "  ✗ gcc not found"
	@pkg-config --exists libjpeg && echo "  ✓ libjpeg found" || echo "  ✗ libjpeg not found (install: libjpeg or libjpeg-turbo)"
	@pkg-config --exists zbar && echo "  ✓ zbar found" || echo "  ✗ zbar not found (install: zbar)"
	@pkg-config --exists libcurl && echo "  ✓ libcurl found" || echo "  ✗ libcurl not found (install: curl)"
	@echo ""
	@echo "Runtime dependencies:"
	@which ffmpeg > /dev/null && echo "  ✓ ffmpeg found (for dev)" || echo "  ✗ ffmpeg not found (install for dev builds)"
	@which rpicam-vid > /dev/null && echo "  ✓ rpicam-vid found (for prod)" || echo "  ℹ rpicam-vid not found (only needed on Raspberry Pi)"
	@echo ""
	@echo "To install on Arch Linux:"
	@echo "  sudo pacman -S gcc libjpeg-turbo zbar curl ffmpeg v4l-utils"
	@echo ""
	@echo "To install on Raspberry Pi OS:"
	@echo "  sudo apt install build-essential libjpeg-dev libzbar-dev libcurl4-openssl-dev"
