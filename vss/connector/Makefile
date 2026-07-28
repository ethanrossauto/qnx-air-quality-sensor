# Build the two QPP signal connectors for a QNX aarch64le target.
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Ethan Ross
#
# Prerequisite: the QNX SDP 8.0 environment on your PATH.
#
#   source <your-sdp>/qnxsdp-env.sh
#   make                 # both targets
#   make publisher       # direct I2C -> signal service (no Sensor Framework)
#   make connector       # Sensor Framework unit -> signal service
#
# Everything resolves relative to this folder, so a fresh clone builds without
# editing paths. Override CPU to target another architecture, e.g.
#   make CPU=gcc_ntox86_64
#
# Note: qpp itself is not built here. Get it from
# github.com/qnx/qnx-posix-publish-subscribe and build it with the same SDP.

CPU     ?= gcc_ntoaarch64le
CC       = qcc -V$(CPU)
CFLAGS  ?= -Wall -Wextra -O2

# The connector decodes the driver's packed payload, so it needs the driver header.
DRIVER_INC = ../../driver

# Set to 1 if your sensor reports true TVOC in ppb rather than a dimensionless
# VOC index. See the comment block in either source file.
VOC_IS_PPB ?= 0
ifeq ($(VOC_IS_PPB),1)
CFLAGS += -DAQ_VOC_FIELD_IS_PPB
endif

.PHONY: all publisher connector clean

all: publisher connector

# Direct path: opens the I2C bus itself. SDP headers only, links nothing extra.
publisher: aq_signal_publisher

aq_signal_publisher: aq_signal_publisher.c
	$(CC) $(CFLAGS) -o $@ $<

# Bridged path: subscribes to a Sensor Framework unit, so it links libsensor.
connector: aq_signal_connector

aq_signal_connector: aq_signal_connector.c $(DRIVER_INC)/aq_external_sensor.h
	$(CC) $(CFLAGS) -I$(DRIVER_INC) -o $@ $< -lsensor

clean:
	rm -f aq_signal_publisher aq_signal_connector
