#!/bin/bash
# Script to load environment variables and run the QR kiosk scanner

# Check if .env file exists
if [ ! -f .env ]; then
    echo "Error: .env file not found!"
    echo "Please copy example.env to .env and configure your settings:"
    echo "  cp example.env .env"
    exit 1
fi

# Load environment variables from .env
set -a
source .env
set +a

# Run the QR scanner
./qr_cam
