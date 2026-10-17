/*
 * softpn_ndis_ioctl.h - interface of the softpn NDIS protocol driver (softpn_ndis.sys).
 *
 * Shared by the driver (drivers/softpn_ndis/sys) and the user-mode backend
 * (src/pn/l2_port_ndis.cpp). User mode must include <windows.h> and <ntddndis.h> first.
 *
 * Derived from protuser.h of the Microsoft NDISPROT 6x sample (MS-PL, see
 * drivers/softpn_ndis/upstream/LICENSE-MS-PL.txt). The IOCTL codes 0x200-0x205 are unchanged.
 *
 * Use:
 *   h = CreateFileW(SOFTPN_NDIS_DOS_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
 *                   FILE_FLAG_OVERLAPPED, NULL);
 *   IOCTL_NDISPROT_QUERY_BINDING (index 0, 1, ...)  -> adapter names "\DEVICE\{GUID}"
 *   IOCTL_NDISPROT_OPEN_DEVICE   (UTF-16 adapter name, no terminator) -> one adapter per handle
 *   IOCTL_SOFTPN_SET_FILTER      -> EtherTypes to receive / allowed to send
 *   ReadFile  = one received frame (FCS not included), WriteFile = one frame to send
 */
#ifndef SOFTPN_NDIS_IOCTL_H
#define SOFTPN_NDIS_IOCTL_H

#define SOFTPN_NDIS_NT_NAME   L"\\Device\\SoftPnNdis"
#define SOFTPN_NDIS_LINK_NAME L"\\Global??\\SoftPnNdis"
#define SOFTPN_NDIS_DOS_NAME  L"\\\\.\\SoftPnNdis"

/* Bumped on every incompatible change of the structures below. */
#define SOFTPN_NDIS_API_VERSION 1

#define FSCTL_NDISPROT_BASE FILE_DEVICE_NETWORK

#define _NDISPROT_CTL_CODE(_Function, _Method, _Access) \
    CTL_CODE(FSCTL_NDISPROT_BASE, _Function, _Method, _Access)

#define IOCTL_NDISPROT_OPEN_DEVICE \
    _NDISPROT_CTL_CODE(0x200, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_NDISPROT_QUERY_OID_VALUE \
    _NDISPROT_CTL_CODE(0x201, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_NDISPROT_SET_OID_VALUE \
    _NDISPROT_CTL_CODE(0x205, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_NDISPROT_QUERY_BINDING \
    _NDISPROT_CTL_CODE(0x203, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_NDISPROT_BIND_WAIT \
    _NDISPROT_CTL_CODE(0x204, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

/* softpn additions */
#define IOCTL_SOFTPN_GET_VERSION \
    _NDISPROT_CTL_CODE(0x210, METHOD_BUFFERED, FILE_READ_ACCESS)          /* out: ULONG */
#define IOCTL_SOFTPN_SET_FILTER \
    _NDISPROT_CTL_CODE(0x211, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS) /* in: SOFTPN_FILTER */
#define IOCTL_SOFTPN_GET_STATS \
    _NDISPROT_CTL_CODE(0x212, METHOD_BUFFERED, FILE_READ_ACCESS)          /* out: SOFTPN_STATS */

/* Structure to go with IOCTL_NDISPROT_QUERY_OID_VALUE. Data is variable length. */
typedef struct _NDISPROT_QUERY_OID
{
    NDIS_OID            Oid;
    NDIS_PORT_NUMBER    PortNumber;
    UCHAR               Data[sizeof(ULONG)];
} NDISPROT_QUERY_OID, *PNDISPROT_QUERY_OID;

/* Structure to go with IOCTL_NDISPROT_SET_OID_VALUE. Data is variable length. */
typedef struct _NDISPROT_SET_OID
{
    NDIS_OID            Oid;
    NDIS_PORT_NUMBER    PortNumber;
    UCHAR               Data[sizeof(ULONG)];
} NDISPROT_SET_OID, *PNDISPROT_SET_OID;

/*
 * Structure to go with IOCTL_NDISPROT_QUERY_BINDING. In: BindingIndex (0-based). Out: the
 * adapter name ("\DEVICE\{GUID}") and the friendly name, UTF-16, at the given offsets.
 */
typedef struct _NDISPROT_QUERY_BINDING
{
    ULONG            BindingIndex;
    ULONG            DeviceNameOffset;    /* from start of this struct */
    ULONG            DeviceNameLength;    /* in bytes */
    ULONG            DeviceDescrOffset;   /* from start of this struct */
    ULONG            DeviceDescrLength;   /* in bytes */
} NDISPROT_QUERY_BINDING, *PNDISPROT_QUERY_BINDING;

#define SOFTPN_MAX_ETHERTYPES 8

/* Also accept frames tagged 802.1Q (inline 0x8100 or stripped by the NIC) whose inner EtherType
   is in the list. On receive the tag is always delivered inline. */
#define SOFTPN_FILTER_VLAN        0x00000001
/* Drop received frames whose source MAC is the adapter's own address. */
#define SOFTPN_FILTER_DROP_OWN    0x00000002

/*
 * Per open handle. Until the first SET_FILTER (and after a filter with EtherTypeCount 0) nothing
 * is received and nothing can be sent. Sends are allowed only for listed EtherTypes (outer, or
 * inner after an inline 0x8100 tag if SOFTPN_FILTER_VLAN) and only with the adapter's source MAC.
 */
typedef struct _SOFTPN_FILTER
{
    ULONG   Version;                            /* SOFTPN_NDIS_API_VERSION */
    ULONG   Flags;                              /* SOFTPN_FILTER_* */
    ULONG   EtherTypeCount;                     /* 0..SOFTPN_MAX_ETHERTYPES */
    USHORT  EtherTypes[SOFTPN_MAX_ETHERTYPES];  /* host byte order, e.g. 0x8892 */
} SOFTPN_FILTER, *PSOFTPN_FILTER;

/* Counters of one open handle, reset by IOCTL_NDISPROT_OPEN_DEVICE. */
typedef struct _SOFTPN_STATS
{
    ULONG   Version;            /* SOFTPN_NDIS_API_VERSION */
    ULONG   QueueDepth;         /* frames waiting for a read now */
    ULONG   QueueHighWater;     /* maximum of QueueDepth */
    ULONG   QueueLimit;         /* frames kept before the oldest is dropped */
    ULONG64 RxAccepted;         /* frames matching the filter */
    ULONG64 RxDelivered;        /* frames copied to a read */
    ULONG64 RxDroppedQueueFull; /* oldest frames dropped because nobody read */
    ULONG64 RxDroppedOwn;       /* loopback or own source MAC */
    ULONG64 RxDroppedNoMemory;
    ULONG64 RxVlanReinserted;   /* tag stripped by the NIC, put back inline */
    ULONG64 TxFrames;           /* completed sends */
    ULONG64 TxErrors;           /* sends completed with an error */
    ULONG64 TxRejected;         /* writes refused by the EtherType / source MAC check */
} SOFTPN_STATS, *PSOFTPN_STATS;

#endif /* SOFTPN_NDIS_IOCTL_H */
