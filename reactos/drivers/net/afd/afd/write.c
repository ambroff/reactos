/* $Id: write.c,v 1.13 2004/11/30 04:49:50 arty Exp $
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/net/afd/afd/write.c
 * PURPOSE:          Ancillary functions driver
 * PROGRAMMER:       Art Yerkes (ayerkes@speakeasy.net)
 * UPDATE HISTORY:
 * 20040708 Created
 */
#include "afd.h"
#include "tdi_proto.h"
#include "tdiconn.h"
#include "debug.h"

NTSTATUS DDKAPI SendComplete
( PDEVICE_OBJECT DeviceObject,
  PIRP Irp,
  PVOID Context ) {
    NTSTATUS Status = Irp->IoStatus.Status;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PAFD_FCB FCB = (PAFD_FCB)Context;
    PLIST_ENTRY NextIrpEntry;
    PIRP NextIrp = NULL;
    PIO_STACK_LOCATION NextIrpSp;
    PAFD_SEND_INFO SendReq;
    PAFD_MAPBUF Map;
    UINT TotalBytesCopied = 0, SpaceAvail, i, CopySize = 0;

    AFD_DbgPrint(MID_TRACE,("Called, status %x, %d bytes used\n",
			    Irp->IoStatus.Status,
			    Irp->IoStatus.Information));

    ASSERT_IRQL(APC_LEVEL);

    if( !SocketAcquireStateLock( FCB ) ) return Status;

    FCB->SendIrp.InFlightRequest = NULL; 
    /* Request is not in flight any longer */

    if( FCB->State == SOCKET_STATE_CLOSED ) {
	SocketStateUnlock( FCB );
	DestroySocket( FCB );
	return STATUS_SUCCESS;
    }

    if( !NT_SUCCESS(Status) ) {
	/* Complete all following send IRPs with error */
	
	while( !IsListEmpty( &FCB->PendingIrpList[FUNCTION_SEND] ) ) {
	    NextIrpEntry = 
		RemoveHeadList(&FCB->PendingIrpList[FUNCTION_SEND]);
	    NextIrp = 
		CONTAINING_RECORD(NextIrpEntry, IRP, Tail.Overlay.ListEntry);
	    NextIrpSp = IoGetCurrentIrpStackLocation( NextIrp );
	    SendReq = NextIrpSp->Parameters.DeviceIoControl.Type3InputBuffer;

	    UnlockBuffers( SendReq->BufferArray,
			   SendReq->BufferCount,
			   FALSE );

	    NextIrp->IoStatus.Status = Status;
	    NextIrp->IoStatus.Information = 0;

	    IoCompleteRequest( NextIrp, IO_NETWORK_INCREMENT );
	}
	
	SocketStateUnlock( FCB );

	return STATUS_SUCCESS;
    }

    RtlMoveMemory( FCB->Send.Window,
		   FCB->Send.Window + FCB->Send.BytesUsed,
		   FCB->Send.BytesUsed - Irp->IoStatus.Information );
    FCB->Send.BytesUsed -= Irp->IoStatus.Information;

    if( !FCB->Send.BytesUsed &&
	!IsListEmpty( &FCB->PendingIrpList[FUNCTION_SEND] ) &&
	NT_SUCCESS(Status) ) {
	NextIrpEntry = 
	    RemoveHeadList(&FCB->PendingIrpList[FUNCTION_RECV]);
	NextIrp = 
	    CONTAINING_RECORD(NextIrpEntry, IRP, Tail.Overlay.ListEntry);
	NextIrpSp = IoGetCurrentIrpStackLocation( NextIrp );
	SendReq = NextIrpSp->Parameters.DeviceIoControl.Type3InputBuffer;
	Map = (PAFD_MAPBUF)(SendReq->BufferArray + SendReq->BufferCount);

	AFD_DbgPrint(MID_TRACE,("SendReq @ %x\n", SendReq));
	    
	SpaceAvail = FCB->Send.Size - FCB->Send.BytesUsed;
	    
	for( i = 0; FCB->Send.BytesUsed < FCB->Send.Content && 
		 i < SendReq->BufferCount; i++ ) {
	    Map[i].BufferAddress = 
		MmMapLockedPages( Map[i].Mdl, KernelMode );

	    CopySize = MIN( SpaceAvail, 
			    SendReq->BufferArray[i].len );

	    RtlCopyMemory( FCB->Send.Window + FCB->Send.BytesUsed,
			   Map[i].BufferAddress,
			   CopySize );
	    
	    MmUnmapLockedPages( Map[i].Mdl, KernelMode );
	    
	    FCB->Send.BytesUsed += CopySize;
	    TotalBytesCopied += CopySize;
	    SpaceAvail -= CopySize;
	}
    }
	
    /* Some data is still waiting */
    if( FCB->Send.BytesUsed ) {
	FCB->PollState &= ~AFD_EVENT_SEND;

	SocketCalloutEnter( FCB );

	Status = TdiSend( &FCB->SendIrp.InFlightRequest,
			  IrpSp->FileObject,
			  0,
			  FCB->Send.Window,
			  FCB->Send.BytesUsed,
			  &FCB->SendIrp.Iosb,
			  SendComplete,
			  FCB );

	SocketCalloutLeave( FCB );
    } else {
	FCB->PollState |= AFD_EVENT_SEND;
	PollReeval( FCB->DeviceExt, FCB->FileObject );
    }

    if( TotalBytesCopied > 0 ) {
	UnlockBuffers( SendReq->BufferArray, SendReq->BufferCount, FALSE );

	if( Status == STATUS_PENDING )
	    Status = STATUS_SUCCESS;

	AFD_DbgPrint(MID_TRACE,("Dismissing request: %x\n", Status));
	
	return UnlockAndMaybeComplete( FCB, Status, Irp, TotalBytesCopied, 
				       NULL, TRUE );
    } else if( NextIrp ) {
	AFD_DbgPrint(MID_TRACE,("Could not do any more with Irp %x\n",
				NextIrp));
	InsertHeadList( &FCB->PendingIrpList[FUNCTION_SEND],
			&Irp->Tail.Overlay.ListEntry );
    }

    SocketStateUnlock( FCB );
	
    return STATUS_SUCCESS;
}

NTSTATUS STDCALL
AfdConnectedSocketWriteData(PDEVICE_OBJECT DeviceObject, PIRP Irp, 
			    PIO_STACK_LOCATION IrpSp, BOOLEAN Short) {
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    PAFD_SEND_INFO SendReq;
    UINT TotalBytesCopied = 0, i, CopySize = 0, 
	SpaceAvail = 0, TotalBytesEncountered = 0;

    AFD_DbgPrint(MID_TRACE,("Called on %x\n", FCB));

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp, FALSE );
    
    FCB->EventsFired &= ~AFD_EVENT_SEND;

    if( !(SendReq = LockRequest( Irp, IrpSp )) ) 
	return UnlockAndMaybeComplete
	    ( FCB, STATUS_NO_MEMORY, Irp, TotalBytesCopied, NULL, FALSE );

    AFD_DbgPrint(MID_TRACE,("Socket state %d\n", FCB->State));

    if( FCB->State != SOCKET_STATE_CONNECTED ) {
	if( SendReq->AfdFlags & AFD_IMMEDIATE ) {
	    AFD_DbgPrint(MID_TRACE,("Nonblocking\n"));
	    return UnlockAndMaybeComplete
		( FCB, STATUS_CANT_WAIT, Irp, 0, NULL, TRUE );
	} else {
	    AFD_DbgPrint(MID_TRACE,("Queuing request\n"));
	    return LeaveIrpUntilLater( FCB, Irp, FUNCTION_SEND );
	}
    }

    AFD_DbgPrint(MID_TRACE,("We already have %d bytes waiting.\n", 
			    FCB->Send.BytesUsed));

    SendReq->BufferArray = LockBuffers( SendReq->BufferArray,
					SendReq->BufferCount,
					NULL, NULL,
					FALSE, FALSE );
    
    AFD_DbgPrint(MID_TRACE,("FCB->Send.BytesUsed = %d\n", 
			    FCB->Send.BytesUsed));

    if( !FCB->Send.BytesUsed ) {
	SpaceAvail = FCB->Send.Size - FCB->Send.BytesUsed;

	AFD_DbgPrint(MID_TRACE,("We can accept %d bytes\n",
				SpaceAvail));
	
	for( i = 0; FCB->Send.BytesUsed < FCB->Send.Size && 
		 i < SendReq->BufferCount; i++ ) {
	    CopySize = MIN( SpaceAvail, 
			    SendReq->BufferArray[i].len );

	    TotalBytesEncountered += SendReq->BufferArray[i].len;

	    AFD_DbgPrint(MID_TRACE,("Copying Buffer %d, %x:%d to %x\n",
				    i, 
				    SendReq->BufferArray[i].buf,
				    CopySize,
				    FCB->Send.Window + FCB->Send.BytesUsed));
	    
	    RtlCopyMemory( FCB->Send.Window + FCB->Send.BytesUsed,
			   SendReq->BufferArray[i].buf,
			   CopySize );
	    
	    FCB->Send.BytesUsed += CopySize;
	    TotalBytesCopied += CopySize;
	    SpaceAvail -= CopySize;
	}
	
	if( TotalBytesEncountered == 0 ) {
	    UnlockBuffers( SendReq->BufferArray, SendReq->BufferCount, FALSE );
	    
	    AFD_DbgPrint(MID_TRACE,("Empty send\n"));
	    return UnlockAndMaybeComplete
		( FCB, Status, Irp, TotalBytesCopied, NULL, TRUE );
	}
	
	AFD_DbgPrint(MID_TRACE,("Completed %d bytes\n", TotalBytesCopied));
	
	if( TotalBytesCopied > 0 ) {
	    UnlockBuffers( SendReq->BufferArray, SendReq->BufferCount, FALSE );

	    FCB->SendIrp.InFlightRequest = (PVOID)1; /* Placeholder */

	    SocketCalloutEnter( FCB );

	    Status = TdiSend( &FCB->SendIrp.InFlightRequest,
			      FCB->Connection.Object,
			      0,
			      FCB->Send.Window,
			      FCB->Send.BytesUsed,
			      &FCB->SendIrp.Iosb,
			      SendComplete,
			      FCB );
	    
	    SocketCalloutLeave( FCB );

	    if( Status == STATUS_PENDING )
		Status = STATUS_SUCCESS;
	    
	    AFD_DbgPrint(MID_TRACE,("Dismissing request: %x (%d)\n", 
				    Status, TotalBytesCopied));
	    
	    return UnlockAndMaybeComplete
		( FCB, Status, Irp, TotalBytesCopied, NULL, TRUE );
	}
    }

    if( SendReq->AfdFlags & AFD_IMMEDIATE ) {
	AFD_DbgPrint(MID_TRACE,("Nonblocking\n"));
	return UnlockAndMaybeComplete
	    ( FCB, STATUS_CANT_WAIT, Irp, 0, NULL, TRUE );
    } else {
	AFD_DbgPrint(MID_TRACE,("Queuing request\n"));
	return LeaveIrpUntilLater( FCB, Irp, FUNCTION_SEND );
    }
}

NTSTATUS DDKAPI PacketSocketSendComplete
( PDEVICE_OBJECT DeviceObject,
  PIRP Irp,
  PVOID Context ) {
    PAFD_FCB FCB = (PAFD_FCB)Context;

    AFD_DbgPrint(MID_TRACE,("Called, status %x, %d bytes used\n",
			    Irp->IoStatus.Status,
			    Irp->IoStatus.Information));

    /* It's ok if the FCB already died */
    if( !SocketAcquireStateLock( FCB ) ) return STATUS_SUCCESS;

    FCB->SendIrp.InFlightRequest = NULL; 
    /* Request is not in flight any longer */

    if( FCB->State == SOCKET_STATE_CLOSED ) {
	SocketStateUnlock( FCB );
	DestroySocket( FCB );
	return STATUS_SUCCESS;
    }

    SocketStateUnlock( FCB );

    return STATUS_SUCCESS;
}

NTSTATUS STDCALL
AfdPacketSocketWriteData(PDEVICE_OBJECT DeviceObject, PIRP Irp, 
			 PIO_STACK_LOCATION IrpSp) {
    NTSTATUS Status = STATUS_SUCCESS;
    PTDI_CONNECTION_INFORMATION TargetAddress;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    PAFD_SEND_INFO_UDP SendReq;
   
    AFD_DbgPrint(MID_TRACE,("Called on %x\n", FCB));

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp, FALSE );

    FCB->EventsFired &= ~AFD_EVENT_SEND;

    /* Check that the socket is bound */
    if( FCB->State != SOCKET_STATE_BOUND ) 
	return UnlockAndMaybeComplete
	    ( FCB, STATUS_UNSUCCESSFUL, Irp, 0, NULL, FALSE );
    if( !(SendReq = LockRequest( Irp, IrpSp )) ) 
	return UnlockAndMaybeComplete
	    ( FCB, STATUS_NO_MEMORY, Irp, 0, NULL, FALSE );
    
    AFD_DbgPrint
	(MID_TRACE,("RemoteAddress #%d Type %d\n", 
		    ((PTRANSPORT_ADDRESS)SendReq->RemoteAddress)->
		    TAAddressCount,
		    ((PTRANSPORT_ADDRESS)SendReq->RemoteAddress)->
		    Address[0].AddressType));

    TdiBuildConnectionInfo( &TargetAddress,
			    ((PTRANSPORT_ADDRESS)SendReq->RemoteAddress) );

    /* Check the size of the Address given ... */

    if( TargetAddress ) {
	SocketCalloutEnter( FCB );

	Status = TdiSendDatagram
	    ( &FCB->SendIrp.InFlightRequest,
	      FCB->AddressFile.Object,
	      SendReq->BufferArray[0].buf,
	      SendReq->BufferArray[0].len,
	      TargetAddress,
	      &FCB->SendIrp.Iosb,
	      PacketSocketSendComplete,
	      FCB );
    
	SocketCalloutLeave( FCB );

	ExFreePool( TargetAddress );
    } else Status = STATUS_NO_MEMORY;

    if( Status == STATUS_PENDING ) Status = STATUS_SUCCESS;

    AFD_DbgPrint(MID_TRACE,("Dismissing request: %x\n", Status));
    
    return UnlockAndMaybeComplete
	( FCB, Status, Irp, SendReq->BufferArray[0].len, NULL, TRUE );
}

