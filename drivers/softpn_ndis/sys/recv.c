/*++

Copyright (c) 2000  Microsoft Corporation

Module Name:

    recv.c

Abstract:

    NDIS protocol entry points and utility routines to handle receiving
    data.

Environment:

    Kernel mode only.

Revision History:

--*/

#include "precomp.h"

#define __FILENUMBER 'VCER'



NTSTATUS
NdisprotRead(
    IN PDEVICE_OBJECT       pDeviceObject,
    IN PIRP                 pIrp
    )
/*++

Routine Description:

    Dispatch routine to handle IRP_MJ_READ.

Arguments:

    pDeviceObject - pointer to our device object
    pIrp - Pointer to request packet

Return Value:

    NT status code.

--*/
{
    PIO_STACK_LOCATION      pIrpSp;
    NTSTATUS                NtStatus;
    PNDISPROT_OPEN_CONTEXT   pOpenContext;

    UNREFERENCED_PARAMETER(pDeviceObject);

    pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
    pOpenContext = pIrpSp->FileObject->FsContext;

    do
    {
        //
        // Validate!
        //
        if (pOpenContext == NULL)
        {
            DEBUGP(DL_FATAL, ("Read: NULL FsContext on FileObject %p\n",
                        pIrpSp->FileObject));
            NtStatus = STATUS_INVALID_HANDLE;
            break;
        }

        NPROT_STRUCT_ASSERT(pOpenContext, oc);

        if (pIrp->MdlAddress == NULL)
        {
            DEBUGP(DL_FATAL, ("Read: NULL MDL address on IRP %p\n", pIrp));
            NtStatus = STATUS_INVALID_PARAMETER;
            break;
        }

        //
        // Try to get a virtual address for the MDL.
        //
        if (MmGetSystemAddressForMdlSafe(pIrp->MdlAddress, NormalPagePriority | MdlMappingNoExecute) == NULL)
        {
            DEBUGP(DL_FATAL, ("Read: MmGetSystemAddr failed for IRP %p, MDL %p\n",
                    pIrp, pIrp->MdlAddress));
            NtStatus = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        NPROT_ACQUIRE_LOCK(&pOpenContext->Lock, FALSE);

        if (!NPROT_TEST_FLAGS(pOpenContext->Flags, NPROTO_BIND_FLAGS, NPROTO_BIND_ACTIVE))
        {
            NPROT_RELEASE_LOCK(&pOpenContext->Lock, FALSE);
            NtStatus = STATUS_INVALID_HANDLE;
            break;
        }

        IoSetCancelRoutine(pIrp, NdisprotCancelRead);

        if (pIrp->Cancel &&
            IoSetCancelRoutine(pIrp, NULL))
        {

            //
            // IRP has been canceled but the I/O manager did not manage to call our cancel routine. This
            // code is safe referencing the Irp->Cancel field without locks because of the memory barriers
            // in the interlocked exchange sequences used by IoSetCancelRoutine.
            //

            NtStatus = STATUS_CANCELLED;

            // IRP should be completed after releasing the lock

        }
        else
        {

            //
            //  Add this IRP to the list of pended Read IRPs
            //
            NPROT_INSERT_TAIL_LIST(&pOpenContext->PendedReads, &pIrp->Tail.Overlay.ListEntry);
            pIrp->Tail.Overlay.DriverContext[0] = (PVOID)pOpenContext;

            NPROT_REF_OPEN(pOpenContext);  // pended read IRP
            pOpenContext->PendedReadCount++;
            IoMarkIrpPending(pIrp);


            NtStatus = STATUS_PENDING;
        }

        NPROT_RELEASE_LOCK(&pOpenContext->Lock, FALSE);
        //
        //  Run the service routine for reads.
        //
        ndisprotServiceReads(pOpenContext);

    }
    while (FALSE);

    if (NtStatus != STATUS_PENDING)
    {
        NPROT_ASSERT(NtStatus != STATUS_SUCCESS);
        pIrp->IoStatus.Information = 0;
        pIrp->IoStatus.Status = NtStatus;
        IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    }

    return (NtStatus);
}


VOID
NdisprotCancelRead(
    IN PDEVICE_OBJECT               pDeviceObject,
    IN PIRP                         pIrp
    )
/*++

Routine Description:

    Cancel a pending read IRP. We unlink the IRP from the open context
    queue and complete it.

Arguments:

    pDeviceObject - pointer to our device object
    pIrp - IRP to be cancelled

Return Value:

    None

--*/
{
    PNDISPROT_OPEN_CONTEXT       pOpenContext;

    UNREFERENCED_PARAMETER(pDeviceObject);

    IoReleaseCancelSpinLock(pIrp->CancelIrql);

    pOpenContext = (PNDISPROT_OPEN_CONTEXT) pIrp->Tail.Overlay.DriverContext[0];
    NPROT_STRUCT_ASSERT(pOpenContext, oc);

    NPROT_ACQUIRE_LOCK(&pOpenContext->Lock, FALSE);

    NPROT_REMOVE_ENTRY_LIST(&pIrp->Tail.Overlay.ListEntry);
    pOpenContext->PendedReadCount--;


    NPROT_RELEASE_LOCK(&pOpenContext->Lock, FALSE);

    DEBUGP(DL_INFO, ("CancelRead: Open %p, IRP %p\n", pOpenContext, pIrp));
    pIrp->IoStatus.Status = STATUS_CANCELLED;
    pIrp->IoStatus.Information = 0;
    IoCompleteRequest(pIrp, IO_NO_INCREMENT);

    NPROT_DEREF_OPEN(pOpenContext); // Cancel removed pended Read

}



VOID
ndisprotServiceReads(
    IN PNDISPROT_OPEN_CONTEXT        pOpenContext
    )
/*++

Routine Description:

    Utility routine to copy received data into user buffers and
    complete READ IRPs.

Arguments:

    pOpenContext - pointer to open context

Return Value:

    None

--*/
{
    PIRP                pIrp = NULL;
    PLIST_ENTRY         pIrpEntry;
    PNET_BUFFER_LIST    pRcvNetBufList;
    PLIST_ENTRY         pRcvNetBufListEntry;
    PUCHAR              pSrc, pDst;
    ULONG               BytesRemaining; // at pDst
    PMDL                pMdl;
    ULONG               BytesAvailable;
    BOOLEAN             FoundPendingIrp = FALSE;
    ULONG               SrcTotalLength = 0; // Source NetBuffer DataLenght
    ULONG               Offset = 0;         // CurrentMdlOffset
    ULONG               BytesToCopy = 0;

    DEBUGP(DL_VERY_LOUD, ("ServiceReads: open %p/%x\n",
            pOpenContext, pOpenContext->Flags));

    NPROT_REF_OPEN(pOpenContext);  // temp ref - service reads

    NPROT_ACQUIRE_LOCK(&pOpenContext->Lock, FALSE);

    while (!NPROT_IS_LIST_EMPTY(&pOpenContext->PendedReads) &&
           !NPROT_IS_LIST_EMPTY(&pOpenContext->RecvNetBufListQueue))
    {
        FoundPendingIrp = FALSE;

        //
        //  Get the first pended Read IRP
        //
        pIrpEntry = pOpenContext->PendedReads.Flink;
        while (pIrpEntry != &pOpenContext->PendedReads)
        {
            pIrp = CONTAINING_RECORD(pIrpEntry, IRP, Tail.Overlay.ListEntry);

            //
            //  Check to see if it is being cancelled.
            //
            if (IoSetCancelRoutine(pIrp, NULL))
            {
                //
                //  It isn't being cancelled, and can't be cancelled henceforth.
                //
                NPROT_REMOVE_ENTRY_LIST(pIrpEntry);
                FoundPendingIrp = TRUE;
                break;

                //
                //  NOTE: we decrement PendedReadCount way below in the
                //  while loop, to avoid letting through a thread trying
                //  to unbind.
                //
            }
            else
            {
                //
                //  The IRP is being cancelled; let the cancel routine handle it.
                //
                DEBUGP(DL_INFO, ("ServiceReads: open %p, skipping cancelled IRP %p\n",
                        pOpenContext, pIrp));

                pIrpEntry = pIrpEntry->Flink;
            }
        }

        if (FoundPendingIrp == FALSE)
        {
            break;
        }
        //
        //  Get the first queued receive packet
        //
        pRcvNetBufListEntry = pOpenContext->RecvNetBufListQueue.Flink;
        NPROT_REMOVE_ENTRY_LIST(pRcvNetBufListEntry);

        pOpenContext->RecvNetBufListCount --;

        NPROT_RELEASE_LOCK(&pOpenContext->Lock, FALSE);

        NPROT_DEREF_OPEN(pOpenContext);  // Service: dequeue rcv packet

        pRcvNetBufList = NPROT_RCV_NBL_FROM_LIST_ENTRY(pRcvNetBufListEntry);
        NPROT_ASSERT(pRcvNetBufList != NULL);
        _Analysis_assume_(pRcvNetBufList != NULL);
        NPROT_RCV_NBL_FROM_LIST_ENTRY(pRcvNetBufListEntry) = NULL;

        //
        //  Copy as much data as possible from the receive packet to
        //  the IRP MDL.
        //

        pDst = NULL;
        NdisQueryMdl(pIrp->MdlAddress, &pDst, &BytesRemaining, NormalPagePriority | MdlMappingNoExecute);
        NPROT_ASSERT(pDst != NULL);  // since it was already mapped
        _Analysis_assume_(pDst != NULL);

        pMdl = NET_BUFFER_CURRENT_MDL(NET_BUFFER_LIST_FIRST_NB(pRcvNetBufList));

        //
        // Copy the data in the received packet into the buffer provided by the client.
        // If the length of the receive packet is greater than length of the given buffer,
        // we just copy as many bytes as we can. Once the buffer is full, we just discard
        // the rest of the data, and complete the IRP sucessfully even we only did a partial copy.
        //

        SrcTotalLength = NET_BUFFER_DATA_LENGTH(NET_BUFFER_LIST_FIRST_NB(pRcvNetBufList));
        Offset = NET_BUFFER_CURRENT_MDL_OFFSET(NET_BUFFER_LIST_FIRST_NB(pRcvNetBufList));

        while (BytesRemaining && (pMdl != NULL) && SrcTotalLength)
        {
            pSrc = NULL;
            NdisQueryMdl(pMdl, &pSrc, &BytesAvailable, NormalPagePriority | MdlMappingNoExecute);

            if (pSrc == NULL)
            {
                DEBUGP(DL_FATAL,
                    ("ServiceReads: Open %p, NdisQueryMdl failed for MDL %p\n",
                            pOpenContext, pMdl));
                break;
            }

            NPROT_ASSERT(BytesAvailable > Offset);

            BytesToCopy = MIN(BytesAvailable - Offset, BytesRemaining);
            BytesToCopy = MIN(BytesToCopy, SrcTotalLength);

            NPROT_COPY_MEM(pDst, pSrc + Offset, BytesToCopy);
            BytesRemaining -= BytesToCopy;
            pDst += BytesToCopy;
            SrcTotalLength -= BytesToCopy;

            //
            // CurrentMdlOffset is used only for the first Mdl processed. For the remaining Mdls, it is 0.
            //
            Offset = 0;

            NdisGetNextMdl(pMdl, &pMdl);
        }

        //
        //  Complete the IRP.
        //
        pIrp->IoStatus.Status = STATUS_SUCCESS;
        pIrp->IoStatus.Information = MmGetMdlByteCount(pIrp->MdlAddress) - BytesRemaining;

        DEBUGP(DL_INFO, ("ServiceReads: Open %p, IRP %p completed with %d bytes\n",
            pOpenContext, pIrp, (ULONG)pIrp->IoStatus.Information));

        IoCompleteRequest(pIrp, IO_NO_INCREMENT);
        InterlockedIncrement64((LONG64 *)&pOpenContext->Stats.RxDelivered);

        ndisprotFreeReceiveNetBufferList(pOpenContext, pRcvNetBufList,FALSE);


        NPROT_DEREF_OPEN(pOpenContext);    // took out pended Read

        NPROT_ACQUIRE_LOCK(&pOpenContext->Lock, FALSE);
        pOpenContext->PendedReadCount--;

    }

    NPROT_RELEASE_LOCK(&pOpenContext->Lock, FALSE);

    NPROT_DEREF_OPEN(pOpenContext);    // temp ref - service reads
}


//
//  softpn: EtherType filter helpers. EtherTypes are in host byte order.
//
static BOOLEAN
ndisprotFilterHasType(
    IN const SOFTPN_FILTER *        pFilter,
    IN USHORT                       EtherType
    )
{
    ULONG   i;

    for (i = 0; i < pFilter->EtherTypeCount && i < SOFTPN_MAX_ETHERTYPES; i++)
    {
        if (pFilter->EtherTypes[i] == EtherType)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static USHORT
ndisprotReadEthType(
    IN const UCHAR *                p
    )
{
    return (USHORT)((p[0] << 8) | p[1]);
}

//
//  Does the filter accept a frame with outer type OuterType (and, behind an inline 802.1Q tag,
//  InnerType)? VlanOob: the NIC stripped the tag into the OOB data, so OuterType already is the
//  inner type.
//
static BOOLEAN
ndisprotFilterMatches(
    IN const SOFTPN_FILTER *        pFilter,
    IN USHORT                       OuterType,
    IN BOOLEAN                      HaveInner,
    IN USHORT                       InnerType,
    IN BOOLEAN                      VlanOob
    )
{
    BOOLEAN Vlan = (pFilter->Flags & SOFTPN_FILTER_VLAN) != 0;

    if (VlanOob)
    {
        return Vlan && ndisprotFilterHasType(pFilter, OuterType);
    }
    if (OuterType == NPROT_8021Q_TAG_TYPE)
    {
        return Vlan && HaveInner && ndisprotFilterHasType(pFilter, InnerType);
    }
    return ndisprotFilterHasType(pFilter, OuterType);
}


BOOLEAN
ndisprotFilterAllowsSend(
    IN PNDISPROT_OPEN_CONTEXT       pOpenContext,
    IN const UCHAR *                pFrame,
    IN ULONG                        Length
    )
/*++

Routine Description:

    softpn: the write check. A frame may be sent if its EtherType (or, with SOFTPN_FILTER_VLAN,
    the type behind an inline 802.1Q tag) is in the filter of the handle. The caller checks the
    minimum length and the source MAC.

--*/
{
    SOFTPN_FILTER   Filter;
    USHORT          OuterType;
    BOOLEAN         HaveInner;
    USHORT          InnerType = 0;

    NPROT_ACQUIRE_LOCK(&pOpenContext->Lock, FALSE);
    Filter = pOpenContext->Filter;
    NPROT_RELEASE_LOCK(&pOpenContext->Lock, FALSE);

    OuterType = ndisprotReadEthType(pFrame + NPROT_ETH_TYPE_OFFSET);
    HaveInner = Length >= sizeof(NDISPROT_ETH_HEADER) + NPROT_8021Q_TAG_LEN;
    if (HaveInner)
    {
        InnerType = ndisprotReadEthType(pFrame + NPROT_ETH_TYPE_OFFSET + NPROT_8021Q_TAG_LEN);
    }
    return ndisprotFilterMatches(&Filter, OuterType, HaveInner, InnerType, FALSE);
}


VOID
NdisprotReceiveNetBufferLists(
    IN NDIS_HANDLE                  ProtocolBindingContext,
    IN PNET_BUFFER_LIST             pNetBufferLists,
    IN NDIS_PORT_NUMBER             PortNumber,
    IN ULONG                        NumberOfNetBufferLists,
    IN ULONG                        ReceiveFlags
    )
/*++

Routine Description:

    Protocol entry point called by NDIS if the driver below
    uses NDIS 6 net buffer list indications.

    softpn: every accepted frame is copied (the sample kept the miniport's net buffer list while
    a read was pending), so all indicated lists go back to the miniport at once. Frames are
    accepted by the EtherType filter of the handle; loopback frames and (with
    SOFTPN_FILTER_DROP_OWN) frames with the adapter's own source MAC are dropped. An 802.1Q tag
    the NIC stripped into Ieee8021QNetBufferListInfo is put back inline, so user mode sees the
    same bytes as on the wire.

Arguments:

    ProtocolBindingContext - pointer to open context
    pNetBufferLists - a list of the Net Buffer lists being indicated up.
    PortNumber - Port on which the Net Bufer list was received
    NumberOfNetBufferLists - the number of NetBufferLists in this indication
    ReceiveFlags - indicates whether the NetBufferLists can be pended in
                   the protocol driver.

Return Value:

--*/
{
    PNDISPROT_OPEN_CONTEXT  pOpenContext;
    PNET_BUFFER_LIST        pNetBufList;
    PNET_BUFFER             pNetBuf;
    PNET_BUFFER_LIST        pCopyNetBufList;
    PUCHAR                  pCopyBuf;
    PUCHAR                  pHeader;
    UCHAR                   HeaderStorage[sizeof(NDISPROT_ETH_HEADER) + NPROT_8021Q_TAG_LEN];
    ULONG                   TotalLength;
    ULONG                   HeaderLength;
    ULONG                   CopyLength;
    ULONG                   BytesCopied;
    NDIS_STATUS             Status;
    ULONG                   ReturnFlags = 0;
    BOOLEAN                 DispatchLevel;
    SOFTPN_FILTER           Filter;
    NDIS_NET_BUFFER_LIST_8021Q_INFO VlanInfo;
    BOOLEAN                 VlanOob;
    BOOLEAN                 HaveInner;
    USHORT                  OuterType;
    USHORT                  InnerType;
    USHORT                  Tci;

    UNREFERENCED_PARAMETER(PortNumber);
    UNREFERENCED_PARAMETER(NumberOfNetBufferLists);

    pOpenContext = (PNDISPROT_OPEN_CONTEXT)ProtocolBindingContext;
    NPROT_STRUCT_ASSERT(pOpenContext, oc);

    DispatchLevel = NDIS_TEST_RECEIVE_AT_DISPATCH_LEVEL(ReceiveFlags);
    if (DispatchLevel)
    {
        NDIS_SET_RETURN_FLAG(ReturnFlags, NDIS_RETURN_FLAGS_DISPATCH_LEVEL);
    }

    NPROT_ACQUIRE_LOCK(&pOpenContext->Lock, DispatchLevel);
    Filter = pOpenContext->Filter;
    NPROT_RELEASE_LOCK(&pOpenContext->Lock, DispatchLevel);

    if (Filter.EtherTypeCount != 0 &&
        pOpenContext->State != NdisprotPausing &&
        pOpenContext->State != NdisprotPaused)
    {
        for (pNetBufList = pNetBufferLists;
             pNetBufList != NULL;
             pNetBufList = NET_BUFFER_LIST_NEXT_NBL(pNetBufList))
        {
            pNetBuf = NET_BUFFER_LIST_FIRST_NB(pNetBufList);
            TotalLength = NET_BUFFER_DATA_LENGTH(pNetBuf);

            if (TotalLength < sizeof(NDISPROT_ETH_HEADER))
            {
                continue;   // runt
            }

            HeaderLength = TotalLength >= sizeof(HeaderStorage) ? sizeof(HeaderStorage)
                                                                 : sizeof(NDISPROT_ETH_HEADER);
            pHeader = NdisGetDataBuffer(pNetBuf, HeaderLength, HeaderStorage, 1, 0);
            if (pHeader == NULL)
            {
                continue;
            }

            VlanInfo.Value = NET_BUFFER_LIST_INFO(pNetBufList, Ieee8021QNetBufferListInfo);
            VlanOob = VlanInfo.Value != NULL;

            OuterType = ndisprotReadEthType(pHeader + NPROT_ETH_TYPE_OFFSET);
            HaveInner = HeaderLength == sizeof(HeaderStorage);
            InnerType = HaveInner ? ndisprotReadEthType(pHeader + NPROT_ETH_TYPE_OFFSET + NPROT_8021Q_TAG_LEN) : 0;

            if (!ndisprotFilterMatches(&Filter, OuterType, HaveInner, InnerType, VlanOob))
            {
                continue;
            }

            if (NdisTestNblFlag(pNetBufList, NDIS_NBL_FLAGS_IS_LOOPBACK_PACKET) ||
                ((Filter.Flags & SOFTPN_FILTER_DROP_OWN) != 0 &&
                 NPROT_MEM_CMP(pHeader + NPROT_MAC_ADDR_LEN, pOpenContext->CurrentAddress, NPROT_MAC_ADDR_LEN)))
            {
                InterlockedIncrement64((LONG64 *)&pOpenContext->Stats.RxDroppedOwn);
                continue;
            }

            InterlockedIncrement64((LONG64 *)&pOpenContext->Stats.RxAccepted);

            CopyLength = TotalLength + (VlanOob ? NPROT_8021Q_TAG_LEN : 0);
            pCopyNetBufList = ndisprotAllocateReceiveNetBufferList(pOpenContext, CopyLength, &pCopyBuf);
            if (pCopyNetBufList == NULL)
            {
                InterlockedIncrement64((LONG64 *)&pOpenContext->Stats.RxDroppedNoMemory);
                continue;
            }
            NBL_SET_PROT_RSVD_FLAG(pCopyNetBufList, NPROT_ALLOCATED_NBL);

            if (!VlanOob)
            {
                Status = NdisCopyFromNetBufferToNetBuffer(NET_BUFFER_LIST_FIRST_NB(pCopyNetBufList), 0,
                                                          TotalLength, pNetBuf, 0, &BytesCopied);
            }
            else
            {
                // Destination and source MAC, then the tag, then EtherType and payload.
                Status = NdisCopyFromNetBufferToNetBuffer(NET_BUFFER_LIST_FIRST_NB(pCopyNetBufList), 0,
                                                          NPROT_ETH_TYPE_OFFSET, pNetBuf, 0, &BytesCopied);
                if (Status == NDIS_STATUS_SUCCESS)
                {
                    Status = NdisCopyFromNetBufferToNetBuffer(NET_BUFFER_LIST_FIRST_NB(pCopyNetBufList),
                                                              NPROT_ETH_TYPE_OFFSET + NPROT_8021Q_TAG_LEN,
                                                              TotalLength - NPROT_ETH_TYPE_OFFSET,
                                                              pNetBuf, NPROT_ETH_TYPE_OFFSET, &BytesCopied);
                }
                Tci = (USHORT)((VlanInfo.TagHeader.UserPriority << 13) |
                               (VlanInfo.TagHeader.CanonicalFormatId << 12) |
                               VlanInfo.TagHeader.VlanId);
                pCopyBuf[NPROT_ETH_TYPE_OFFSET + 0] = (UCHAR)(NPROT_8021Q_TAG_TYPE >> 8);
                pCopyBuf[NPROT_ETH_TYPE_OFFSET + 1] = (UCHAR)(NPROT_8021Q_TAG_TYPE & 0xFF);
                pCopyBuf[NPROT_ETH_TYPE_OFFSET + 2] = (UCHAR)(Tci >> 8);
                pCopyBuf[NPROT_ETH_TYPE_OFFSET + 3] = (UCHAR)(Tci & 0xFF);
                InterlockedIncrement64((LONG64 *)&pOpenContext->Stats.RxVlanReinserted);
            }

            if (Status != NDIS_STATUS_SUCCESS)
            {
                DEBUGP(DL_FATAL, ("ReceiveNetBufferList: Open %p, failed to"
                    " copy the data, %d bytes\n", pOpenContext, TotalLength));
                ndisprotFreeReceiveNetBufferList(pOpenContext, pCopyNetBufList, DispatchLevel);
                InterlockedIncrement64((LONG64 *)&pOpenContext->Stats.RxDroppedNoMemory);
                continue;
            }

            //
            //  Queue this up and service any pending Read IRPs.
            //
            ndisprotQueueReceiveNetBufferList(pOpenContext, pCopyNetBufList, DispatchLevel);
        }
    }

    //
    //  We kept nothing: give the lists back if the miniport gave us ownership.
    //
    if (NDIS_TEST_RECEIVE_CAN_PEND(ReceiveFlags))
    {
        NdisReturnNetBufferLists(pOpenContext->BindingHandle,
                                 pNetBufferLists,
                                 ReturnFlags);
    }
}


VOID
ndisprotQueueReceiveNetBufferList(
    IN PNDISPROT_OPEN_CONTEXT        pOpenContext,
    IN PNET_BUFFER_LIST              pRcvNetBufList,
    IN BOOLEAN                       DispatchLevel
    )
/*++

Routine Description:

    Queue up a received net buffer list on the open context structure.
    If the queue size goes beyond a water mark, discard a Net Buffer list
    at the head of the queue.

    Finally, run the queue service routine.

Arguments:

    pOpenContext - pointer to open context
    pRcvPacket - the received packet
    DipatchLevel - the irql level

Return Value:

    None

--*/
{
    PLIST_ENTRY        pEnt;
    PLIST_ENTRY        pDiscardEnt;
    PNET_BUFFER_LIST   pDiscardNetBufList;

    do
    {

        NPROT_REF_OPEN(pOpenContext);    // queued rcv net buffer list

        NPROT_ACQUIRE_LOCK(&pOpenContext->Lock, DispatchLevel);

        if ((pOpenContext->State == NdisprotPaused)
            || (pOpenContext->State == NdisprotPausing))
        {
            NPROT_RELEASE_LOCK(&pOpenContext->Lock, DispatchLevel);

            ndisprotFreeReceiveNetBufferList(pOpenContext, pRcvNetBufList, DispatchLevel);

            NPROT_DEREF_OPEN(pOpenContext);  // softpn: the sample leaked this reference
            break;
        }

        //
        //  Check if the binding is in the proper state to receive
        //  this net buffer list.
        //
        if (NPROT_TEST_FLAGS(pOpenContext->Flags, NPROTO_BIND_FLAGS, NPROTO_BIND_ACTIVE) &&
            (pOpenContext->PowerState == NetDeviceStateD0))
        {

            //
            // Queue the net buffer list
            //
            pEnt = NPROT_RCV_NBL_TO_LIST_ENTRY(pRcvNetBufList);
            NPROT_INSERT_TAIL_LIST(&pOpenContext->RecvNetBufListQueue, pEnt);
            NPROT_RCV_NBL_FROM_LIST_ENTRY(pEnt) = pRcvNetBufList;
            pOpenContext->RecvNetBufListCount++;
            if (pOpenContext->RecvNetBufListCount > pOpenContext->Stats.QueueHighWater)
            {
                pOpenContext->Stats.QueueHighWater = pOpenContext->RecvNetBufListCount;
            }

            DEBUGP(DL_VERY_LOUD, ("QueueReceiveNetBufferList: open %p,"
                    " queued nbl %p, queue size %d\n",
                    pOpenContext, pRcvNetBufList, pOpenContext->RecvNetBufListCount));

        }
        else
        {
            //
            //  Received this net buffer list when the binding is going away.
            //  Drop this.
            //
            NPROT_RELEASE_LOCK(&pOpenContext->Lock, DispatchLevel);

            ndisprotFreeReceiveNetBufferList(pOpenContext, pRcvNetBufList, DispatchLevel);

            NPROT_DEREF_OPEN(pOpenContext);  // dropped rcv packet - bad state
            break;
        }


        //
        //  Trim the queue if it has grown too big.
        //
        if (pOpenContext->RecvNetBufListCount > MAX_RECV_QUEUE_SIZE)
        {
            //
            //  Remove the head of the queue.
            //
            pDiscardEnt = pOpenContext->RecvNetBufListQueue.Flink;
            NPROT_REMOVE_ENTRY_LIST(pDiscardEnt);

            pOpenContext->RecvNetBufListCount --;
            InterlockedIncrement64((LONG64 *)&pOpenContext->Stats.RxDroppedQueueFull);

            NPROT_RELEASE_LOCK(&pOpenContext->Lock, DispatchLevel);

            pDiscardNetBufList = NPROT_RCV_NBL_FROM_LIST_ENTRY(pDiscardEnt);

            NPROT_RCV_NBL_FROM_LIST_ENTRY(pDiscardEnt) = NULL;

            ndisprotFreeReceiveNetBufferList(pOpenContext, pDiscardNetBufList, DispatchLevel);

            NPROT_DEREF_OPEN(pOpenContext);  // dropped rcv packet - queue too long

            DEBUGP(DL_INFO, ("QueueReceiveNetBufferList: open %p queue"
                    " too long, discarded  %p\n",
                    pOpenContext, pDiscardNetBufList));
        }
        else
        {
            NPROT_RELEASE_LOCK(&pOpenContext->Lock, DispatchLevel);
        }

        //
        //  Run the receive queue service routine now.
        //
        ndisprotServiceReads(pOpenContext);
    }
    while (FALSE);
}

_Success_(return != 0)
PNET_BUFFER_LIST
ndisprotAllocateReceiveNetBufferList(
    _In_  PNDISPROT_OPEN_CONTEXT        pOpenContext,
    _In_  UINT                          DataLength,
    _Outptr_result_bytebuffer_(DataLength)
	      PUCHAR *                      ppDataBuffer
    )
/*++

Routine Description:

    Allocate resources to copy and queue a received net buffer list

Arguments:

    pOpenContext - pointer to open context for received packet
    DataLength - total length in bytes of the net buffer list's first net buffer
    ppDataBuffer - place to return pointer to allocated buffer

Return Value:

    Pointer to NDIS packet if successful, else NULL.

--*/
{
    PNET_BUFFER_LIST            pNetBufList;
    PMDL                        pMdl;
    PUCHAR                      pDataBuffer;

    pNetBufList = NULL;
    pMdl = NULL;
    pDataBuffer = NULL;
    *ppDataBuffer = NULL;

    do
    {
        NPROT_ALLOC_MEM(pDataBuffer, DataLength);

        if (pDataBuffer == NULL)
        {
            DEBUGP(DL_FATAL, ("AllocRcvNbl: open %p, failed to alloc"
                " data buffer %d bytes\n", pOpenContext, DataLength));
            break;
        }

        //
        //  Make this an NDIS buffer.
        //
        pMdl = NdisAllocateMdl(pOpenContext->BindingHandle, pDataBuffer, DataLength);

        if (pMdl == NULL)
        {
            DEBUGP(DL_FATAL, ("AllocateRcvNbl: open %p, failed to alloc"
                " MDL, %d bytes\n", pOpenContext, DataLength));
            break;
        }

        pNetBufList = NdisAllocateNetBufferAndNetBufferList(
                        pOpenContext->RecvNetBufferListPool,
                        0,                              // ContextSize
                        0,                              // ContextBackfill
                        pMdl,                    // MdlChain
                        0,                              // DataOffset
                        DataLength);                   // DataLength

        if (pNetBufList == NULL)
        {
            DEBUGP(DL_FATAL, ("AllocateRcvNbl: open %p, failed to alloc"
                " Net Buffer List, %d bytes\n", pOpenContext, DataLength));
            break;
        }


        *ppDataBuffer = pDataBuffer;

    }
    while (FALSE);

    if (pNetBufList == NULL)
    {
        //
        //  Clean up
        //
        if (pMdl != NULL)
        {
            NdisFreeMdl(pMdl);
        }

        if (pDataBuffer != NULL)
        {
            NPROT_FREE_MEM(pDataBuffer);
        }
    }

    return (pNetBufList);
}



VOID
ndisprotFreeReceiveNetBufferList(
    IN PNDISPROT_OPEN_CONTEXT        pOpenContext,
    IN PNET_BUFFER_LIST              pNetBufferList,
    IN BOOLEAN                       DispatchLevel
    )
/*++

Routine Description:

    Free up all resources associated with a received net buffer list. If this
    is a local copy, free the net buffer list to our receive pool, else return
    this to the miniport.

Arguments:

    pOpenContext - pointer to open context
    pNetBufferList - pointer to net buffer list to be freed.
    DipatchLevel - the irql level

Return Value:

    None

--*/
{
    PMDL                pMdl;
    UINT                TotalLength;
    UINT                BufferLength;
    PUCHAR              pCopyData = NULL;
    ULONG               ReturnFlags = 0;


    do
    {
        if (NBL_TEST_PROT_RSVD_FLAG(pNetBufferList, NPROT_ALLOCATED_NBL))
        {
            //
            //  This is a local copy.
            //

            pMdl = NET_BUFFER_FIRST_MDL(NET_BUFFER_LIST_FIRST_NB(pNetBufferList));
            TotalLength = NET_BUFFER_DATA_LENGTH(NET_BUFFER_LIST_FIRST_NB(pNetBufferList));

            NPROT_ASSERT(pMdl != NULL);

            NdisQueryMdl(
                pMdl,
                (PVOID *)&pCopyData,
                &BufferLength,
                NormalPagePriority | MdlMappingNoExecute);

            NPROT_ASSERT(BufferLength == TotalLength);


            NPROT_ASSERT(pCopyData != NULL); // we would have allocated non-paged pool


            NdisFreeNetBufferList(pNetBufferList);

            NdisFreeMdl(pMdl);

            NPROT_FREE_MEM(pCopyData);
            break;
        }
        //
        // The NetBufferList should be returned

        NET_BUFFER_LIST_NEXT_NBL(pNetBufferList) = NULL;

        if (DispatchLevel)
        {
            NDIS_SET_RETURN_FLAG(ReturnFlags, NDIS_RETURN_FLAGS_DISPATCH_LEVEL);
        }

        NdisReturnNetBufferLists(pOpenContext->BindingHandle,
                                  pNetBufferList,
                                  ReturnFlags);
    }
    while (FALSE);

}


VOID
ndisprotCancelPendingReads(
    IN PNDISPROT_OPEN_CONTEXT        pOpenContext
    )
/*++

Routine Description:

    Cancel any pending read IRPs queued on the given open.

Arguments:

    pOpenContext - pointer to open context

Return Value:

    None

--*/
{
    PIRP                pIrp;
    PLIST_ENTRY         pIrpEntry;

    NPROT_REF_OPEN(pOpenContext);  // temp ref - cancel reads

    NPROT_ACQUIRE_LOCK(&pOpenContext->Lock, FALSE);

    while (!NPROT_IS_LIST_EMPTY(&pOpenContext->PendedReads))
    {
        //
        //  Get the first pended Read IRP
        //
        pIrpEntry = pOpenContext->PendedReads.Flink;
        pIrp = CONTAINING_RECORD(pIrpEntry, IRP, Tail.Overlay.ListEntry);

        //
        //  Check to see if it is being cancelled.
        //
        if (IoSetCancelRoutine(pIrp, NULL))
        {
            //
            //  It isn't being cancelled, and can't be cancelled henceforth.
            //
            NPROT_REMOVE_ENTRY_LIST(pIrpEntry);

            NPROT_RELEASE_LOCK(&pOpenContext->Lock, FALSE);

            //
            //  Complete the IRP.
            //
            pIrp->IoStatus.Status = STATUS_CANCELLED;
            pIrp->IoStatus.Information = 0;

            DEBUGP(DL_INFO, ("CancelPendingReads: Open %p, IRP %p cancelled\n",
                pOpenContext, pIrp));

            IoCompleteRequest(pIrp, IO_NO_INCREMENT);

            NPROT_DEREF_OPEN(pOpenContext);    // took out pended Read for cancelling

            NPROT_ACQUIRE_LOCK(&pOpenContext->Lock, FALSE);
            pOpenContext->PendedReadCount--;
        }
        else
        {
            //
            //  It is being cancelled, let the cancel routine handle it.
            //
            NPROT_RELEASE_LOCK(&pOpenContext->Lock, FALSE);

            //
            //  Give the cancel routine some breathing space, otherwise
            //  we might end up examining the same (cancelled) IRP over
            //  and over again.
            //
            NdisMSleep(100000);    // 100 ms.

            NPROT_ACQUIRE_LOCK(&pOpenContext->Lock, FALSE);
        }
    }

    NPROT_RELEASE_LOCK(&pOpenContext->Lock, FALSE);

    NPROT_DEREF_OPEN(pOpenContext);    // temp ref - cancel reads
}


VOID
ndisprotFlushReceiveQueue(
    IN PNDISPROT_OPEN_CONTEXT            pOpenContext
    )
/*++

Routine Description:

    Free any receive packets queued up on the specified open

Arguments:

    pOpenContext - pointer to open context

Return Value:

    None

--*/
{
    PLIST_ENTRY         pRcvNetBufListEntry;
    PNET_BUFFER_LIST    pRcvNetBufList;

    NPROT_REF_OPEN(pOpenContext);  // temp ref - flushRcvQueue

    NPROT_ACQUIRE_LOCK(&pOpenContext->Lock, FALSE);

    while (!NPROT_IS_LIST_EMPTY(&pOpenContext->RecvNetBufListQueue))
    {
        //
        //  Get the first queued receive packet
        //
        pRcvNetBufListEntry = pOpenContext->RecvNetBufListQueue.Flink;
        NPROT_REMOVE_ENTRY_LIST(pRcvNetBufListEntry);

        pOpenContext->RecvNetBufListCount--;

        NPROT_RELEASE_LOCK(&pOpenContext->Lock, FALSE);

        pRcvNetBufList = NPROT_RCV_NBL_FROM_LIST_ENTRY(pRcvNetBufListEntry);
        NPROT_RCV_NBL_FROM_LIST_ENTRY(pRcvNetBufListEntry) = NULL;

        DEBUGP(DL_LOUD, ("FlushReceiveQueue: open %p, nbl %p\n",
            pOpenContext, pRcvNetBufList));

        ndisprotFreeReceiveNetBufferList(pOpenContext, pRcvNetBufList, FALSE);

        NPROT_DEREF_OPEN(pOpenContext);    // took out pended Read

        NPROT_ACQUIRE_LOCK(&pOpenContext->Lock, FALSE);
    }

    NPROT_RELEASE_LOCK(&pOpenContext->Lock, FALSE);

    NPROT_DEREF_OPEN(pOpenContext);    // temp ref - flushRcvQueue
}


