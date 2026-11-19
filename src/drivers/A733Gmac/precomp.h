#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <ntintsafe.h>
#include <netadaptercx.h>
#include <netadapter.h>
#include <netdevice.h>
#include <netpacketqueue.h>
#include <netrxqueue.h>
#include <nettxqueue.h>
#include <net/logicaladdress.h>
#include <net/virtualaddress.h>

#include "registers.h"
#include "device.h"
#include "hardware.h"
#include "platform.h"
#include "phy.h"
#include "queues.h"
#include "adapter.h"
#include "netringiterator.h"

#define GMAC_POOL_TAG 'MGA7'
#define GMAC_LOG_ERROR(fmt, ...) DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_ERROR_LEVEL, "A733Gmac: " fmt "\n", __VA_ARGS__)
#define GMAC_LOG_INFO(fmt, ...)  DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL,  "A733Gmac: " fmt "\n", __VA_ARGS__)
