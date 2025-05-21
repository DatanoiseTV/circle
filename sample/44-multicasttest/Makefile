# Makefile for Circle Sample 44 - Multicast Test

CIRCLE_PATH = ../..

# Application specific sources
SOURCES = main.cpp kernel.cpp

# Add library search paths
LIBDIRS = $(CIRCLE_PATH)/lib $(CIRCLE_PATH)/addon/fatfs $(CIRCLE_PATH)/addon/sensor

# Add library names
LIBS = circle fatfs sensor

# Executable name
TARGET = kernel.img

# Default is for Raspberry Pi 3/4 64-bit
include $(CIRCLE_PATH)/Rules.mk
