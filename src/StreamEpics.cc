/***************************************************************
* StreamDevice Support                                         *
*                                                              *
* (C) 1999 Dirk Zimoch (zimoch@delta.uni-dortmund.de)          *
* (C) 2005 Dirk Zimoch (dirk.zimoch@psi.ch)                    *
*                                                              *
* This is the interface to EPICS for StreamDevice.             *
* Please refer to the HTML files in ../doc/ for a detailed     *
* documentation.                                               *
*                                                              *
* If you do any changes in this file, you are not allowed to   *
* redistribute it any more. If there is a bug or a missing     *
* feature, send me an email and/or your patch. If I accept     *
* your changes, they will go to the next release.              *
*                                                              *
* DISCLAIMER: If this software breaks something or harms       *
* someone, it's your problem.                                  *
*                                                              *
***************************************************************/

#include <errno.h>
#include "StreamCore.h"
#include "StreamError.h"

#include "epicsVersion.h"
#ifdef BASE_VERSION
#define EPICS_3_13
#endif

#ifdef EPICS_3_13
extern "C" {
#endif

#define epicsAlarmGLOBAL
#include "alarm.h"
#undef epicsAlarmGLOBAL
#include "dbStaticLib.h"
#include "drvSup.h"
#include "recSup.h"
#include "recGbl.h"
#include "devLib.h"
#include "callback.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#ifdef EPICS_3_13

#include <semLib.h>
#include <wdLib.h>
#include <taskLib.h>

extern DBBASE *pdbbase;

} // extern "C"

#else // !EPICS_3_13

#include "epicsTimer.h"
#include "epicsMutex.h"
#include "epicsEvent.h"
#include "epicsTime.h"
#include "epicsThread.h"
#include "epicsString.h"
#include "registryFunction.h"
#include "iocsh.h"

#if (!defined VERSION_INT && EPICS_MODIFICATION<9)
// iocshCmd() is missing in iocsh.h (up to R3.14.8.2)
// To build with win32-x86, you MUST fix iocsh.h.
// Move the declaration below to iocsh.h and rebuild base.
extern "C" epicsShareFunc int epicsShareAPI iocshCmd(const char *command);
#endif

#endif // !EPICS_3_13

#if defined(__vxworks) || defined(vxWorks)
#include <symLib.h>
#include <sysSymTbl.h>
#endif

#include "devStream.h"

#define Z PRINTF_SIZE_T_PREFIX

enum MoreFlags {
    // 0x00FFFFFF used by StreamCore
    InDestructor  = 0x0100000,
    ValueReceived = 0x0200000,
    Aborted       = 0x0400000
};

extern "C" {
void streamExecuteCommand(CALLBACK *pcallback);
void streamRecordProcessCallback(CALLBACK *pcallback);
long streamReload(const char* recordname);
long streamReportRecord(const char* recordname);
}

class Stream : protected StreamCore
#ifndef EPICS_3_13
    , epicsTimerNotify
#endif
{
    dbCommon* record;
    const struct link *ioLink;
    streamIoFunction readData;
    streamIoFunction writeData;
#ifdef EPICS_3_13
    WDOG_ID timer;
    CALLBACK timeoutCallback;
    SEM_ID mutex;
    SEM_ID initDone;
#else
    epicsTimerQueueActive* timerQueue;
    epicsTimer* timer;
    epicsMutex mutex;
    epicsEvent initDone;
#endif
    int status;
    int convert;
    ssize_t currentValueLength;
    IOSCANPVT ioscanpvt;
    CALLBACK commandCallback;
    CALLBACK processCallback;


#ifdef EPICS_3_13
    static void expire(CALLBACK *pcallback);
#else
// epicsTimerNotify method
    expireStatus expire(const epicsTime&);
#endif

// StreamCore methods
    void protocolStartHook();
    void protocolFinishHook(ProtocolResult);
    void startTimer(unsigned long timeout);
    bool getFieldAddress(const char* fieldname,
        StreamBuffer& address);
    bool formatValue(const StreamFormat&,
        const void* fieldaddress);
    bool matchValue(const StreamFormat&,
        const void* fieldaddress);
    void lockMutex();
    void releaseMutex();
    bool execute();
    friend void streamExecuteCommand(CALLBACK *pcallback);
    friend void streamRecordProcessCallback(CALLBACK *pcallback);
    friend long streamReportRecord(const char* recordname);

// Stream Epics methods
    Stream(dbCommon* record, const struct link *ioLink,
        streamIoFunction readData, streamIoFunction writeData);
    ~Stream();
    long parseLink(const struct link *ioLink, char* filename, char* protocol,
        char* busname, int* addr, char* busparam);
    long initRecord(const char* filename, const char* protocol,
        const char* busname, int addr, const char* busparam);
    bool print(format_t *format, va_list ap);
    ssize_t scan(format_t *format, void* pvalue, size_t maxStringSize);
    bool process();

// device support functions
    friend long streamInitRecord(dbCommon *record, const struct link *ioLink,
        streamIoFunction readData, streamIoFunction writeData);
    friend long streamReadWrite(dbCommon *record);
    friend long streamGetIointInfo(int cmd, dbCommon *record,
        IOSCANPVT *ppvt);
    friend long streamPrintf(dbCommon *record, format_t *format, ...);
    friend ssize_t streamScanfN(dbCommon *record, format_t *format,
        void*, size_t maxStringSize);
    friend long streamReload(const char* recordname);

public:
    long priority() { return record->prio; };
    static long report(int interest);
    static long drvInit();
};


// shell functions ///////////////////////////////////////////////////////
extern "C" { // needed for Windows
epicsExportAddress(int, streamDebug);
epicsExportAddress(int, streamError);
}

// for subroutine record
long streamReloadSub()
{
    return streamReload(NULL);
}

long streamReload(const char* recordname)
{
    DBENTRY dbentry;
    dbCommon* record;
    long status;

    int oldStreamError = streamError;
    streamError = 1;

    if(!pdbbase) {
        error("No database has been loaded\n");
        streamError = oldStreamError;
        return ERROR;
    }
    debug("streamReload(%s)\n", recordname);
    dbInitEntry(pdbbase,&dbentry);
    for (status = dbFirstRecordType(&dbentry); status == OK;
        status = dbNextRecordType(&dbentry))
    {
        for (status = dbFirstRecord(&dbentry); status == OK;
            status = dbNextRecord(&dbentry))
        {
            char* value;
            if (dbFindField(&dbentry, "DTYP") != OK)
                continue;
            if ((value = dbGetString(&dbentry)) == NULL)
                continue;
            if (strcmp(value, "stream") != 0)
                continue;
            record=(dbCommon*)dbentry.precnode->precord;
            if (recordname && strcmp(recordname, record->name) != 0)
                continue;

            // This cancels any running protocol and reloads
            // the protocol file
            status = record->dset->init_record(record);
            if (status == OK || status == DO_NOT_CONVERT)
            {
                printf("%s: Protocol reloaded\n", record->name);
            }
            else
            {
                error("%s: Protocol reload failed\n", record->name);
            }
        }
    }
    dbFinishEntry(&dbentry);
    StreamProtocolParser::free();
    streamError = oldStreamError;
    return OK;
}

long streamSetLogfile(const char* filename)
{
    FILE *oldfile, *newfile = NULL;
    if (filename)
    {
        newfile = fopen(filename, "w");
        if (!newfile)
        {
            fprintf(stderr, "Opening file %s failed: %s\n", filename, strerror(errno));
            return ERROR;
        }
    }
    oldfile = StreamDebugFile;
    StreamDebugFile = newfile;
    if (oldfile) fclose(oldfile);
    return OK;
}

#ifndef EPICS_3_13
static const iocshArg streamReloadArg0 =
    { "recordname", iocshArgString };
static const iocshArg * const streamReloadArgs[] =
    { &streamReloadArg0 };
static const iocshFuncDef streamReloadDef =
    { "streamReload", 1, streamReloadArgs };

void streamReloadFunc (const iocshArgBuf *args)
{
    streamReload(args[0].sval);
}

static const iocshArg streamReportRecordArg0 =
    { "recordname", iocshArgString };
static const iocshArg * const streamReportRecordArgs[] =
    { &streamReportRecordArg0 };
static const iocshFuncDef streamReportRecordDef =
    { "streamReportRecord", 1, streamReportRecordArgs };

void streamReportRecordFunc (const iocshArgBuf *args)
{
    streamReportRecord(args[0].sval);
}

static const iocshArg streamSetLogfileArg0 =
    { "filename", iocshArgString };
static const iocshArg * const streamSetLogfileArgs[] =
    { &streamSetLogfileArg0 };
static const iocshFuncDef streamSetLogfileDef =
    { "streamSetLogfile", 1, streamSetLogfileArgs };

void streamSetLogfileFunc (const iocshArgBuf *args)
{
    streamSetLogfile(args[0].sval);
}

static void streamRegistrar ()
{
    iocshRegister(&streamReloadDef, streamReloadFunc);
    iocshRegister(&streamReportRecordDef, streamReportRecordFunc);
    iocshRegister(&streamSetLogfileDef, streamSetLogfileFunc);
    // make streamReload available for subroutine records
    registryFunctionAdd("streamReload",
        (REGISTRYFUNCTION)streamReloadSub);
    registryFunctionAdd("streamReloadSub",
        (REGISTRYFUNCTION)streamReloadSub);
}

extern "C" {
epicsExportRegistrar(streamRegistrar);
}


#endif // !EPICS_3_13

// driver support ////////////////////////////////////////////////////////

struct drvet stream = {
    2,
    (DRVSUPFUN) Stream::report,
    (DRVSUPFUN) Stream::drvInit
};
extern "C" {
epicsExportAddress(drvet, stream);
}

#ifdef EPICS_3_13
void streamEpicsPrintTimestamp(char* buffer, size_t size)
{
    size_t tlen;
    char* c;
    TS_STAMP tm;
    tsLocalTime (&tm);
    tsStampToText(&tm, TS_TEXT_MMDDYY, buffer);
    c = strchr(buffer,'.');
    if (c) {
        c[4] = 0;
    }
    tlen = strlen(buffer);
    sprintf(buffer+tlen, " %.*s", (int)(size-tlen-2), taskName(0));
}
#else // !EPICS_3_13
void streamEpicsPrintTimestamp(char* buffer, size_t size)
{
    size_t tlen;
    epicsTime tm = epicsTime::getCurrent();
    tlen = tm.strftime(buffer, size, "%Y/%m/%d %H:%M:%S.%06f");
    sprintf(buffer+tlen, " %.*s", (int)(size-tlen-2), epicsThreadGetNameSelf());
}
#endif // !EPICS_3_13

long Stream::
report(int interest)
{
    debug("Stream::report(interest=%d)\n", interest);
    printf("  %s\n", StreamVersion);

    printf("  registered bus interfaces:\n");
    StreamBusInterfaceClass interface;
    while (interface)
    {
        printf("    %s\n", interface.name());
        ++interface;
    }

    if (interest < 1) return OK;
    printf("  registered converters:\n");
    StreamFormatConverter* converter;
    int c;
    for (c=0; c < 256; c++)
    {
        converter = StreamFormatConverter::find(c);
        if (converter)
        {
            printf("    %%%c %s\n", c, converter->name());
        }
    }

    Stream* pstream;
    printf("  connected records:\n");
    for (pstream = static_cast<Stream*>(first); pstream;
        pstream = static_cast<Stream*>(pstream->next))
    {
        if (interest == 2)
        {
            printf("\n%s: %s\n", pstream->name(),
                pstream->ioLink->value.instio.string);
            pstream->printProtocol();
        }
        else
        {
            printf("    %s: %s\n", pstream->name(),
                pstream->ioLink->value.instio.string);
            if (interest == 3)
            {
                StreamBuffer buffer;
                pstream->printStatus(buffer);
                printf("      %s\n", buffer());
            }
        }
    }
    return OK;
}

long streamReportRecord(const char* recordname)
{
    Stream* pstream;
    for (pstream = static_cast<Stream*>(Stream::first); pstream;
        pstream = static_cast<Stream*>(pstream->next))
    {
        if (!recordname ||
#ifdef EPICS_3_13
            strcmp(pstream->name(), recordname) == 0)
#else
            epicsStrGlobMatch(pstream->name(), recordname))
#endif
        {
            printf("%s: %s\n", pstream->name(),
                pstream->ioLink->value.instio.string);
            StreamBuffer buffer;
            pstream->printStatus(buffer);
            printf("%s\n", buffer());
            pstream->printProtocol();
            printf("\n");
        }
    }
    return OK;
}

long Stream::
drvInit()
{
    char* path;
    debug("drvStreamInit()\n");
    path = getenv("STREAM_PROTOCOL_PATH");
#if defined(__vxworks) || defined(vxWorks)
    // for compatibility reasons look for global symbols
    if (!path)
    {
        char* symbol;
        SYM_TYPE type;
        if (symFindByName(sysSymTbl,
            "STREAM_PROTOCOL_PATH", &symbol, &type) == OK)
        {
            path = *(char**)symbol;
        }
        else
        if (symFindByName(sysSymTbl,
            "STREAM_PROTOCOL_DIR", &symbol, &type) == OK)
        {
            path = *(char**)symbol;
        }
    }
#endif
    if (!path)
    {
        fprintf(stderr,
            "drvStreamInit: Warning! STREAM_PROTOCOL_PATH not set. "
            "Defaults to \"%s\"\n", StreamProtocolParser::path);
    }
    else
    {
        StreamProtocolParser::path = path;
    }
    debug("StreamProtocolParser::path = %s\n",
        StreamProtocolParser::path);
    StreamPrintTimestampFunction = streamEpicsPrintTimestamp;
    return OK;
}

// device support (C interface) //////////////////////////////////////////

long streamInit(int after)
{
    if (after)
    {
        streamError = 0; // Switch off errors after init in order not to spam messages when a device is down.
        StreamProtocolParser::free();
    }
    return OK;
}

long streamInitRecord(dbCommon* record, const struct link *ioLink,
    streamIoFunction readData, streamIoFunction writeData)
{
    long status;
    char filename[80];
    char protocol[80];
    char busname[80];
    int addr = -1;
    char busparam[80];
    memset(busparam, 0 ,sizeof(busparam));

    debug("streamInitRecord(%s): SEVR=%d\n", record->name, record->sevr);
    Stream* pstream = (Stream*)record->dpvt;
    if (!pstream)
    {
        // initialize the first time
        debug("streamInitRecord(%s): create new Stream object\n",
            record->name);
        pstream = new Stream(record, ioLink, readData, writeData);
        record->dpvt = pstream;
    } else {
        // stop any running protocol
        debug("streamInitRecord(%s): stop running protocol\n",
            record->name);
        pstream->finishProtocol(Stream::Abort);
    }
    // scan the i/o link
    debug("streamInitRecord(%s): parse link \"%s\"\n",
        record->name, ioLink->value.instio.string);
    status = pstream->parseLink(ioLink, filename, protocol,
        busname, &addr, busparam);
    // (re)initialize bus and protocol
    debug("streamInitRecord(%s): calling initRecord\n",
        record->name);
    if (status == 0)
        status = pstream->initRecord(filename, protocol,
            busname, addr, busparam);
    if (status != OK && status != DO_NOT_CONVERT)
    {
        error("%s: Record initialization failed\n", record->name);
    }
    else if (!pstream->ioscanpvt)
    {
        scanIoInit(&pstream->ioscanpvt);
    }
    debug("streamInitRecord(%s) done status=%#lx\n", record->name, status);
    return status;
}

long streamReadWrite(dbCommon *record)
{
    Stream* pstream = (Stream*)record->dpvt;
    if (!pstream || pstream->status == ERROR)
    {
        (void) recGblSetSevr(record, UDF_ALARM, INVALID_ALARM);
        error("%s: Record not initialised correctly\n", record->name);
        return ERROR;
    }
    return pstream->process() ? pstream->convert : ERROR;
}

long streamGetIointInfo(int cmd, dbCommon *record, IOSCANPVT *ppvt)
{
    Stream* pstream = (Stream*)record->dpvt;
    debug("streamGetIointInfo(%s,cmd=%d): pstream=%p, ioscanpvt=%p\n",
        record->name, cmd,
        (void*)pstream, pstream ? pstream->ioscanpvt : NULL);
    if (!pstream)
    {
        error("streamGetIointInfo called without stream instance\n");
        return ERROR;
    }
    *ppvt = pstream->ioscanpvt;
    if (cmd == 0)
    {
        debug("streamGetIointInfo: starting protocol\n");
        // SCAN has been set to "I/O Intr"
        if (!pstream->startProtocol(Stream::StartAsync))
        {
            error("%s: Can't start \"I/O Intr\" protocol\n",
                record->name);
        }
    }
    else
    {
        // SCAN is no longer "I/O Intr"
        pstream->finishProtocol(Stream::Abort);
    }
    return OK;
}

long streamPrintf(dbCommon *record, format_t *format, ...)
{
    debug("streamPrintf(%s,format=%%%c)\n",
        record->name, format->priv->conv);
    Stream* pstream = (Stream*)record->dpvt;
    if (!pstream) return ERROR;
    va_list ap;
    va_start(ap, format);
    bool success = pstream->print(format, ap);
    va_end(ap);
    return success ? OK : ERROR;
}

ssize_t streamScanfN(dbCommon* record, format_t *format,
    void* value, size_t maxStringSize)
{
    ssize_t size;
    Stream* pstream = (Stream*)record->dpvt;
    if (!pstream) return ERROR;
    size = pstream->scan(format, value, maxStringSize);
    return size;
}

// Stream methods ////////////////////////////////////////////////////////

Stream::
Stream(dbCommon* _record, const struct link *ioLink,
    streamIoFunction readData, streamIoFunction writeData)
:record(_record), ioLink(ioLink), readData(readData), writeData(writeData)
{
    streamname = record->name;
#ifdef EPICS_3_13
    timer = wdCreate();
    mutex = semMCreate(SEM_INVERSION_SAFE | SEM_Q_PRIORITY);
    initDone = semBCreate(SEM_Q_FIFO, SEM_EMPTY);
    callbackSetCallback(expire, &timeoutCallback);
    callbackSetUser(this, &timeoutCallback);
#else
    timerQueue = &epicsTimerQueueActive::allocate(true);
    timer = &timerQueue->createTimer();
#endif
    callbackSetCallback(streamExecuteCommand, &commandCallback);
    callbackSetUser(this, &commandCallback);
    callbackSetCallback(streamRecordProcessCallback, &processCallback);
    callbackSetUser(this, &processCallback);
    status = ERROR;
    convert = DO_NOT_CONVERT;
    ioscanpvt = NULL;
}

Stream::
~Stream()
{
    lockMutex();
    flags |= InDestructor;;
    debug("~Stream(%s) %p\n", name(), (void*)this);
    if (record->dpvt)
    {
        finishProtocol(Abort);
        debug("~Stream(%s): protocol finished\n", name());
        record->dpvt = NULL;
        debug("~Stream(%s): dpvt cleared\n", name());
    }
#ifdef EPICS_3_13
    wdDelete(timer);
    debug("~Stream(%s): watchdog destroyed\n", name());
#else
    timer->destroy();
    debug("~Stream(%s): timer destroyed\n", name());
    timerQueue->release();
    debug("~Stream(%s): timer queue released\n", name());
#endif
    releaseMutex();
}

long Stream::
parseLink(const struct link *ioLink, char* filename,
    char* protocol, char* busname, int* addr, char* busparam)
{
    // parse link parameters: filename protocol busname addr busparam
    int n;
    if (ioLink->type != INST_IO)
    {
        error("%s: Wrong I/O link type %s\n", name(),
            pamaplinkType[ioLink->type].strvalue);
        return S_dev_badInitRet;
    }
    int items = sscanf(ioLink->value.instio.string, "%s%s%s%n%i%n",
        filename, protocol, busname, &n, addr, &n);
    if (items <= 0)
    {
        error("%s: Empty I/O link. "
            "Forgot the leading '@' or confused INP with OUT or link is too long ?\n",
            name());
        return S_dev_badInitRet;
    }
    if (items < 3)
    {
        error("%s: Wrong I/O link format\n"
            "  expect \"@file protocol bus addr params\"\n"
            "  in \"@%s\"\n", name(),
            ioLink->value.instio.string);
        return S_dev_badInitRet;
    }
    while (isspace((unsigned char)ioLink->value.instio.string[n])) n++;
    strcpy (busparam, ioLink->value.constantStr+n);
    return OK;
}

long Stream::
initRecord(const char* filename, const char* protocol,
    const char* busname, int addr, const char* busparam)
{
    // It is safe to call this function again with different arguments

    // attach to bus interface
    debug("Stream::initRecord %s: attachBus(%s, %d, \"%s\")\n",
        name(), busname, addr, busparam);
    if (!attachBus(busname, addr, busparam))
    {
        error("%s: Can't attach to bus %s %d\n",
            name(), busname, addr);
        return S_dev_noDevice;
    }

    // parse protocol file
    debug("Stream::initRecord %s: parse(%s, %s)\n",
        name(), filename, protocol);
    if (!parse(filename, protocol))
    {
        error("%s: Protocol parse error\n",
            name());
        return S_dev_noDevice;
    }

    // record is ready to use
    status = NO_ALARM;

    if (ioscanpvt)
    {
        // we have been called by streamReload
        debug("Stream::initRecord %s: initialize after streamReload\n",
            name());
        if (record->scan == SCAN_IO_EVENT)
        {
            debug("Stream::initRecord %s: "
                "restarting \"I/O Intr\" after reload\n",
                name());
            if (!startProtocol(StartAsync))
            {
                error("%s: Can't restart \"I/O Intr\" protocol\n",
                    name());
            }
        }
        return OK;
    }

    debug("Stream::initRecord %s: initialize the first time\n",
        name());

    if (!onInit) return DO_NOT_CONVERT; // no @init handler, keep DOL

    // initialize the record from hardware
    if (!startProtocol(StartInit))
    {
        error("%s: Can't start init run\n",
            name());
        return ERROR;
    }
    debug("Stream::initRecord %s: waiting for initDone\n",
        name());
#ifdef EPICS_3_13
    semTake(initDone, WAIT_FOREVER);
#else
    initDone.wait();
#endif
    debug("Stream::initRecord %s: initDone\n",
        name());

    // init run has set status and convert
    if (status != NO_ALARM)
    {
        record->stat = status;
        error("%s: @init handler failed\n",
            name());
        return ERROR;
    }
    debug("Stream::initRecord %s: initialized. convert=%d\n",
        name(), convert);
    return convert;
}

bool Stream::
process()
{
    MutexLock lock(this);
    if (record->pact || record->scan == SCAN_IO_EVENT)
    {
        if (status != NO_ALARM)
        {
            debug("Stream::process(%s) error status=%s (%d)\n",
                name(),
                status >= 0 && status < ALARM_NSTATUS ?
                    epicsAlarmConditionStrings[status] : "ERROR",
                status);
            (void) recGblSetSevr(record, status, INVALID_ALARM);
            return false;
        }
        debug("Stream::process(%s) ready. %s\n",
            name(), convert==2 ?
            "convert" : "don't convert");
        return true;
    }
    if (flags & InDestructor)
    {
        error("%s: Try to process while in stream destructor (try later)\n",
            name());
        (void) recGblSetSevr(record, UDF_ALARM, INVALID_ALARM);
        return false;
    }
    debug("Stream::process(%s) start\n", name());
    status = NO_ALARM;
    convert = OK;
    if (!startProtocol(StreamCore::StartNormal))
    {
        debug("Stream::process(%s): could not start, status=%d\n",
            name(), status);
        (void) recGblSetSevr(record, status, INVALID_ALARM);
        return false;
    }
    debug("Stream::process(%s): protocol started\n", name());
    record->pact = true;
    return true;
}

bool Stream::
print(format_t *format, va_list ap)
{
    switch (format->type)
    {
        case DBF_ULONG:
        case DBF_LONG:
        case DBF_ENUM:
            return printValue(*format->priv, va_arg(ap, long));
        case DBF_DOUBLE:
            return printValue(*format->priv, va_arg(ap, double));
        case DBF_STRING:
            return printValue(*format->priv, va_arg(ap, char*));
        default:
            error("INTERNAL ERROR (%s): Illegal format type %d\n",
                name(), format->type);
            return false;
    }
}

ssize_t Stream::
scan(format_t *format, void* value, size_t maxStringSize)
{
    // called by streamScanfN

    size_t size = maxStringSize;
    // first remove old value from inputLine (in case we are scanning arrays)
    consumedInput += currentValueLength;
    currentValueLength = 0;
    switch (format->type)
    {
        case DBF_ULONG:
        case DBF_LONG:
        case DBF_ENUM:
            currentValueLength = scanValue(*format->priv, *(long*)value);
            break;
        case DBF_DOUBLE:
            currentValueLength = scanValue(*format->priv, *(double*)value);
            break;
        case DBF_STRING:
            currentValueLength  = scanValue(*format->priv, (char*)value, size);
            break;
        default:
            error("INTERNAL ERROR (%s): Illegal format type %d\n",
                name(), format->type);
            return ERROR;
    }
    debug("Stream::scan() currentValueLength=%" Z "d\n", currentValueLength);
    if (currentValueLength < 0)
    {
        currentValueLength = 0; // important for arrays with less than NELM elements
        return ERROR;
    }
    // Don't remove scanned value from inputLine yet, because
    // we might need the string in a later error message.
    if (format->type == DBF_STRING) return size;
    return OK;
}

// epicsTimerNotify virtual method ///////////////////////////////////////

#ifdef EPICS_3_13
void Stream::
expire(CALLBACK *pcallback)
{
    Stream* pstream = static_cast<Stream*>(pcallback->user);
    pstream->timerCallback();
}
#else
epicsTimerNotify::expireStatus Stream::
expire(const epicsTime&)
{
    timerCallback();
    return noRestart;
}
#endif

// StreamCore virtual methods ////////////////////////////////////////////

void Stream::
protocolStartHook()
{
    flags &= ~Aborted;
}

void Stream::
protocolFinishHook(ProtocolResult result)
{
    switch (result)
    {
        case Success:
            status = NO_ALARM;
            if (flags & ValueReceived)
            {
                record->udf = false;
                if (flags & InitRun)
                {
                    // records start with UDF/INVALID,
                    // but now this record has a value
                    record->sevr = NO_ALARM;
                    record->stat = NO_ALARM;
                }
            }
            break;
        case LockTimeout:
        case ReplyTimeout:
            status = TIMEOUT_ALARM;
            break;
        case WriteTimeout:
            status = WRITE_ALARM;
            break;
        case ReadTimeout:
            status = READ_ALARM;
            break;
        case ScanError:
        case FormatError:
            status = CALC_ALARM;
            break;
        case Abort:
            flags |= Aborted;
        case Fault:
            status = UDF_ALARM;
            if (record->pact || record->scan == SCAN_IO_EVENT)
                error("%s: Protocol aborted\n", name());
            break;
        default:
            status = UDF_ALARM;
            error("INTERNAL ERROR (%s): Illegal protocol result\n",
                name());
            break;

    }
    if (flags & InitRun)
    {
#ifdef EPICS_3_13
        semGive(initDone);
#else
        initDone.signal();
#endif
        return;
    }

    if (result != Abort && record->scan == SCAN_IO_EVENT)
    {
        // re-enable early input
        flags |= AcceptInput;
    }

    if (record->pact || record->scan == SCAN_IO_EVENT)
    {
        // process record in callback thread to break possible recursion
        callbackSetPriority(priority(), &processCallback);
        callbackRequest(&processCallback);
    }

}

void streamRecordProcessCallback(CALLBACK *pcallback)
{
    Stream* pstream = static_cast<Stream*>(pcallback->user);
    dbCommon* record = pstream->record;

    // process record
    // This will call streamReadWrite.
    debug("streamRecordProcessCallback(%s) processing record\n",
            pstream->name());
    dbScanLock(record);
    ((DEVSUPFUN)record->rset->process)(record);
    dbScanUnlock(record);
    debug("streamRecordProcessCallback(%s) processing record done\n",
            pstream->name());

    if (record->scan == SCAN_IO_EVENT && !(pstream->flags & Aborted))
    {
        // restart protocol for next turn
        debug("streamRecordProcessCallback(%s) restart async protocol\n",
            pstream->name());
        if (!pstream->startProtocol(Stream::StartAsync))
        {
            error("%s: Can't restart \"I/O Intr\" protocol\n",
                pstream->name());
        }
    }
}

void Stream::
startTimer(unsigned long timeout)
{
    debug("Stream::startTimer(stream=%s, timeout=%lu) = %f seconds\n",
        name(), timeout, timeout * 0.001);
#ifdef EPICS_3_13
    callbackSetPriority(priority(), &timeoutCallback);
    wdStart(timer, (timeout+1)*sysClkRateGet()/1000-1,
        reinterpret_cast<FUNCPTR>(callbackRequest),
        reinterpret_cast<int>(&timeoutCallback));
#else
    timer->start(*this, timeout * 0.001);
#endif
}

bool Stream::
getFieldAddress(const char* fieldname, StreamBuffer& address)
{
    DBADDR dbaddr;
    if (strchr(fieldname, '.') != NULL)
    {
        // record.FIELD (access to other record)
        if (dbNameToAddr(fieldname, &dbaddr) != OK) return false;
    }
    else
    {
        // FIELD in this record or VAL in other record
        StreamBuffer fullname;
        fullname.print("%s.%s", name(), fieldname);
        if (dbNameToAddr(fullname(), &dbaddr) != OK)
        {
            // VAL in other record
            fullname.clear().print("%s.VAL", fieldname);
            if (dbNameToAddr(fullname(), &dbaddr) != OK) return false;
        }
    }
    address.append(&dbaddr, sizeof(dbaddr));
    return true;
}

static const unsigned char dbfMapping[] =
#ifdef DBF_INT64
    {0, DBF_UINT64, DBF_INT64, DBF_ENUM, DBF_DOUBLE, DBF_STRING};
#else
    {0, DBF_ULONG, DBF_LONG, DBF_ENUM, DBF_DOUBLE, DBF_STRING};
#endif

bool Stream::
formatValue(const StreamFormat& format, const void* fieldaddress)
{
    debug("Stream::formatValue(%s, format=%%%c, fieldaddr=%p\n",
        name(), format.conv, fieldaddress);

// --  TO DO: If SCAN is "I/O Intr" and record has not been processed,  --
// --  do it now to get the latest value (only for output records?)     --

    format_s fmt;
    fmt.type = dbfMapping[format.type];
    fmt.priv = &format;
    if (fieldaddress)
    {
        // Format like "%([record.]field)..." has requested to get value
        // from field of this or other record.
        StreamBuffer fieldBuffer;
        DBADDR* pdbaddr = (DBADDR*)fieldaddress;

        /* Handle time stamps special. %T converter takes double. */
        if (strcmp(((dbFldDes*)pdbaddr->pfldDes)->name, "TIME") == 0)
        {
            double time;

            if (format.type != double_format)
            {
                error ("%s: can only read double values from TIME field\n",
                    name());
                return false;
            }
            if (pdbaddr->precord == record)
            {
                /* if getting time from own record, update timestamp first */
                recGblGetTimeStamp(record);
            }
            /* convert EPICS epoch (1990) to unix epoch (1970) */
            /* we are losing about 3 digits precision here */
            time = pdbaddr->precord->time.secPastEpoch +
                631152000u + pdbaddr->precord->time.nsec * 1e-9;
            debug("Stream::formatValue(%s): read %f from TIME field\n",
                name(), time);
            return printValue(format, time);
        }

        /* convert type to LONG, ENUM, DOUBLE, or STRING */
        long nelem = pdbaddr->no_elements;
        size_t size = nelem * dbValueSize(fmt.type);

        /* print (U)CHAR arrays as string */
        if (format.type == string_format &&
            (pdbaddr->field_type == DBF_CHAR || pdbaddr->field_type == DBF_UCHAR))
        {
            debug("Stream::formatValue(%s): format %s.%s array[%ld] size %d of %s as string\n",
                name(),
                pdbaddr->precord->name,
                ((dbFldDes*)pdbaddr->pfldDes)->name,
                nelem,
                pdbaddr->field_size,
                pamapdbfType[pdbaddr->field_type].strvalue);
            fmt.type = DBF_CHAR;
            size = nelem;
        }

        char* buffer = fieldBuffer.clear().reserve(size);

        if (dbGet(pdbaddr, fmt.type, buffer,
            NULL, &nelem, NULL) != 0)
        {
            error("%s: dbGet(%s.%s, %s) failed\n",
                name(),
                pdbaddr->precord->name,
                ((dbFldDes*)pdbaddr->pfldDes)->name,
                pamapdbfType[dbfMapping[format.type]].strvalue);
            return false;
        }
        debug("Stream::formatValue(%s): got %ld elements\n",
                name(),nelem);

        /* terminate CHAR array as string */
        if (fmt.type == DBF_CHAR)
        {
            if (nelem >= pdbaddr->no_elements) nelem = pdbaddr->no_elements-1;
            buffer[nelem] = 0;
            nelem = 1; /* array is only 1 string */
        }

        long i;
        for (i = 0; i < nelem; i++)
        {
            switch (format.type)
            {
                case enum_format:
                    if (!printValue(format,
                        (long)((epicsUInt16*)buffer)[i]))
                        return false;
                    break;
                case unsigned_format:
                    if (!printValue(format,
                        (long)((epicsUInt32*)buffer)[i]))
                        return false;
                    break;
                case signed_format:
                    if (!printValue(format,
                        (long)((epicsInt32*)buffer)[i]))
                        return false;
                    break;
                case double_format:
                    if (!printValue(format,
                        (double)((epicsFloat64*)buffer)[i]))
                        return false;
                    break;
                case string_format:
                    if (!printValue(format, buffer+MAX_STRING_SIZE*i))
                        return false;
                    break;
                case pseudo_format:
                    error("%s: %%(FIELD) syntax not allowed "
                        "with pseudo formats\n",
                        name());
                    return false;
                default:
                    error("INTERNAL ERROR %s: Illegal format.type=%d\n",
                        name(), format.type);
                    return false;
            }
        }
        return true;
    }
    debug("Stream::formatValue(%s) format=%%%c type=%s\n",
        name(), format.conv, pamapdbfType[fmt.type].strvalue);
    if (!writeData)
    {
        error("%s: No writeData() function provided\n", name());
        return false;
    }
    if (writeData(record, &fmt) == ERROR)
    {
        debug("Stream::formatValue(%s): writeData failed\n", name());
        return false;
    }
    return true;
}

bool Stream::
matchValue(const StreamFormat& format, const void* fieldaddress)
{
    // this function must increase consumedInput
    ssize_t consumed = 0;
    long lval;
    double dval;
    char* buffer;
    int status;
    const char* putfunc;
    format_s fmt;
    size_t stringsize = MAX_STRING_SIZE;

    fmt.type = dbfMapping[format.type];
    fmt.priv = &format;
    if (fieldaddress)
    {
        // Format like "%([record.]field)..." has requested to put value
        // to field of this or other record.
        StreamBuffer fieldBuffer;
        DBADDR* pdbaddr = (DBADDR*)fieldaddress;
        size_t size;
        size_t nord;
        size_t nelem = pdbaddr->no_elements;
        if (format.type == string_format &&
            (pdbaddr->field_type == DBF_CHAR || pdbaddr->field_type == DBF_UCHAR))
        {
            // string to char array
            size = nelem;
        }
        else
            size = nelem * dbValueSize(fmt.type);
        buffer = fieldBuffer.clear().reserve(size);  // maybe write to field directly in case types match?
        for (nord = 0; nord < nelem; nord++)
        {
            debug("Stream::matchValue(%s): buffer before: %s\n",
                name(), fieldBuffer.expand()());
            switch (format.type)
            {
                case unsigned_format:
                {
                    consumed = scanValue(format, lval);
                    if (consumed >= 0) ((epicsUInt32*)buffer)[nord] = lval;
                    debug("Stream::matchValue(%s): %s.%s[%" Z "u] = %lu\n",
                            name(), pdbaddr->precord->name,
                            ((dbFldDes*)pdbaddr->pfldDes)->name,
                            nord, lval);
                    break;
                }
                case signed_format:
                {
                    consumed = scanValue(format, lval);
                    if (consumed >= 0) ((epicsInt32*)buffer)[nord] = lval;
                    debug("Stream::matchValue(%s): %s.%s[%" Z "u] = %li\n",
                            name(), pdbaddr->precord->name,
                            ((dbFldDes*)pdbaddr->pfldDes)->name,
                            nord, lval);
                    break;
                }
                case enum_format:
                {
                    consumed = scanValue(format, lval);
                    if (consumed >= 0)
                        ((epicsUInt16*)buffer)[nord] = (epicsUInt16)lval;
                    debug("Stream::matchValue(%s): %s.%s[%" Z "u] = %li\n",
                            name(), pdbaddr->precord->name,
                            ((dbFldDes*)pdbaddr->pfldDes)->name,
                            nord, lval);
                    break;
                }
                case double_format:
                {
                    consumed = scanValue(format, dval);
                    // Direct assignment to buffer fails with
                    // gcc 3.4.3 for xscale_be
                    // Optimization bug?
                    epicsFloat64 f64=dval;
                    if (consumed >= 0)
                        memcpy(((epicsFloat64*)buffer)+nord,
                            &f64, sizeof(f64));
                    debug("Stream::matchValue(%s): %s.%s[%" Z "u] = %#g %#g\n",
                            name(), pdbaddr->precord->name,
                            ((dbFldDes*)pdbaddr->pfldDes)->name,
                            nord, dval,
                            ((epicsFloat64*)buffer)[nord]);
                    break;
                }
                case string_format:
                {
                    if (pdbaddr->field_type == DBF_CHAR ||
                        pdbaddr->field_type == DBF_UCHAR)
                    {
                        // string to char array
                        stringsize = nelem;
                        consumed = scanValue(format, buffer, stringsize);
                        debug("Stream::matchValue(%s): %s.%s = \"%.*s\"\n",
                                name(), pdbaddr->precord->name,
                                ((dbFldDes*)pdbaddr->pfldDes)->name,
                                (int)consumed, buffer);
                        nord = nelem; // this shortcuts the loop
                    }
                    else
                    {
                        stringsize = MAX_STRING_SIZE;
                        consumed = scanValue(format,
                            buffer+MAX_STRING_SIZE*nord, stringsize);
                        debug("Stream::matchValue(%s): %s.%s[%" Z "u] = \"%.*s\"\n",
                                name(), pdbaddr->precord->name,
                                ((dbFldDes*)pdbaddr->pfldDes)->name,
                                nord, (int)stringsize, buffer+MAX_STRING_SIZE*nord);
                    }
                    break;
                }
                default:
                    error("INTERNAL ERROR: Stream::matchValue %s: "
                        "Illegal format type\n", name());
                    return false;
            }
            debug("Stream::matchValue(%s): buffer after: %s\n",
                name(), fieldBuffer.expand()());
            if (consumed < 0) break;
            consumedInput += consumed;
        }
        if (!nord)
        {
            // scan error: set other record to alarm status
            if (pdbaddr->precord != record)
            {
                (void) recGblSetSevr(pdbaddr->precord,
                    CALC_ALARM, INVALID_ALARM);
                if (!INIT_RUN)
                {
                    // process other record to send alarm monitor
                    dbProcess(pdbaddr->precord);
                }
            }
            return false;
        }
        if (strcmp(((dbFldDes*)pdbaddr->pfldDes)->name, "TIME") == 0)
        {
#ifdef epicsTimeEventDeviceTime
            if (format.type != double_format)
            {
                error ("%s: can only write double values to TIME field\n",
                    name());
                return false;
            }
            /* convert from Unix epoch (1 Jan 1970) to EPICS epoch (1 Jan 1990) */
            dval = dval-631152000u;
            pdbaddr->precord->time.secPastEpoch = (long)dval;
            // rounding: we don't have 9 digits precision
            // in a double of today's number of seconds
            pdbaddr->precord->time.nsec = (long)((dval-(long)dval)*1e6)*1000;
            debug("Stream::matchValue(%s): writing %i.%i to %s.TIME field\n",
                name(),
                pdbaddr->precord->time.secPastEpoch,
                pdbaddr->precord->time.nsec,
                pdbaddr->precord->name);
            pdbaddr->precord->tse = epicsTimeEventDeviceTime;
            return true;
#else
            error ("%s: writing TIME field is not supported "
                "in this EPICS version\n", name());
            return false;
#endif
        }
        if (format.type == string_format &&
            (pdbaddr->field_type == DBF_CHAR ||
            pdbaddr->field_type == DBF_UCHAR))
        {
            /* write strings to [U]CHAR arrays */
            nord = stringsize;
            fmt.type = DBF_CHAR;
        }
        if (pdbaddr->precord == record || INIT_RUN)
        {
            // write into own record, thus don't process it
            // in @init we must not process other record
            putfunc = "dbPut";
            status = dbPut(pdbaddr, fmt.type, buffer, (long)nord);
            if (INIT_RUN && pdbaddr->precord != record)
            {
                // clean error status of other record in @init
                pdbaddr->precord->udf = false;
                pdbaddr->precord->sevr = NO_ALARM;
                pdbaddr->precord->stat = NO_ALARM;
            }
        }
        else
        {
            // write into other record, thus process it
            putfunc = "dbPutField";
            status = dbPutField(pdbaddr, fmt.type,
                buffer, (long)nord);
        }
        debug("Stream::matchValue(%s): %s(%s.%s, %s, %s) status=0x%x\n",
            name(), putfunc,
            pdbaddr->precord->name,
            ((dbFldDes*)pdbaddr->pfldDes)->name,
            pamapdbfType[fmt.type].strvalue,
            fieldBuffer.expand()(),
            status);
        if (status != 0)
        {
            flags &= ~ScanTried;
            switch (fmt.type)
            {
                case DBF_ULONG:
                case DBF_LONG:
                case DBF_ENUM:
                    error("%s: %s(%s.%s, %s, %li, %" Z "u) failed\n",
                        name(), putfunc, pdbaddr->precord->name,
                        ((dbFldDes*)pdbaddr->pfldDes)->name,
                        pamapdbfType[fmt.type].strvalue,
                        lval, nord);
                    return false;
                case DBF_DOUBLE:
                    error("%s: %s(%s.%s, %s, %#g, %" Z "u) failed\n",
                        name(), putfunc, pdbaddr->precord->name,
                        ((dbFldDes*)pdbaddr->pfldDes)->name,
                        pamapdbfType[fmt.type].strvalue,
                        dval, nord);
                    return false;
                case DBF_STRING:
                case DBF_CHAR:
                    error("%s: %s(%s.%s, %s, \"%.*s\", %" Z "u) failed\n",
                        name(), putfunc, pdbaddr->precord->name,
                        ((dbFldDes*)pdbaddr->pfldDes)->name,
                        pamapdbfType[fmt.type].strvalue,
                        (int)consumed, buffer, nord);
                    return false;
                default:
                    return false;
            }
        }
        return true;
    }
    // no fieldaddress (the "normal" case)
    if (!readData)
    {
        error("%s: No readData() function provided\n", name());
        return false;
    }
    currentValueLength = 0;
    convert = readData(record, &fmt); // this will call scan()
    if (convert == ERROR)
    {
        debug("Stream::matchValue(%s): readData failed\n", name());
        if (currentValueLength > 0)
        {
            error("%s: Record does not accept input \"%s%s\"\n",
                name(), inputLine.expand(consumedInput, 19)(),
                inputLine.length()-consumedInput > 20 ? "..." : "");
            flags &= ~ScanTried;
        }
        return false;
    }
    flags |= ValueReceived;
    consumedInput += currentValueLength;
    debug("Stream::matchValue(%s): success, %" Z "d bytes consumed\n", name(), currentValueLength);
    return true;
}

#ifdef EPICS_3_13
// Pass command to vxWorks shell
extern "C" int execute(const char *cmd);

void streamExecuteCommand(CALLBACK *pcallback)
{
    Stream* pstream = static_cast<Stream*>(pcallback->user);

    if (execute(pstream->outputLine()) != OK)
    {
        pstream->execCallback(StreamIoFault);
    }
    else
    {
        pstream->execCallback(StreamIoSuccess);
    }
}
#else
// Pass command to iocsh
void streamExecuteCommand(CALLBACK *pcallback)
{
    Stream* pstream = static_cast<Stream*>(pcallback->user);

    if (iocshCmd(pstream->outputLine()) != OK)
    {
        pstream->execCallback(StreamIoFault);
    }
    else
    {
        pstream->execCallback(StreamIoSuccess);
    }
}
#endif

bool Stream::
execute()
{
    callbackSetPriority(priority(), &commandCallback);
    callbackRequest(&commandCallback);
    return true;
}

void Stream::
lockMutex()
{
#ifdef EPICS_3_13
    semTake(mutex, WAIT_FOREVER);
#else
    mutex.lock();
#endif
}

void Stream::
releaseMutex()
{
#ifdef EPICS_3_13
    semGive(mutex);
#else
    mutex.unlock();
#endif
}
