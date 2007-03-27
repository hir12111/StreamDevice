/***************************************************************
* Stream Device bus interface to the ESD CI488 CAN-GPIB module *
*                                                              *
* (C) 2000 Dirk Zimoch (zimoch@delta.uni-dortmund.de)          *
*                                                              *
* This interface should work with any CAN driver that supports *
* the functions from canBus.h                                  *
***************************************************************/

#include <devStream.h>
#include <canBus.h>
#include <stdlib.h>
#include <string.h>
#include <devLib.h>

long streamCanInitRecord (stream_t *stream, char *busName, char *linkString);
long streamCanWritePart (stream_t *stream, char* data, long length);
#define streamCanStartInput NULL
#define streamCanStopInput NULL
#define streamCanStartProtocol NULL
#define streamCanStopProtocol NULL

streamBusInterface_t streamCan = {
    streamCanInitRecord,
    streamCanWritePart,
    streamCanStartInput,
    streamCanStopInput,
    streamCanStartProtocol,
    streamCanStopProtocol,
    2048 /* number of channels (CAN IDs) */
};

typedef struct {
    void *busId;
    ushort_t RxId;
    ushort_t TxId;
    ushort_t bytesPerFrame;
} streamCanPrivate_t;

void streamCanMessage (stream_t *stream, canMessage_t *message);

/* Link Format:
   @protocolFile protocol busname TxId RxId [bytesPerFrame]

   protocolFile is the name of a file in STREAM_PROTOCOL_DIR
   protocol is one of the protocols in the protocolFile
   busname is one of the can busses
   TxId and RxId are CAN-IDs on the bus (0...0x7FF). The record writes its
   output to TxId and reads its input from RxId.
   bytesPerFrame is the max number of bytes sent in a frame (1...8, default 8)

   STREAM_PROTOCOL_DIR should be set in the startup file (default "/")
*/

long streamCanInitRecord (stream_t *stream, char *busName, char *linkString)
{
    streamCanPrivate_t *busPrivate;
    int status;

    busPrivate = (streamCanPrivate_t *) malloc (sizeof (streamCanPrivate_t));
    if (busPrivate == NULL)
    {
        printErr ("streamCanInitRecord %s: out of memory\n",
            stream->record->name);
        return S_dev_noMemory;
    }
    stream->busPrivate = busPrivate;
    status = canOpen(busName, &busPrivate->busId);
    if (status)
    {
        printErr ("streamCanInitRecord %s: can't open bus \"%s\"\n",
            stream->record->name, busName);
        return status;
    }
    switch (sscanf (linkString, "%hi%hi%hi",
        &busPrivate->TxId, &busPrivate->RxId,
        &busPrivate->bytesPerFrame))
    {
        case 2: busPrivate->bytesPerFrame = 8;
        case 3: break;
        default:
            printErr ("streamCanInitRecord %s: illegal link parameter '%s'\n",
                stream->record->name, linkString);
            return S_db_badField;
    }
    if (busPrivate->TxId > 0x7FF)
    {
        printErr ("streamCanInitRecord %s: TxId 0x%x out of range\n",
                stream->record->name, busPrivate->TxId);
        return S_can_badAddress;
    }
    if (busPrivate->RxId > 0x7FF)
    {
        printErr ("streamCanInitRecord %s: RxId 0x%x out of range\n",
                stream->record->name, busPrivate->RxId);
        return S_can_badAddress;
    }
    if (busPrivate->bytesPerFrame < 1 || busPrivate->bytesPerFrame > 8)
    {
        printErr ("streamCanCanInitRecord %s: %d bytes per frame"
            " out of range\n",
            stream->record->name, busPrivate->bytesPerFrame);
        return S_db_badField;
    }    
    stream->channel = busPrivate->TxId;
    /* Register the message handlers with the Canbus driver */
    canMessage(busPrivate->busId, busPrivate->TxId,
        (canMsgCallback_t *) streamCanMessage, stream);
    canMessage(busPrivate->busId, busPrivate->RxId,
        (canMsgCallback_t *) streamCanMessage, stream);
    return OK;
}

long streamCanWritePart (stream_t *stream, char* data, long length)
{
    canMessage_t message;
    streamCanPrivate_t *busPrivate = (streamCanPrivate_t *)stream->busPrivate;

    if (stream->protocol.writeTimeout)
    {
        devStreamStartTimer (stream, stream->protocol.writeTimeout,
            (FUNCPTR) devStreamWriteTimeout);
    }
    message.identifier = busPrivate->TxId;
    message.rtr = SEND;
    if (length > busPrivate->bytesPerFrame) {
        length = busPrivate->bytesPerFrame;
    }
    memcpy (message.data, data, length);
    message.length = length;
    #ifdef DEBUG
    printErr ("streamCanWritePart %s: id=%#x, length=%d, data=%08x %08x\n",
        stream->record->name, message.identifier, message.length,
        *(ulong_t *)message.data, *(ulong_t *)(message.data+4));
    #endif
    if (canWrite (busPrivate->busId, &message,
        stream->protocol.writeTimeout) != OK)
    {
        #ifdef DEBUG
        printErr ("streamCanWritePart %s: canWrite failed\n",
            stream->record->name);
        #endif
        return ERROR;
    }
    #ifdef DEBUG
    printErr ("streamCanWritePart %s: %d of %d bytes written\n",
        stream->record->name, stream->processed, stream->length);
    #endif
    return length;
}

/* interrupt level ! */
void streamCanMessage (stream_t *stream, canMessage_t *message)
{
    streamCanPrivate_t *busPrivate = (streamCanPrivate_t *)stream->busPrivate;
    int endFlag = FALSE;
    
    if (stream->haveBus &&
        busPrivate->TxId == message->identifier &&
        message->rtr == SEND)
    {
        #ifdef DEBUG
        logMsg ("streamCanMessage %s: %d bytes sent %08x %08x\n",
            stream->record->name, message->length,
            *(ulong_t *)message->data, *(ulong_t *)(message->data+4));
        #endif
        devStreamWriteFinished (stream);
        return;
    }
    if (stream->acceptEvent &&
        busPrivate->TxId == message->identifier &&
        message->rtr == RTR)
    {
        #ifdef DEBUG
        logMsg ("streamCanMessage %s: got remote frame\n",
            stream->record->name);
        #endif
        devStreamEvent (stream);
        return;
    }
    if (stream->acceptInput &&
        busPrivate->RxId == message->identifier &&
        message->rtr == SEND)
    {
        #ifdef DEBUG
        logMsg ("streamCanMessage %s: %d bytes received %08x %08x\n",
            stream->record->name, message->length,
            *(ulong_t *)message->data, *(ulong_t *)(message->data+4));
        #endif
        if (stream->protocol.inTerminator[0] == 0 && 
            stream->protocol.readTimeout == 0)
        {
            /* No terminator and no timeout, thus read only one frame */
            endFlag = TRUE;
        }
        else
        {
            if (stream->protocol.readTimeout != 0)
            {
                #ifdef DEBUG
                logMsg ("streamCanMessage %s: read watchdog start %d ms\n",
                    stream->record->name, stream->protocol.readTimeout);
                #endif
                devStreamStartTimer (stream, stream->protocol.readTimeout,
                    (FUNCPTR) devStreamReadTimeout);
            }
        }
        devStreamReceive (stream,
            (char *)message->data, message->length, endFlag);
    }
}
