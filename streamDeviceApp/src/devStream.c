/***************************************************************
* Stream Device Support                                        *
*                                                              *
* (C) 1999 Dirk Zimoch (zimoch@delta.uni-dortmund.de)          *
*                                                              *
* This is the kernel of the Stream Device. Please refer to the *
* HTML files in ../doc/ for a detailed documentation.          *
*                                                              *
* Please do not redistribute this file after you have changed  *
* something. If there is a bug or missing features, send me an *
* email.                                                       *
***************************************************************/

#include <devStream.h>

#include <stdlib.h>
#include <string.h>
#include <sysSymTbl.h>
#include <limits.h>
#include <math.h>
#include <ctype.h>
#include <wdLib.h>

#include <devLib.h>
#include <recSup.h>

#define PHASE_INIT 0
#define PHASE_REPLY 1
#define PHASE_ASYNC 2
#define PHASE_GOT_VALUE  3

/*
* PHO - 7/2/05 switch out debugging messages
*/
int devStreamDebug = FALSE;

/* is there no header file that declares symFindByNameEPICS? */
STATUS symFindByNameEPICS( SYMTAB_ID symTblId,
	char      *name,
	char      **pvalue,
	SYM_TYPE  *pType    );

void logMsg (const char*format, ...);

typedef struct streamPrivate_s {
    stream_t public;
    streamBusInterface_t *interface;
    IOSCANPVT ioscanpvt;
    WDOG_ID wdId;
    uchar_t *ioBuffer;
    uchar_t *codeIndex;
    uchar_t *activeCommand;
    struct streamPrivate_s **lockQueue;
    long (* readData) (dbCommon *, format_t *);
    long (* writeData) (dbCommon *, format_t *);
    struct streamPrivate_s *lockNext;
    int lockCancelled;
    int phase;
    int convert;
    int status;
    long length;
    long processed;
} streamPrivate_t;

typedef struct busEntry_s {
    struct busEntry_s *next;
    streamBusInterface_t *interface;
    streamPrivate_t **lockQueue;
    char name [8];
} busEntry_t;

LOCAL char *devStreamProtocolDir = NULL;
LOCAL busEntry_t *busList = NULL;
LOCAL SEM_ID lockSem;

LOCAL void process (streamPrivate_t *stream);
LOCAL void callback (streamPrivate_t *stream);
LOCAL void replyTimeout (streamPrivate_t *stream);
LOCAL void lockTimeout (streamPrivate_t *stream);
LOCAL long startIo (streamPrivate_t *stream);
LOCAL long nextIo (streamPrivate_t *stream);
LOCAL void busLockRequest (streamPrivate_t *stream);
LOCAL void busUnlock (streamPrivate_t *stream);

LOCAL void callbackRequestWrapper (CALLBACK *cb, char *caller)
{
    cb->user = caller;
#ifdef DEBUG
    printErr ("callbackRequest %p, prio %d, function %p, by %s\n",
        cb, cb->priority, cb->callback, cb->user);
#endif
    callbackRequest (cb);
}
#define callbackRequest(cb) callbackRequestWrapper (cb, __FUNCTION__)

/* Here come EPICS device support functions */

long devStreamInit (int after)
{
    char *symbol;
    SYM_TYPE type;

    if (after || devStreamProtocolDir != NULL) return OK;
    if (symFindByName (sysSymTbl, "STREAM_PROTOCOL_DIR", &symbol, &type) == OK)
    {
        devStreamProtocolDir = *(char **)symbol;
    }
    else
    {
        printErr ("devStreamInit: symbol STREAM_PROTOCOL_DIR not found\n");
        devStreamProtocolDir = "";
    }
    lockSem = semMCreate (SEM_Q_PRIORITY);
    return OK;
}    

/* use devStreamWrite in output records */
long devStreamWrite (dbCommon *record)
{
    streamPrivate_t *stream = (streamPrivate_t *) record->dpvt;


    if (devStreamDebug)
        printErr ("devStreamWrite %s: pact=%d task=%s caller=%s\n",
            record->name, record->pact, taskName (0), stream->public.callback.user);

    if (stream == NULL)
    {
        recGblSetSevr(record, UDF_ALARM, INVALID_ALARM);
        record->pact = TRUE;
        if (devStreamDebug)
            printErr ("devStreamWrite %s: not initialized; record suspended\n",
                record->name);
        return ERROR;
    }
    if (stream->status != NO_ALARM)
    {
        recGblSetSevr(record, stream->status, INVALID_ALARM);
        return ERROR;
    }
    if (record->pact == TRUE)
    {
        return OK;
    }
    if (record->scan != SCAN_IO_EVENT)
    {
        return startIo (stream);
    }
    return OK;
}

/* use devStreamRead in input records */
long devStreamRead (dbCommon *record)
{
    streamPrivate_t *stream = (streamPrivate_t *) record->dpvt;

    if (devStreamDebug)
        printErr ("devStreamRead %s: pact=%d\n",
            record->name, record->pact);

    if (stream == NULL)
    {
        recGblSetSevr(record, UDF_ALARM, INVALID_ALARM);
        record->pact = TRUE;
        if (devStreamDebug)
            printErr ("devStreamRead %s: not initialized; record suspended\n",
                record->name);
        return ERROR;
    }
    if (stream->status != NO_ALARM)
    {
        recGblSetSevr(record, stream->status, INVALID_ALARM);
        return ERROR;
    }
    if (record->pact == TRUE || record->scan == SCAN_IO_EVENT)
    {
        return stream->convert;
    }
    return startIo (stream);
}

long devStreamGetIointInfo (int cmd, dbCommon *record, IOSCANPVT *ppvt)
{
    streamPrivate_t *stream = (streamPrivate_t *) record->dpvt;

    if (stream == NULL) return ERROR;
    if (stream->ioscanpvt == NULL) scanIoInit(&stream->ioscanpvt);
    *ppvt = stream->ioscanpvt;
    if (cmd == 0)
    {
        /* record has been added to I/O event scanner */
        startIo (stream);

        if (devStreamDebug)
            printErr ("devStreamGetIointInfo %s: IO started\n",
                record->name);
    }
    else
    {
        /* record has been deleted from I/O event scanner */
        devStreamReadTimeout (&stream->public);

        if (devStreamDebug)
            printErr ("devStreamGetIointInfo %s: IO terminated\n",
                record->name);
    }
    return OK;
}

/* The following functions are record interface. Their prototypes will not
   change in future releases.
*/

/* Call this within your InitRecord device support function 
   Give it the ioLink (INP or OUT) and your record interface readData and
   writeData functions. They will be called whenever a format description
   is found in an in or out command.
*/

long devStreamInitRecord (dbCommon *record, struct link *ioLink,
    long (* readData) (dbCommon *, format_t *),
    long (* writeData) (dbCommon *, format_t *))
{
    streamPrivate_t *stream;
    char *linkString = ioLink->value.instio.string;
    char **busType;
    FILE *file;
    busEntry_t *busEntry;
    int errorLine;
    int i;
    int status;
    SYM_TYPE type;
    char path [PATH_MAX+1];
    char searchName [20];
    char fileName [80];
    char protocolName [80];
    char busName [80];
     
    record->dpvt = NULL;
    stream = (streamPrivate_t *) malloc (sizeof (streamPrivate_t));
    if (stream == NULL)
    {
        printErr ("devStreamInitRecord %s: out of memory\n",
            record->name);
        return S_dev_noMemory;
    }
    stream->public.record = record;
    stream->public.channel = 0xFFFFFFFF;
    stream->public.busPrivate = NULL;
    stream->public.haveBus = FALSE;
    stream->public.acceptInput = FALSE;
    stream->public.acceptEvent = FALSE;
    stream->lockNext = NULL;
    stream->ioscanpvt = NULL;
    stream->status = NO_ALARM;
    stream->readData = readData;
    stream->writeData = writeData;
    stream->length = 0;
    stream->processed = 0;
    stream->convert = 0;
    stream->phase = (record->scan == SCAN_IO_EVENT) ?
        PHASE_ASYNC : PHASE_INIT;
    
    /* parse the link for: "@filename protocol bus"*/
    if (ioLink->type != INST_IO)
    {
        printErr ("devStreamInitRecord %s: bad link type %s\n",
            record->name, pamaplinkType[ioLink->type]);
        return S_db_badField;
    }
    if (sscanf (linkString, "%s%s%s%n",
        fileName, protocolName, busName, &i) != 3)
    {
        printErr ("devStreamInitRecord %s: error parsing link \"%s\"\n",
            record->name, linkString);
        return S_db_badField;
    }
    linkString += i;
    while (isspace((uchar_t) *linkString)) linkString++;

    /* search the protocol */
    if (sprintf (path, "%s/%s", devStreamProtocolDir, fileName) > PATH_MAX)
    {
        printErr ("devStreamInitRecord %s: path \"%s\" too long\n",
            record->name, path);
        return S_db_badField;
    }
    #ifdef DEBUG
    printErr ("devStreamInitRecord %s: reading config file \"%s\"\n",
        record->name, path);
    #endif
    file = fopen (path, "r");
    if (file == NULL)
    {
        printErr ("devStreamInitRecord %s: can't open config file"
            " \"%s\" (%s)\n", record->name, path, strerror (errno));
        return errno;
    }
    errorLine = devStreamParseProtocol (&stream->public.protocol,
        file, protocolName);
    fclose (file);
    switch (errorLine)
    {
        case OK:
            break;
        case ERROR:
            printErr ("devStreamInitRecord %s: protocol \"%s\""
                " not found in config file \"%s\"\n",
                record->name, protocolName, path);
            return ERROR;
        case -2:
             printErr ("devStreamInitRecord %s: out of memory"
                " during config file parse\n", record->name);
            return S_dev_noMemory;
        default:
            printErr ("devStreamInitRecord %s: parse error in config file"
                " \"%s\" line %d\n", record->name, path, errorLine);
            return ERROR;
    }
    stream->ioBuffer = (uchar_t *) malloc(stream->public.protocol.bufferSize);
    if (stream->ioBuffer == NULL)
    {
        printErr ("devStreamInitRecord %s: out of memory\n",
            record->name);
        return S_dev_noMemory;
    }
    
    /* search for the bus */
    for (busEntry = busList; busEntry != NULL; busEntry = busEntry->next)
    {
        if (strncmp (busEntry->name, busName, 8) == 0) break;
    }
    if (busEntry == NULL)
    {
        /* found an unused bus */
        #ifdef DEBUG
        printErr ("devStreamInitRecord %s: new bus %s\n",
            record->name, busName);
        #endif
        busEntry = (busEntry_t *) malloc (sizeof (busEntry_t));
        if (busEntry == NULL)
        {
            printErr ("devStreamInitRecord %s: out of memory\n",
                record->name);
            return S_dev_noMemory;
        }
        busEntry->next = busList;
        strncpy (busEntry->name, busName, 8);
        busEntry->interface = NULL;
        busEntry->lockQueue = NULL;
        busList = busEntry;
        sprintf (searchName, "%.8s_streamBus", busName);
        if (symFindByName (sysSymTbl, searchName,
            (char **)&busType, &type) != OK)
        {
            printErr ("devStreamInitRecord %s: bus interface for bus"
                " \"%s\" not found\n", record->name, busName);
            return S_db_badField;
        }
        sprintf (searchName, "_stream%.8s", *busType);
        if (symFindByNameEPICS (sysSymTbl, searchName,
            (char **) &busEntry->interface, &type) != OK)
        {
            printErr ("devStreamInitRecord %s: interface \"%s\" for bus \"%s\""
                " not found\n", record->name, searchName, busName);
            return S_db_badField;
        }
        busEntry->lockQueue = (streamPrivate_t **) malloc (
            busEntry->interface->nChannels * sizeof (streamPrivate_t *));
        if (busEntry->lockQueue == NULL)
        {
            printErr ("devStreamInitRecord %s: out of memory\n",
                record->name);
            return S_dev_noMemory;
        }
        for (i = 0; i < busEntry->interface->nChannels; i++)
        {
            busEntry->lockQueue[i] = NULL;
        }
    }
    if (busEntry->interface == NULL)
    {
        printErr ("devStreamInitRecord %s: uninitialized bus \"%s\"\n",
            record->name, busName);
        return S_db_badField;
    }
    stream->interface = busEntry->interface;
    stream->lockQueue = busEntry->lockQueue;
    
    /* Create watchdog for timeouts */
    stream->wdId = wdCreate();
    if (stream->wdId == NULL)
    {
        printErr ("devStreamInitRecord %s: out of memory\n",
            record->name);
        return S_dev_noMemory;
    }
    
    /* Create a callback for async processing */
    callbackSetCallback ((CALLBACKFUNC) callback, &stream->public.callback);
    callbackSetPriority (record->prio, &stream->public.callback);
    callbackSetUser ("NONE", &stream->public.callback);
   
    /* Call bus dependend init procedure */
    record->dpvt = stream;
    status = stream->interface->initRecord (&stream->public,
        busName, linkString);
    if (status != OK)
    {
        record->dpvt = NULL;
        return status;
    }
    if (stream->public.channel >= stream->interface->nChannels)
    {
        printErr ("devStreamInitRecord %s: invalid channel number %d\n",
            record->name, stream->public.channel);
        record->dpvt = NULL;
        return status;
    }
    #ifdef DEBUG
    printErr ("devStreamInitRecord %s complete\n", record->name);
    #endif
    return OK;
}

/* Call this in your record interface function dataRead. The last argument
   is the address of the long, double or char[] where to put the read value
   (e.g. a record field). The data type depends on the format.
*/
long devStreamScanf (dbCommon *record, format_t *format, void *value)
{
    streamPrivate_t *stream = (streamPrivate_t *) record->dpvt;
    char *source = (char *) stream->ioBuffer + stream->processed;
    int len = -1;
    int width = 0;
    long raw;
    long shift;
    int match;
    unsigned char bcd1, bcd10;
    int status;

    switch (format->conversion)
    {
        case 'd':
        case 'i':
        case 'o':
        case 'u':
        case 'x':
        case 'X':
            if (format->flags & FORMAT_FLAG_SKIP)
                status = sscanf (source, format->string, &len);
            else
            {
                status = sscanf (source, format->string, (long *) value, &len);
                if (status != 1)
                    if (devStreamDebug)
                        printErr ("devStreamScanf %s: can't find long in \"%s\"\n",
                            record->name, source);
            }
            break;
        case 'f':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
            if (format->flags & FORMAT_FLAG_SKIP)
                status = sscanf (source, format->string, &len);
            else
            {
                status = sscanf (source, format->string, (double *) value, &len);
                if (status != 1)
                    if (devStreamDebug)
                        printErr ("devStreamScanf %s: can't find double in \"%s\"\n",
                            record->name, source);

            }
            break;
        case 'c':
        case 's':
        case '[':
            len = 0;
            if (format->flags & FORMAT_FLAG_SKIP)
                status = sscanf (source, format->string, &len);
            else
            {
                /* this is dangerous if width is not given or too big! */
                status = sscanf (source, format->string, (char *) value, &len);
                ((char *) value)[len] = 0;
                if (status != 1)
                    if (devStreamDebug)
                        printErr ("devStreamScanf %s: can't find string in \"%s\"\n",
                            record->name, source);
            }
            break;
        case 'b':
            /* binary ASCII input */
            raw = 0;
            width = format->width;
            if (width == 0) width = -1;
            while (isspace ((uchar_t) source[++len]));
            if (source[len] != '0' && source[len] != '1')
            {
                stream->status = CALC_ALARM;
                return ERROR;
            }
            while (width-- && (source[len] == '0' || source[len] == '1'))
            {
                raw <<= 1;
                if (source[len++] == '1') raw |= 1;
            }
            if (!(format->flags & FORMAT_FLAG_SKIP))
                *(long *) value = raw; 
            break;
        case '{':
            /* enumerated strings */
            len = 0;
            raw = 0;
            match = 1;
            while (*++stream->codeIndex != '}')
            {
                switch (*stream->codeIndex)
                {
                    case IN:
                    case OUT:
                    case NULL:
                        if (devStreamDebug)
                            printErr ("devStreamScanf %s: unterminated"
                                " %%{ format\n", record->name);
                        stream->status = SOFT_ALARM;
                        return ERROR;
                    case '|':
                        if (match == 0)
                        {
                            /* try next string */
                            raw++;
                            len = 0;
                        }
                        match++;
                        break;
                    case SKIP:
                        if (match == 1) len++;
                        break;
                    case ESC:
                        ++stream->codeIndex;
                    default:
                        if (match == 1 &&
                            (uchar_t) source[len++] != *stream->codeIndex)
                        {
                            /* mismatch */
                            match = 0;
                        }
                }
            }
            if (match == 0)
            {
                stream->status = CALC_ALARM;
                return ERROR;
            }
            if (!(format->flags & FORMAT_FLAG_SKIP))
                *(long *) value = raw; 
            break;
        case 'r':
            /* raw input */
            len = 0;
            raw = 0;
            width = format->width;
            if (width == 0) width = 1;
            if (format->flags & FORMAT_FLAG_SKIP)
            {
                len = width;
                break;
            }
            if (format->flags & FORMAT_FLAG_ALT)
            {
                /* little endian (sign extended)*/
                shift = 0;
                while (--width)
                {
                    raw |= (uchar_t) source[len++] << shift;
                    shift += 8;
                }
                raw |= ((signed char *) source)[len++] << shift;
            }
            else
            {
                /* big endian (sign extended)*/
                raw = ((signed char *) source)[len++];
                while (--width)
                {
                    raw = (raw << 8) | (uchar_t) source[len++];
                }
            }
            *(long *) value = raw;
            break;
        case 'D':
            /* packed BCD input */
            len = 0;
            raw = 0;
            width = format->width;
            if (width == 0) width = 1;
            if (format->flags & FORMAT_FLAG_ALT)
            {
                /* little endian */
                shift = 1;
                while (width--)
                {
                    bcd1 = bcd10 = (uchar_t) source[len++];
                    bcd1 &= 0x0F;
                    bcd10 /= 16;
                    if (bcd1 > 9 || shift * bcd1 < bcd1)
                    {
                        stream->status = CALC_ALARM;
                        return ERROR;
                    }
                    if (width == 0 && format->flags & FORMAT_FLAG_SIGN)
                    {
                        raw += bcd1 * shift;
                        if (bcd10 != 0) raw = -raw;
                        break;
                    }
                    if (bcd10 > 9 || shift * bcd10 < bcd10)
                    {
                        stream->status = CALC_ALARM;
                        return ERROR;
                    }
                    raw += (bcd1 + 10 * bcd10) * shift;
                    if (shift <= 100000000) shift *= 100;
                    else shift = 0;
                }
            }
            else
            {
                /* big endian */
                match = 0;
                if (format->flags & FORMAT_FLAG_SIGN && source[len] & 0xF0)
                {
                    match = 1;
                    source[len] &= 0x0F;
                }
                while (width--)
                {
                    long temp;
                    
                    bcd1 = bcd10 = (uchar_t) source[len++];
                    bcd1 &= 0x0F;
                    bcd10 /= 16;
                    if (bcd1 > 9 || bcd10 > 9)
                    {
                        stream->status = CALC_ALARM;
                        return ERROR;
                    }
                    temp = raw * 100 + (bcd1 + 10 * bcd10);
                    if (temp < raw)
                    {
                        stream->status = CALC_ALARM;
                        return ERROR;
                    }
                    raw = temp;
                }
                if (match) raw = -raw; 
            }
            if (!(format->flags & FORMAT_FLAG_SKIP))
                *(long *) value = raw; 
            break;
        default:
            if (devStreamDebug)
                printErr ("devStreamScanf %s: unimplemented format %%%c\n",
                    record->name, format->conversion);
            stream->status = SOFT_ALARM;
            return ERROR;
    }
    if (len < 0)
    {
        stream->status = CALC_ALARM;
        return ERROR;
    }
    stream->processed += len;
    return OK;
}

/* Call this in your record interface function dataWrite. The last argument
   is the long, double or char* value to write (e.g. the value of a record
   field). The data type depends on the format.
*/
long devStreamPrintf (dbCommon *record, format_t *format, ...)
{
    streamPrivate_t *stream = (streamPrivate_t *) record->dpvt;
    char *buffer = (char *) stream->ioBuffer + stream->length;
    int buffersize = stream->public.protocol.bufferSize - stream->length;
    int len = -1;
    int width = 0;
    int prec;
    int fill;
    long raw;
    char *string;
    long i;
    va_list value;
    uchar_t bcd[6]={0,0,0,0,0,0};

    va_start (value, format);
    switch (format->conversion)
    {
        case 'c':
        case 'd':
        case 'i':
        case 'o':
        case 'u':
        case 'x':
        case 'X':
            width = 12;
            if (format->width > width) width = format->width;
            if (width >= buffersize) break;
            sprintf (buffer, format->string, va_arg (value, long), &len);
            break;
        case 'f':
            width = log10 (*(double *) value);
        case 'e':
        case 'E':
        case 'g':
        case 'G':
            width += format->prec + 8;
            if (format->width > width) width = format->width;
            if (width >= buffersize) break;
            sprintf (buffer, format->string, va_arg (value, double), &len);
            break;
        case 's':
            string = va_arg (value, char *);
            width = strlen (string);
            if (format->prec > 0 && format->prec < width) width = format->prec;
            if (format->width > width) width = format->width;
            if (width >= buffersize) break;
            sprintf (buffer, format->string, string, &len);
            break;
        case 'b':
            /* binary ASCII output */
            raw = va_arg (value, long);
            len = 0;
            prec = format->prec;
            if (prec == -1)
            {
                prec = sizeof (long) * 8;
                while (prec && (raw & (1 << (prec - 1))) == 0) prec--;
            }
            if (prec == 0) prec++;
            width = prec;
            if (format->width > width) width = format->width;
            if (width >= buffersize) break;
            if (!(format->flags & FORMAT_FLAG_LEFT))
            {
                if (format->flags & FORMAT_FLAG_ZERO) fill = '0';
                else fill = ' ';
                while (len < width - prec)
                {
                    buffer[len++] = fill;
                }
            }
            while (prec--)
            {
                buffer[len++] = (raw & (1 << prec)) ? '1' : '0';
            }
            while (len < width)
            {
                buffer[len++] = ' ';
            }
            buffer[len] = 0;
            break;
        case 'r':
            /* raw output */
            raw = va_arg (value, long);
            len = 0;
            prec = format->prec;
            if (prec == -1) prec = 1;
            width = prec;
            if (format->width > width) width = format->width;
            if (width >= buffersize) break;
            if (format->flags & FORMAT_FLAG_ALT)
            {
                /* little endian */
                while (prec--)
                {
                    buffer[len++] = raw & 0xFF;
                    raw >>= 8;
                }
                fill = (buffer[-1] & 0x80) ? 0xFF : 0x00;
                while (len < width)
                {
                    buffer[len++] = fill;
                }
            }
            else
            {
                /* big endian */
                fill = ((raw >> (8 * (prec-1))) & 0x80) ? 0xFF : 0x00;
                while (len < width - prec)
                {
                    buffer[len++] = fill;
                }
                while (prec--)
                {
                    buffer[len++] = (raw >> (8 * prec)) & 0xFF;
                }
            }
            break;
        case 'D':
            /* packed BCD output (prec = number of nibbles) */
            raw = va_arg (value, long);
            prec = format->prec;
            if (prec == -1)
            {
                prec = 2 * sizeof (raw);
            }
            width = (prec + (format->flags & FORMAT_FLAG_SIGN ? 2 : 1)) / 2;
            if (format->width > width) width = format->width;
            if (width >= buffersize) break;
            if (format->flags & FORMAT_FLAG_SIGN && raw < 0)
            {
                /* negative BCD value, use MSB as sign */
                bcd[5] = 0x80;
                raw = - raw;
            }
            if (prec > 10) prec = 10;
            for (i = 0; i < prec; i++)
            {
                bcd[i/2] |= raw % 10 << (4 * (i & 1));
                raw /= 10;
            }            
            if (format->flags & FORMAT_FLAG_ALT)
            {
                /* little endian */
                for (i = 0; i < (prec + 1) / 2; i++)
                {
                    buffer[len++] = bcd[i];
                }
                while (len < width)
                {
                    buffer[len++] = 0;
                }
                buffer[len-1] |=  bcd[5];
            }
            else
            {
                /* big endian */
                while (len < width - (prec + 1) / 2)
                {
                    buffer[len++] = 0;
                }
                for (i = (prec - 1) / 2; i >= 0; i--)
                {
                    buffer[len++] = bcd[i];
                }
                buffer[0] |= bcd[5];
            }
            break;
        case '{':
            /* enumerated strings */
            raw = va_arg (value, long);
            width = 0;
            i = 0;
            while (*++stream->codeIndex != '}')
            {
                switch (*stream->codeIndex)
                {
                    case IN:
                    case OUT:
                    case NULL:
                        if (devStreamDebug)
                            printErr ("devStreamPrintf %s: unterminated"
                                " %%{ format\n", record->name);
                        stream->status = SOFT_ALARM;
                        return ERROR;
                    case '|':
                        if (raw == i)
                        {
                            buffer[width] = 0;
                        }
                        i++;
                        break;
                    case ESC:
                        ++stream->codeIndex;
                    default:
                        if (raw == i)
                        {
                            buffer[width++] = *stream->codeIndex;
                            if (width >= buffersize) break;
                        }
                }
            }
            len = width;
            break;
        default:
            if (devStreamDebug)
                printErr ("devStreamPrintf %s: unimplemented format %%%c\n",
                    record->name, format->conversion);
            stream->status = SOFT_ALARM;
            return ERROR;
    }
    va_end (value);
    if (width >= buffersize)
    {
        stream->status = HW_LIMIT_ALARM;
        return ERROR;
    }
    if (len < 0)
    {
        stream->status = CALC_ALARM;
        return ERROR;
    }
    stream->length += len;
    return OK;
}

long devStreamScanSep (dbCommon *record)
{
    streamPrivate_t *stream = (streamPrivate_t *) record->dpvt;
    uchar_t *sep = stream->public.protocol.separator;
    uchar_t *buffer = stream->ioBuffer + stream->processed;
    int maxlen = stream->length - stream->processed;
    int len = 0;
    
    if (maxlen <= 0) return ERROR;
    if (*sep == ' ')
    {
        /* skip any leading whitespaces */
        while (len <= maxlen && isspace (buffer[len])) len++;
        sep++;
    }
    while (*sep != 0)
    {
        if (*sep == SKIP) { sep++; len++; continue; }
        if (*sep == ESC) sep ++;
        if (len >= maxlen || *sep++ != buffer[len++])
        {
            if (devStreamDebug)
                printErr ("devStreamScanSep %s: separator \"%s\" not found\n",
                    record->name, stream->public.protocol.separator);
            return ERROR;
        }
    }
    stream->processed += len;

    if (devStreamDebug)
        printErr ("devStreamScanSep %s: %d of %d bytes consumed\n",
            record->name, stream->processed, stream->length);
    return OK;
}

long devStreamPrintSep (dbCommon *record)
{
    streamPrivate_t *stream = (streamPrivate_t *) record->dpvt;
    uchar_t *sep = stream->public.protocol.separator;
    uchar_t *buffer = stream->ioBuffer + stream->length;
    int maxlen = stream->public.protocol.bufferSize - stream->length;
    int len = 0;
    
    if (*sep == ' ')
    {
        /* skip leading space */
        sep++;
    }
    while (*sep != 0)
    {
        if (*sep == SKIP) { sep++; continue; }
        if (*sep == ESC) sep ++;
        buffer[len++] = *sep++;
        if (len > maxlen)
        {
            stream->status = HW_LIMIT_ALARM;
            return ERROR;
        }
    }
    stream->length += len;
    return OK;
}

/*
   The following functions are bus interface. Their prorotypes will not change
   in future minor releases.
   These functions can be called from interrupt level.
*/

/* devStreamReceive
   Call this function from yout bus interface, whenever input data arrives
   after a 'startInput'. It returns the number of consumed bytes. If this
   number is smaller than dataSize, you might call it again with the remaining
   input. Don't call it any more after a 'stopInput'. You can give the endFlag
   as a hint, if the bus detects 'end of message'.
*/
void devStreamReceive (stream_t *public, char *data, long dataSize, int endflag)
{
    streamPrivate_t *stream = (streamPrivate_t *) public;
    protocol_t *protocol = &stream->public.protocol;
    ulong_t bufferSize = protocol->bufferSize;
    ulong_t oldLength = stream->length;
    ulong_t copySize = dataSize;
    uchar_t *buffer = stream->ioBuffer;
    uchar_t *endPtr, *bufferIndex;
    int termindex;

    if (*stream->activeCommand != IN) return;
    if (oldLength + dataSize > bufferSize)
    {
        /* buffer is full -> copy only the first part */
        endflag = TRUE;
        copySize = bufferSize - oldLength;
        stream->status = HW_LIMIT_ALARM;
    }
    memcpy (buffer + oldLength, data, copySize);
    stream->length += copySize;
    bufferIndex = buffer + stream->length;
    endPtr = bufferIndex - 1;
    *bufferIndex = 0;
    #ifdef DEBUG
    logMsg ("devStreamDataReceive %s: input @ %p:\n%s\n",
        public->record->name, buffer + oldLength, buffer + oldLength);
    #endif
    /* endPtr points to the last databyte, bufferIndex points to the byte after the data */
    if (protocol->inTerminator[0] != 0)
    {
        /* find the terminator in new data */
        for (bufferIndex = buffer + oldLength; bufferIndex != NULL;)
        {
            #ifdef DEBUG
            logMsg ("devStreamDataReceive %s: terminator search @ %p\n",
                public->record->name, bufferIndex);
            #endif
            termindex = protocol->inTerminator[0];
            dataSize = buffer + oldLength + copySize - bufferIndex;
            /* search for last terminator byte */
            bufferIndex = endPtr = memchr (bufferIndex, protocol->inTerminator[termindex], dataSize);
            if (endPtr != NULL)
            {
                bufferIndex++;
                while (--termindex)
                {
                    if (--endPtr < buffer || *endPtr != protocol->inTerminator[termindex])
                    {
                        /* other terminator bytes don't match */
                        #ifdef DEBUG
                        logMsg ("devStreamDataReceive %s: terminator byte %d doesn't match @ %p\n",
                            public->record->name, termindex, endPtr);
                        #endif
                        endPtr = NULL;
                        break;
                    }
                }
                if (termindex == 0) break;
            }
        }
        if (endPtr != NULL)
        {
            #ifdef DEBUG
            logMsg ("devStreamDataReceive %s: terminator found @ %p\n",
                public->record->name, endPtr);
            #endif
            /* endPtr points to the first terminator byte */
            /* bufferIndex points to the byte after the terminator */
            /* don't consume data after the terminator */
            copySize = bufferIndex - (buffer + oldLength);
            /* skip the terminator */
            *endPtr = 0;
            stream->length = endPtr - buffer;
            endflag = TRUE;
            stream->status = NO_ALARM;
        }
    }
    if (endflag)
    {
        wdCancel (stream->wdId);
        #ifdef DEBUG
        logMsg ("devStreamDataReceive %s: input complete\n",
            public->record->name);
        #endif
        public->acceptInput = FALSE;
        callbackRequest(&public->callback);
    }
}

void devStreamStartTimer (stream_t *public, ushort_t timeout, FUNCPTR callback)
{
    streamPrivate_t *stream = (streamPrivate_t *) public;
    
    wdStart (stream->wdId, timeout * sysClkRateGet() / 1000, callback, (int) stream);
    
}

/* You must call this to signal output complete */
void devStreamWriteFinished (stream_t *public)
{
    streamPrivate_t *stream = (streamPrivate_t *) public;
    
    if (*stream->activeCommand != OUT) return;
    wdCancel (stream->wdId);
    #ifdef DEBUG
    logMsg ("devStream %s: write finished\n", public->record->name);
    #endif
    callbackRequest (&public->callback);
}

/* You must call this to signal an event */
void devStreamEvent (stream_t *public)
{
    streamPrivate_t *stream = (streamPrivate_t *) public;
    
    if (*stream->activeCommand != EVENT) return;
    wdCancel (stream->wdId);
    public->acceptEvent = FALSE;
    #ifdef DEBUG
    logMsg ("devStream %s: event received\n", public->record->name);
    #endif
    if (public->record->scan == SCAN_IO_EVENT && stream->interface->startProtocol != NULL)
    {
        stream->interface->startProtocol (&stream->public);
    }
    callbackRequest (&public->callback);
}

/* You can call this to signal bus problems */
void devStreamBusError (stream_t *public)
{
    streamPrivate_t *stream = (streamPrivate_t *) public;
    
    wdCancel (stream->wdId);
    stream->status = COMM_ALARM;
    public->acceptInput = FALSE;
    public->acceptEvent = FALSE;
    #if 1
    logMsg ("devStream %s: busError\n", public->record->name);
    #endif
    callbackRequest (&public->callback);
}

/* You can call this to signal a timout while writing to the bus */
void devStreamWriteTimeout (stream_t *public)
{
    streamPrivate_t *stream = (streamPrivate_t *) public;
    
    if (*stream->activeCommand != OUT) {
        if (devStreamDebug)
            logMsg ("devStream %s: writeTimeout but activeCommand != OUT\n",
                public->record->name);
        return;
    }
    if (!public->haveBus) {
        if (devStreamDebug)
            logMsg ("devStream %s: writeTimeout but don't have bus\n",
                public->record->name);
        return;
    }
    stream->status = WRITE_ALARM;
    if (devStreamDebug)
        logMsg ("devStream %s: writeTimeout, lockNext=%p\n",
            public->record->name,
            stream->lockNext);
    callbackRequest (&public->callback);
}

/* You can call this to signal a timout while reding from the bus */
void devStreamReadTimeout (stream_t *public)
{
    streamPrivate_t *stream = (streamPrivate_t *) public;
    
    if (*stream->activeCommand != IN) {
        if (devStreamDebug)
            logMsg ("devStream %s: readTimeout but activeCommand != IN\n",
                public->record->name);
        return;
    }
    stream->status = READ_ALARM;
    public->acceptInput = FALSE;
    #if 1
    if (stream->public.protocol.inTerminator[0] != 0)
    {
        if (devStreamDebug)
            logMsg ("devStream %s: readTimeout after %d bytes\n", 
                public->record->name, stream->length);
    }
    #endif
    callbackRequest (&public->callback);
}

/* The following functions are local stuff.
   This means, I might change them at any time without any announcement.
*/

LOCAL void replyTimeout (streamPrivate_t *stream)
{
    if (*stream->activeCommand != IN) {
        if (devStreamDebug)
            logMsg ("devStream %s: replyTimeout but activeCommand != IN\n",
                stream->public.record->name);
        return;
    }
    stream->status = TIMEOUT_ALARM;
    stream->public.acceptInput = FALSE;
    stream->public.acceptEvent = FALSE;
    #if 1
    if (devStreamDebug)
        logMsg ("devStream %s: replyTimeout\n", stream->public.record->name);
    #endif
    callbackRequest (&stream->public.callback);
}

LOCAL void lockTimeout (streamPrivate_t *stream)
{
    stream->lockCancelled = TRUE;
    stream->status = TIMEOUT_ALARM;
    #if 1
    if (devStreamDebug)
        logMsg ("devStream %s: lockTimeout\n", stream->public.record->name);
    #endif
    callbackRequest (&stream->public.callback);
}

LOCAL long startIo (streamPrivate_t *stream)
{    
    #ifdef DEBUG
    printErr ("====================================\n"
        "devStream startIo %s = %p\n",
        stream->public.record->name, stream);
    #endif
    stream->convert = OK;
    stream->phase = (stream->public.record->scan == SCAN_IO_EVENT) ?
        PHASE_ASYNC : PHASE_INIT;
    stream->codeIndex = stream->public.protocol.code;
    if (stream->interface->startProtocol != NULL)
    {
        stream->interface->startProtocol (&stream->public);
    }
    if (nextIo (stream) != OK)
    {
        process (stream);
        return ERROR;
    }
    if (*stream->activeCommand != NUL && stream->phase != PHASE_ASYNC)
    {
        stream->public.record->pact = TRUE;
    }
    return OK;
}

LOCAL long nextIo (streamPrivate_t *stream)
{
    protocol_t *protocol = &stream->public.protocol;
    struct dbCommon *record = stream->public.record; 
    format_t *format;
    int i;

    stream->status = NO_ALARM;
    stream->activeCommand = stream->codeIndex;
    if (*stream->activeCommand == OUT)
    {

        if (devStreamDebug)
            printErr ("devStream nextIo %s: OUT command\n",
                record->name);

        stream->length = 0;
        stream->processed = 0;
        while (1)
        {
            switch (*++stream->codeIndex)
            {
                case IN:
                case OUT:
                case WAIT:
                case EVENT:
                case NUL:
                    /* add the terminator */
                    if (stream->length + protocol->outTerminator[0] >= protocol->bufferSize)
                    {
                        if (devStreamDebug)
                            printErr ("devStream nextIo %s: buffer overflow\n",
                                record->name);
                        stream->status = HW_LIMIT_ALARM;
                        process (stream);
                        return ERROR;
                    } 
                    for (i = 1; i <= protocol->outTerminator[0]; i++)
                    {
                        stream->ioBuffer[stream->length++] = protocol->outTerminator[i];    
                    }
        /* while (1) ends here */
                    if (stream->public.haveBus)
                    {
                        #ifdef DEBUG
                        stream->ioBuffer[stream->length]=0;
                        if (devStreamDebug)
                            printErr ("devStream nextIo %s: output \"%s\" \n",
                                record->name, stream->ioBuffer);
                        #endif
                        callback (stream);
                        return OK;
                    }
                    #ifdef DEBUG
                    stream->ioBuffer[stream->length]=0;
                    if (devStreamDebug)
                        printErr ("devStream nextIo %s: waiting for bus lock; output \"%s\" \n",
                            record->name, stream->ioBuffer);
                    #endif
#if 1
                    if (protocol->lockTimeout != 0)
                    {
                        if (devStreamDebug)
                            printErr ("devStream nextIo %s: starting lock timout %d ms\n",
                                record->name, protocol->lockTimeout);

                        wdStart (stream->wdId, protocol->lockTimeout*sysClkRateGet()/1000, 
                            (FUNCPTR) lockTimeout, (int) stream);
                    }
#endif
                    busLockRequest (stream);
                    return OK;
                case FORMAT:
                    /* format string output */
                    format = (format_t *) (stream->codeIndex + 2);
                    stream->codeIndex += stream->codeIndex[1];
                    if (format->flags & FORMAT_FLAG_SKIP)
                    {
                        if (devStreamDebug)
                            printErr ("devStream nextIo %s: illegal %%* in output format\n",
                                record->name);
                        stream->status = SOFT_ALARM;
                        process (stream);
                        return ERROR;
                    }
                    if (format->type == 0xFF)
                    {
                        switch (format->conversion)
                        {
                            case 'c':
                            case 'd':
                            case 'i':
                            case 'o':
                            case 'u':
                            case 'x':
                            case 'X':
                            case 'b':
                            case 'r':
                            case 'D':
                                format->type = DBF_LONG;
                                break;
                            case 'f':
                            case 'e':
                            case 'E':
                            case 'g':
                            case 'G':
                                format->type = DBF_DOUBLE;
                                break;
                            case 's':
                                format->type = DBF_STRING;
                                break;
                            case '{':
                                format->type = DBF_ENUM;
                                break;
                        }
                    }
                    if (stream->writeData == NULL)
                    {
                        if (devStreamDebug)
                            printErr ("devStream nextIo %s: no writeData function\n",
                                record->name);
                        stream->status = SOFT_ALARM;
                        process (stream);
                        return ERROR;
                    }
                    if (record->scan == SCAN_IO_EVENT &&
                        record->pact == FALSE)
                    {
                        if (devStreamDebug)
                            printErr ("devStream nextIo %s: process to get new output value\n",
                                record->name);

                        stream->phase = PHASE_REPLY;
                        process (stream);
                    }
                    if (stream->writeData (record, format) == ERROR)
                    {
                        if (stream->status == NO_ALARM)
                        {
                            if (devStreamDebug)
                                printErr ("devStream nextIo %s: error in writeData\n",
                                    record->name);
                            stream->status = SOFT_ALARM;
                        }
                        process (stream);
                        return ERROR;
                    }
                    break;
                case SKIP:
                    if (devStreamDebug)
                        printErr ("devStream nextIo %s: SKIP in OUT command\n",
                            record->name);
                    stream->status = SOFT_ALARM;
                    process (stream);
                    return ERROR;
                case ESC:
                    ++stream->codeIndex;
                default:
                    stream->ioBuffer[stream->length] = *stream->codeIndex;
                    if (stream->length++ >= protocol->bufferSize)
                    {
                        if (devStreamDebug)
                            printErr ("devStream nextIo %s: buffer overflow\n",
                                record->name);
                        stream->status = HW_LIMIT_ALARM;
                        return ERROR;
                    } 
            }
        }
    }
    if (*stream->activeCommand == IN)
    {
        stream->length = 0;
        stream->ioBuffer[0] = 0;
        stream->processed = 0;
        if (stream->phase == PHASE_ASYNC)
        {
            if (devStreamDebug)
                printErr ("devStream nextIo %s: IN command, wait async\n",
                    record->name);

            if (stream->public.haveBus)
            {
                if (devStreamDebug)
                    printErr ("devStream nextIo %s: IN command, unlock bus\n",
                        record->name);

                busUnlock (stream);
            }
            record->pact = FALSE;
        }
        else
        {
            if (stream->phase == PHASE_INIT)
            {
                stream->phase = PHASE_REPLY;
            }

            if (devStreamDebug)
                printErr ("devStream nextIo %s: IN command, reply watchdog start %d ms\n",
                    record->name, protocol->replyTimeout);

            if (protocol->replyTimeout != 0)
            {
                wdStart(stream->wdId, protocol->replyTimeout*sysClkRateGet()/1000, 
                    (FUNCPTR) replyTimeout, (int) stream);
            }
        }
        stream->public.acceptInput = TRUE;
        if (stream->interface->startInput != NULL)
        {
            stream->interface->startInput (&stream->public);
        }
        return OK;
    }
    if (*stream->activeCommand == WAIT)
    {
        i = *(ushort_t *)(++stream->codeIndex);
        stream->codeIndex += sizeof (ushort_t);
        wdStart(stream->wdId, i*sysClkRateGet()/1000,
            (FUNCPTR) callbackRequest, (int) stream);

        if (devStreamDebug)
            printErr ("devStream nextIo %s: WAIT command %d msecs\n",
                record->name, i);

        return OK;
    }
    if (*stream->activeCommand == EVENT)
    {
        i = *(ushort_t *)(++stream->codeIndex);
        stream->codeIndex += sizeof (ushort_t);
        if (stream->phase == PHASE_ASYNC)
        {
            stream->phase = PHASE_INIT;
        }
        if (record->scan == SCAN_IO_EVENT && stream->interface->stopProtocol != NULL)
        {
            stream->interface->stopProtocol (&stream->public);
        }
        if (stream->public.haveBus)
        {
            #if 1
            if (devStreamDebug)
                printErr ("devStream nextIo %s: EVENT command, unlock bus\n",
                    record->name);
            #endif
            busUnlock (stream);
        }
        if (i == 0)
        {
            /* wait forever */

            if (devStreamDebug)
                printErr ("devStream nextIo %s: EVENT command, wait forever\n",
                    record->name, i);
        }
        else
        {
            if (devStreamDebug)
                printErr ("devStream nextIo %s: EVENT command, wait %d msecs\n",
                    record->name, i);

            wdStart(stream->wdId, i*sysClkRateGet()/1000,
                (FUNCPTR) replyTimeout, (int) stream);
        }
        stream->public.acceptEvent = TRUE;
        if (stream->interface->startInput != NULL)
        {
            stream->interface->startInput (&stream->public);
        }
        return OK;
    }
    if (*stream->activeCommand == NUL)
    {
        if (devStreamDebug)
            printErr ("devStream nextIo %s: NUL command\n", record->name);

        stream->status = NO_ALARM;
        process (stream);
        return OK;
    }
    if (devStreamDebug)
        printErr ("devStream nextIo %s: unknown io command 0x%02x\n",
            record->name, *stream->activeCommand);
    stream->status = SOFT_ALARM;
    process (stream);
    return ERROR;
}

LOCAL void process (streamPrivate_t *stream)
{
    struct dbCommon *record = stream->public.record;
    
    if (devStreamDebug)
        printErr ("devStream process %s: pact=%d status=0x%02x\n",
            record->name, record->pact, stream->status);

    if (stream->phase == PHASE_ASYNC && stream->status == CALC_ALARM)
    {
        if (devStreamDebug)
            printErr ("devStream process %s: async input wrong; start over\n",
                record->name);
        stream->codeIndex = stream->activeCommand;
        nextIo (stream);
        return;
    }
    if (stream->interface->stopProtocol != NULL)
    {
        stream->interface->stopProtocol (&stream->public);
    }
    if (stream->public.haveBus)
    {
        if (devStreamDebug)
            printErr ("devStream process %s: unlock bus\n", record->name);
        busUnlock (stream);
    }
    if (record->pact || record->scan == SCAN_IO_EVENT)
    {
        if (devStreamDebug)
            printErr ("devStream process %s: do record process; status=0x%02x phase=%d pact=%d\n",
                record->name, stream->status, stream->phase, record->pact);

        dbScanLock((struct dbCommon *) record);
        (*((struct rset *) record->rset)->process)(record);
        dbScanUnlock((struct dbCommon *) record);
    }
    stream->status = NO_ALARM;
    if (record->scan == SCAN_IO_EVENT)
    {
        startIo (stream);
    }
}

LOCAL void printbytes (unsigned char *bytes, int length)
{
    int i;
    unsigned char c;
    
    for (i = 0; i < length; i++)
    {
        c = bytes[i];
        if (isprint (c))
            printErr ("%c", c);
        else 
            printErr ("\\x%02x", c);
    }
    printErr ("\n");
}

LOCAL void callback (streamPrivate_t *stream)
{
    format_t *format;
    struct dbCommon *record = stream->public.record;
    long size;

    if ((*stream->activeCommand == IN || *stream->activeCommand == EVENT) &&
        stream->interface->stopInput != NULL)
    {
        stream->interface->stopInput (&stream->public);
    }
    wdCancel (stream->wdId);
    if (stream->status != NO_ALARM &&
        /* If there's no terminator, timeout is a valid termination */
        (stream->status != READ_ALARM || stream->public.protocol.inTerminator[0] != 0))
    {

        if (devStreamDebug)
            printErr ("devStream callback %s: alarm 0x%02x\n",
                record->name, stream->status);

        process (stream);
        return;
    }

    if (devStreamDebug)
        printErr ("devStream callback %s:command 0x%02x status 0x%02x \n",
            record->name, *stream->activeCommand, stream->status);

    if (*stream->activeCommand == IN)
    {
        /* input is ready, now check syntax */
        if (devStreamDebug)
        {
            printErr ("devStream callback %s: %d bytes input:\n",
                record->name, stream->length);
            printbytes (stream->ioBuffer, stream->length);
        }

        while (1)
        {
            switch (*++stream->codeIndex)
            {
                case IN:
                case OUT:
                case WAIT:
                case EVENT:
                case NUL:
                    if (stream->processed < stream->length &&
                        !(stream->public.protocol.flags & FLAG_IGNORE_EXTRA_INPUT))
                    {
                        /* answer is longer than expected */
                        if (devStreamDebug)
                        {
                            printErr ("devStream callback %s: %d bytes unparsed input left:\n",
                                record->name, stream->length - stream->processed);
                            printbytes (stream->ioBuffer, stream->processed);
                            printbytes (stream->ioBuffer + stream->processed, stream->length - stream->processed);
                        }
                        stream->status = CALC_ALARM;
                        process (stream);
                    }
                    /* input has successfully been parsed, now continue the protocol */
                    if (devStreamDebug)
                        printErr ("devStream callback %s: input OK\n",
                            record->name);

                    if (stream->phase == PHASE_ASYNC)
                    {
                        stream->phase = PHASE_REPLY;
                    }
                    stream->status = NO_ALARM;
                    nextIo (stream);
                    return;
                case FORMAT:
                    /* format string input */
                    format = (format_t *) (stream->codeIndex + 2);
                    stream->codeIndex += stream->codeIndex[1];
                    if (format->prec != -1)
                    {
                        if (devStreamDebug)
                            printErr ("devStream callback %s: illegal .prec in input format\n",
                                record->name);
                        stream->status = SOFT_ALARM;
                        process (stream);
                        return;
                    }
                    if (format->flags & FORMAT_FLAG_SKIP)
                    {
                        /* swallow the input */
                        if (devStreamScanf (record, format, NULL) != OK)
                        {
                            process (stream);
                            return;
                        }            
                        break;
                    }
                    if (format->type == 0xFF)
                    {
                        switch (format->conversion)
                        {
                            case 'd':
                            case 'i':
                            case 'o':
                            case 'u':
                            case 'x':
                            case 'X':
                            case 'b':
                            case 'r':
                            case 'D':
                                format->type = DBF_LONG;
                                break;
                            case 'f':
                            case 'e':
                            case 'E':
                            case 'g':
                            case 'G':
                                format->type = DBF_DOUBLE;
                                break;
                            case 's':
                            case 'c':
                            case '[':
                                format->type = DBF_STRING;
                                break;
                            case '{':
                                format->type = DBF_ENUM;
                                break;
                        }
                    }
                    if (stream->phase == PHASE_GOT_VALUE)
                    {
                        if (devStreamDebug)
                            printErr ("devStream callback %s: data read twice\n",
                                record->name);
                        stream->status = SOFT_ALARM;
                        process (stream);
                        return;
                    }
                    if (stream->readData == NULL)
                    {
                        if (devStreamDebug)
                            printErr ("devStream callback %s: no readData function\n",
                                record->name);
                        stream->status = SOFT_ALARM;
                        process (stream);
                        return;
                    }
                    stream->convert = stream->readData (record, format);
                    if (stream->convert == ERROR)
                    {
                        if (stream->status == NO_ALARM)
                        {
                            if (devStreamDebug)
                                printErr ("devStream callback %s: readData failed\n",
                                    record->name);
                            stream->status = SOFT_ALARM;
                        }
                        process (stream);
                        return;
                    }
                    stream->phase = PHASE_GOT_VALUE;
                    if (stream->convert == DO_NOT_CONVERT)
                    {
                        record->udf = FALSE;
                    }
                    break;
                case SKIP:
                    stream->processed++;
                    break;
                case ESC:
                    ++stream->codeIndex;
                default:
                    if (stream->processed > stream->length) break;
                    if (stream->ioBuffer[stream->processed++] != *stream->codeIndex)
                    {
                        if (stream->processed > stream->length) break;
                        /* answer doesn't match expected format */
                        if (stream->phase != PHASE_ASYNC)
                            if (devStreamDebug)
                            {
                                printErr ("devStream callback %s: input wrong: char %d of %d should be ",
                                    record->name, stream->processed, stream->length);
                                printbytes (stream->codeIndex, 1);
                                printbytes (stream->ioBuffer, stream->length);
                            }
                        stream->status = CALC_ALARM;
                        process (stream);
                        return;
                    }
                    break;
            }
            if (stream->processed > stream->length)
            {
                if (devStreamDebug)
                {
                    printErr ("devStream callback %s: input too short (%d bytes)\n",
                        record->name, stream->length);
                    printbytes (stream->ioBuffer, stream->length);
                }
                stream->status = CALC_ALARM;
                process (stream);
                return;
            }
        }
    }
    if (*stream->activeCommand == OUT)
    {
        if (stream->processed >= stream->length)
        {
            wdCancel (stream->wdId);

            if (devStreamDebug)
            {
                printErr ("devStream callback %s: %d bytes output complete:\n",
                    record->name, stream->processed);
                printbytes (stream->ioBuffer, stream->length);
            }
            nextIo (stream);
            return;
        }
        if (!stream->public.haveBus)
        {
            if (devStreamDebug)
                printErr ("devStream callback %s: Try to write but don't have bus (caller is %s)\n",
                    record->name, stream->public.callback.user);
            stream->status = WRITE_ALARM;
            process (stream);
            return;
        }

        if (devStreamDebug)
            printErr ("devStream callback %s: %p ready to write\n",
                record->name, stream);

        size = stream->interface->writePart (&stream->public,
            (char *)stream->ioBuffer + stream->processed,
            stream->length - stream->processed);
        if (size == ERROR)
        {
            #if 1
            if (devStreamDebug)
                printErr ("devStream callback %s: writePart returned error\n",
                    record->name);
            #endif
            stream->status = WRITE_ALARM;
            process (stream);
            return;
        }
        stream->processed += size;

        if (devStreamDebug)
            printErr ("devStream callback %s: %d of %d bytes written\n",
                record->name, stream->processed, stream->length);

        return;
    }
    if (*stream->activeCommand == WAIT)
    {
        if (devStreamDebug)
            printErr ("devStream callback %s: WAIT ready\n",
                record->name);

        nextIo (stream);
        return;
    }
    if (*stream->activeCommand == EVENT)
    {
        if (devStreamDebug)
            printErr ("devStream callback %s: EVENT received\n",
                record->name);

        nextIo (stream);
        return;
    }
    if (devStreamDebug)
        printErr ("devStream callback %s: unknown reason 0x%02x\n",
            record->name, *stream->activeCommand);
}

LOCAL void showQueue (char* name, streamPrivate_t *queue)
{
    printErr ("########### Queue of %s ############\n", name);
    while (queue)
    {
        printErr ("  %s %s %s\n", queue->public.record->name,
            queue->public.haveBus ? "haveBus" : "",
            queue->lockCancelled ? "lockCancelled" : "");
        queue = queue->lockNext; 
    }
    printErr ("-------------------------------------\n", name);
}

LOCAL void busLockRequest (streamPrivate_t *stream)
{
    streamPrivate_t **queue = &(stream->lockQueue[stream->public.channel]);
    streamPrivate_t **current;
    streamPrivate_t **first; /* pointer to first item in list */
    streamPrivate_t *temp;
    
    if (devStreamDebug)
        printErr ("devStream busLockRequest %s: queue %sempty\n",
            stream->public.record->name, *queue == NULL ? "" : "NOT ");

    semTake (lockSem, WAIT_FOREVER);
    /* remove cancelled entries */
    if (*queue != NULL)
    {
        current = &((*queue)->lockNext); /* start *after* 1st in queue */
	first = current;
        while (*current != NULL)
        {
            if ((*current)->lockCancelled)
            {
                if (devStreamDebug)
                    printErr ("devStream busLockRequest %s: remove cancelled %s\n",
                        stream->public.record->name,
                        (*current)->public.record->name);
                temp = *current;
                current = &(temp->lockNext);
                temp->lockNext = NULL;
		/* test for infinite loop where last item refernces first - has been seen to happen!! - IJG 26/3/2007 */
		if (current == first)
		   break;
            }
            else
            {
                current = &((*current)->lockNext);
		/* test for infinite loop where last item refernces first - has been seen to happen!! - IJG 26/3/2007 */
		if (current == first)
		   break;
            }
        }
    }
    /* Already in the queue? Should not happen, but you never know ... */
    if (stream->lockNext != NULL)
    {
        if (devStreamDebug)
        {
            printErr ("devStream busLockRequest %s: Oops! I'm already in the queue (next is %s)\n",
                stream->public.record->name, stream->lockNext->public.record->name);
            showQueue (stream->public.record->name, *queue);
        }
        stream->lockNext = NULL;
    }
    /* insert into queue */
    stream->lockCancelled = FALSE;
    if (*queue == NULL)
    {
        /* queue is empty, handle entry now */
        *queue = stream;

        if (devStreamDebug)
            printErr ("devStream busLockRequest %s: starting now\n",
                stream->public.record->name);

        stream->public.haveBus = TRUE;
        callbackRequest (&stream->public.callback);
    }
    else
    {
        /* insert before entry with weaker priority */
        current = &((*queue)->lockNext);       /* start *after* 1st in queue */
	first = current;
        while (*current != NULL &&
            (*current)->public.callback.priority >=
                stream->public.callback.priority)   /* until prio is smaller */
        {
            current = &((*current)->lockNext);
            /* test for infinite loop where last item refernces first - has been seen to happen!! - IJG 26/3/2007 */
	    if (current == first)
	        break;
        }
        stream->lockNext = *current;
        *current = stream;
        #if 0
        printErr ("devStream busLockRequest %s: waiting for %s\n",
            stream->public.record->name, (*queue)->public.record->name);
        #endif
    }
    semGive (lockSem);
}

LOCAL void busUnlock (streamPrivate_t *stream)
{
    streamPrivate_t **queue = &stream->lockQueue[stream->public.channel];
    streamPrivate_t *temp;
    

        if (devStreamDebug)
            printErr ("devStream busUnlock %s\n",
                stream->public.record->name);

    semTake (lockSem, WAIT_FOREVER);
    if (!stream->public.haveBus)
    {
        if (devStreamDebug)
            printErr ("devStream busUnlock %s: Oops! Don't have bus\n",
                stream->public.record->name);
    }
    stream->public.haveBus = FALSE;
    if ((*queue) != stream)
    {
        if ((*queue) == NULL)
        {
            if (devStreamDebug)
                printErr ("devStream busUnlock %s: Oops! I'm not first in queue, it's empty\n",
            stream->public.record->name);
            stream->lockNext = NULL;
        }
        else
        {
            if (devStreamDebug)
                printErr ("devStream busUnlock %s: Oops! I'm not first in queue, it's %p (%s)\n",
                    stream->public.record->name, (*queue), (*queue)->public.record->name);
            stream->lockCancelled = TRUE;
            showQueue (stream->public.record->name, *queue);
        }
        semGive (lockSem);
        return;
    }
    *queue = stream->lockNext;
    stream->lockNext = NULL;
    while (*queue != NULL)
    {
        if (!(*queue)->lockCancelled)
        /* handle next not cancelled entry */
        {
            #if 0
            printErr ("devStream busUnlock %s: starting %s now\n",
                stream->public.record->name,
                (*queue)->public.record->name);
            #endif
            (*queue)->public.haveBus = TRUE;
            callbackRequest (&(*queue)->public.callback);
            semGive (lockSem);
            return;
        }
        else
        /* remove cancelled entry */
        {
            #if 1
            if (devStreamDebug)
                printErr ("devStream busUnlock %s: remove cancelled %s\n",
                    stream->public.record->name,
                    (*queue)->public.record->name);
            #endif
            temp = *queue;
            *queue = temp->lockNext;
            temp->lockNext = NULL;
        }
    }
    semGive (lockSem);
}
