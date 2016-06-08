/*
 *  mms_journal_service.c
 *
 *  Copyright 2016 Michael Zillgith
 *
 *  This file is part of libIEC61850.
 *
 *  libIEC61850 is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  libIEC61850 is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with libIEC61850.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  See COPYING file for the complete license text.
 */

#include "libiec61850_platform_includes.h"
#include "mms_server_internal.h"
#include "mms_value_internal.h"

#if (MMS_JOURNAL_SERVICE == 1)


typedef struct sJournalVariable* JournalVariable;

struct sJournalVariable {
   char* tag; /* UTF8(1..255) */
   int tagSize;

   uint8_t* data;
   int dataSize;

   JournalVariable next;
};

typedef struct {
    uint8_t* entryID;
    int entryIDSize;

    uint64_t timestamp;

    JournalVariable listOfVariables;
} JournalEntry;


typedef struct sJournalEncoder* JournalEncoder;

struct sJournalEncoder {
    uint8_t* buffer;
    int maxSize;
    int bufPos;
    int currentEntryBufPos; /* store start buffer position of current entry in case the whole JournalEntry will become too long */
    uint64_t currentEntryId; //TODO use a byte array for the generic MMS case!
    uint64_t currentTimestamp;
    bool moreFollows;
};

static bool
entryCallback(void* parameter, uint64_t timestamp, uint64_t entryID, bool moreFollow)
{
    JournalEncoder encoder = (JournalEncoder) parameter;

    if (moreFollow) {

        if (encoder->moreFollows) {
            printf("entryCallback return false\n");
            return false;
        }

        encoder->currentEntryBufPos = encoder->bufPos;

        encoder->bufPos += 48; /* reserve space for common entry parts */

        encoder->currentEntryId = entryID;
        encoder->currentTimestamp = timestamp;

    }
    else {
        printf("Encoded last entry: FINISHED\n");
    }

    return true;
}

static const char* REASON_CODE_STR = "ReasonCode";

static bool
entryDataCallback (void* parameter, const char* dataRef, uint8_t* data, int dataSize, uint8_t reasonCode, bool moreFollow)
{
    JournalEncoder encoder = (JournalEncoder) parameter;

    uint8_t* buffer = encoder->buffer;

    //TODO check if entry is too long for buffer!

    if (moreFollow) {
        int bufPos = encoder->bufPos;

        uint32_t dataRefStrLen = strlen(dataRef);
        uint32_t dataRefLen = 1 + BerEncoder_determineLengthSize(dataRefStrLen) + dataRefStrLen;

        uint32_t valueSpecLen = 1 + BerEncoder_determineLengthSize(dataSize) + dataSize;

        uint32_t firstVariableLen = 1 + BerEncoder_determineLengthSize(valueSpecLen + dataRefLen)
                + valueSpecLen + dataRefLen;

        uint8_t reasonCodeNBuf[2];
        MmsValue reasonCodeValue;
        reasonCodeValue.type = MMS_BIT_STRING;
        reasonCodeValue.value.bitString.size = 7;
        reasonCodeValue.value.bitString.buf = reasonCodeNBuf;

        MmsValue_setBitStringFromInteger(&reasonCodeValue, reasonCode);

        uint32_t reasonCodeValueLen = MmsValue_encodeMmsData(&reasonCodeValue, NULL, 0, false);

        uint32_t reasonCodeContentLen = reasonCodeValueLen + 2 + 12;

        uint32_t secondVariableLen = 1 + BerEncoder_determineLengthSize(reasonCodeContentLen)
                + reasonCodeContentLen;

        uint32_t totalLen = firstVariableLen + secondVariableLen;

        if ((int) (bufPos + totalLen) > encoder->maxSize) {
            encoder->moreFollows = true;
            encoder->bufPos = encoder->currentEntryBufPos; /* remove last entry */
            return false;
        }

        bufPos = BerEncoder_encodeTL(0x30, valueSpecLen + dataRefLen, buffer, bufPos);

        /* encode dataRef */
        bufPos = BerEncoder_encodeOctetString(0x80, (uint8_t*) dataRef, dataRefStrLen, buffer, bufPos);

        /* encode valueSpec */
        bufPos = BerEncoder_encodeOctetString(0xa1, data, dataSize, buffer, bufPos);

        /* encode reasonCode */

        bufPos = BerEncoder_encodeTL(0x30, reasonCodeContentLen , buffer, bufPos);

        bufPos = BerEncoder_encodeOctetString(0x80, (uint8_t*) REASON_CODE_STR, 10, buffer, bufPos);

        bufPos = BerEncoder_encodeTL(0xa1, reasonCodeValueLen, buffer, bufPos);
        bufPos = MmsValue_encodeMmsData(&reasonCodeValue, buffer, bufPos, true);

        encoder->bufPos = bufPos;
    }
    else {
        int dataContentLen = encoder->bufPos - (encoder->currentEntryBufPos + 48);

        int journalVariablesLen = 1  + BerEncoder_determineLengthSize(dataContentLen) + dataContentLen;

        int dataLen = 1 + BerEncoder_determineLengthSize(journalVariablesLen) + journalVariablesLen;

        int dataAndTimeLen = dataLen + 8;

        int entryContentLen = 1 + BerEncoder_determineLengthSize(dataAndTimeLen) + dataAndTimeLen;

        int journalEntryContentLen =  10 /* entryIdentifier */
                                    +  4 /* originatingApplication */
                                    + entryContentLen;

        int headerBufPos = encoder->currentEntryBufPos;

        headerBufPos = BerEncoder_encodeTL(0x30, journalEntryContentLen, buffer, headerBufPos);

        headerBufPos = BerEncoder_encodeOctetString(0x80, (uint8_t*) &(encoder->currentEntryId), 8, buffer, headerBufPos);

        headerBufPos = BerEncoder_encodeTL(0xa1, 2, buffer, headerBufPos);
        headerBufPos = BerEncoder_encodeTL(0x30, 0, buffer, headerBufPos);

        headerBufPos = BerEncoder_encodeTL(0xa2, dataAndTimeLen, buffer, headerBufPos);

        MmsValue occurenceTime;
        occurenceTime.type = MMS_BINARY_TIME;
        occurenceTime.value.binaryTime.size = 6;
        MmsValue_setBinaryTime(&occurenceTime, encoder->currentTimestamp);
        headerBufPos = BerEncoder_encodeOctetString(0x80, occurenceTime.value.binaryTime.buf, 6, buffer, headerBufPos);

        headerBufPos = BerEncoder_encodeTL(0xa2, journalVariablesLen, buffer, headerBufPos);
        headerBufPos = BerEncoder_encodeTL(0xa1, dataContentLen, buffer, headerBufPos);

        int entryHeaderLength =  headerBufPos - encoder->currentEntryBufPos;

        /* move data to entry header */
        memmove(buffer + (encoder->currentEntryBufPos + entryHeaderLength), buffer + (encoder->currentEntryBufPos + 48),
                dataContentLen);

        /* prepare buffer for next entry. */
        encoder->bufPos = encoder->currentEntryBufPos + entryHeaderLength + dataContentLen;
    }

    return true;
}

static bool
parseStringWithMaxLength(char* filename, int maxLength, uint8_t* buffer, int* bufPos, int maxBufPos , uint32_t invokeId, ByteBuffer* response)
{
    uint8_t tag = buffer[(*bufPos)++];
    int length;

    if (tag != 0x1a) {
      mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_INVALID_PDU, response);
      return false;
    }

    *bufPos = BerDecoder_decodeLength(buffer, &length, *bufPos, maxBufPos);

    if (*bufPos < 0)  {
      mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_INVALID_PDU, response);
      return false;
    }

    if (length > maxLength) {
      mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_REQUEST_INVALID_ARGUMENT, response);
      return false;
    }

    memcpy(filename, buffer + *bufPos, length);
    filename[length] = 0;
    *bufPos += length;

    return true;
}

#define RESERVED_SPACE_FOR_HEADER 22

void
mmsServer_handleReadJournalRequest(
        MmsServerConnection connection,
        uint8_t* requestBuffer,
        int bufPos, int maxBufPos,
        uint32_t invokeId,
        ByteBuffer* response)
{
    char domainId[65];
    char logName[65];
    uint8_t entryIdBuf[64]; /* maximum size of entry id is 64 bytes! */

    MmsValue entrySpec;
    entrySpec.type = MMS_OCTET_STRING;
    entrySpec.value.octetString.buf = entryIdBuf;
    entrySpec.value.octetString.maxSize = 64;

    MmsValue rangeStart;
    MmsValue rangeStop;

    bool hasNames = false;
    bool hasRangeStartSpec = false;
    bool hasRangeStopSpec = false;
    bool hasTimeSpec = false;
    bool hasEntrySpec = false;

    while (bufPos < maxBufPos) {
        uint8_t tag = requestBuffer[bufPos++];
        int length;

        bufPos = BerDecoder_decodeLength(requestBuffer, &length, bufPos, maxBufPos);

        if (bufPos < 0)  {
            mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_INVALID_PDU, response);
            return;
        }

        switch (tag) {

        case 0xa0: /* journalName */
            {
                uint8_t objectIdTag = requestBuffer[bufPos++];
                bufPos = BerDecoder_decodeLength(requestBuffer, &length, bufPos, maxBufPos);

                switch (objectIdTag) {
                case 0xa1: /* domain-specific */

                    if (!parseStringWithMaxLength(domainId, 64, requestBuffer, &bufPos, bufPos + length, invokeId, response)) {
                        return;
                    }

                    if (!parseStringWithMaxLength(logName, 64, requestBuffer, &bufPos, bufPos + length, invokeId, response)) {
                        return;
                    }

                    hasNames = true;

                    break;

                default:
                    mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_UNRECOGNIZED_MODIFIER, response);
                    return;

                }
            }

            break;

        case 0xa1: /* rangeStartSpecification */
            {
                uint8_t subTag = requestBuffer[bufPos++];
                bufPos = BerDecoder_decodeLength(requestBuffer, &length, bufPos, maxBufPos);

                if (subTag != 0x80) {
                    mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_UNRECOGNIZED_MODIFIER, response);
                    return;
                }


                if ((length == 4) || (length == 6)) {

                    rangeStart.type = MMS_BINARY_TIME;
                    rangeStart.value.binaryTime.size = length;

                    memcpy(rangeStart.value.binaryTime.buf, requestBuffer + bufPos, length);

                    char stringBuf[100];
                    MmsValue_printToBuffer(&rangeStart, stringBuf, 100);

                    hasRangeStartSpec = true;
                }
                else {
                    mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_REQUEST_INVALID_ARGUMENT, response);
                    return;    // forward request to implementation class
                }

                bufPos += length;
            }

            break;

        case 0xa2: /* rangeStopSpecification */
            {
                uint8_t subTag = requestBuffer[bufPos++];
                bufPos = BerDecoder_decodeLength(requestBuffer, &length, bufPos, maxBufPos);

                if (subTag != 0x80) {
                    mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_UNRECOGNIZED_MODIFIER, response);
                    return;
                }

                if ((length == 4) || (length == 6)) {
                    rangeStop.type = MMS_BINARY_TIME;
                    rangeStop.value.binaryTime.size = length;

                    memcpy(rangeStop.value.binaryTime.buf, requestBuffer + bufPos, length);

                    char stringBuf[100];
                    MmsValue_printToBuffer(&rangeStop, stringBuf, 100);

                    hasRangeStopSpec = true;
                }
                else {
                    mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_REQUEST_INVALID_ARGUMENT, response);
                    return;    // forward request to implementation class
                }

                bufPos += length;
            }

            break;

        case 0xa5: /* entryToStartAfter */
            {
                int maxSubBufPos = bufPos + length;

                while (bufPos < maxSubBufPos) {
                    uint8_t subTag = requestBuffer[bufPos++];
                    bufPos = BerDecoder_decodeLength(requestBuffer, &length, bufPos, maxBufPos);

                    switch (subTag) {
                    case 0x80: /* timeSpecification */

                        if ((length == 4) || (length == 6)) {
                            rangeStart.type = MMS_BINARY_TIME;
                            rangeStart.value.binaryTime.size = length;

                            memcpy(rangeStart.value.binaryTime.buf, requestBuffer + bufPos, length);

                            char stringBuf[100];
                            MmsValue_printToBuffer(&rangeStart, stringBuf, 100);

                            hasTimeSpec = true;
                        }
                        else {
                            mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_REQUEST_INVALID_ARGUMENT, response);
                            return;
                        }

                        break;

                    case 0x81: /* entrySpecification */

                        if (length <= entrySpec.value.octetString.maxSize) {
                            memcpy(entrySpec.value.octetString.buf, requestBuffer + bufPos, length);
                            entrySpec.value.octetString.size = length;

                            hasEntrySpec = true;
                        }
                        else {
                            mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_REQUEST_INVALID_ARGUMENT, response);
                            return;
                        }

                        break;

                    default:
                        mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_UNRECOGNIZED_MODIFIER, response);
                        return;
                    }

                    bufPos += length;
                }

            }
            break;

        default:
            mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_INVALID_PDU, response);
            return;
        }
    }

    //TODO check if required fields are present
    if (hasNames == false) {
        printf("MMS_SERVER: readJournal missing journal name\n");
        mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_REQUEST_INVALID_ARGUMENT, response);
        return;
    }

    //TODO check valid field combinations

    /* lookup journal */
    MmsDevice* mmsDevice = MmsServer_getDevice(connection->server);

    MmsDomain* mmsDomain = MmsDevice_getDomain(mmsDevice, domainId);

    if (mmsDomain == NULL) {
        printf("Domain %s not found\n", domainId);
        mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_REQUEST_INVALID_ARGUMENT, response);
        return;
    }

    MmsJournal mmsJournal = MmsDomain_getJournal(mmsDomain, logName);

    if (mmsJournal == NULL) {
        printf("Journal %s not found\n", logName);
        mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_REQUEST_INVALID_ARGUMENT, response);
        return;
    }

    printf("Read journal %s ...\n", mmsJournal->name);

    struct sJournalEncoder encoder;

    encoder.buffer = response->buffer;
    encoder.moreFollows = false;
    encoder.maxSize = connection->maxPduSize - 3; /* reserve three bytes for moreFollows */
    encoder.bufPos = RESERVED_SPACE_FOR_HEADER; /* reserve space for header */

    LogStorage logStorage = mmsJournal->logStorage;

    if (logStorage != NULL) {

        if (hasRangeStartSpec && hasRangeStopSpec) {
            LogStorage_getEntries(logStorage, MmsValue_getBinaryTimeAsUtcMs(&rangeStart), MmsValue_getBinaryTimeAsUtcMs(&rangeStop),
                    entryCallback, entryDataCallback, &encoder);
        }
        else if (hasEntrySpec && hasTimeSpec) {
            LogStorage_getEntriesAfter(logStorage, MmsValue_getBinaryTimeAsUtcMs(&rangeStart), *((uint64_t*) entryIdBuf),
                    entryCallback, entryDataCallback, &encoder);
        }
        else {
            printf("MMS_SERVER: readJournal missing valid argument combination\n");
            mmsServer_writeMmsRejectPdu(&invokeId, MMS_ERROR_REJECT_REQUEST_INVALID_ARGUMENT, response);
            return;
        }

    }
    /* actual encoding will happen in callback handler. When getEntries returns the data is
     * already encoded in the buffer.
     */

    /* encode message header in response buffer */

    uint8_t* buffer = encoder.buffer;
    bufPos = 0;

    int dataSize = encoder.bufPos - RESERVED_SPACE_FOR_HEADER;

    uint32_t invokeIdSize = BerEncoder_UInt32determineEncodedSize((uint32_t) invokeId) + 2;
    uint32_t listOfEntriesLen = 1 + BerEncoder_determineLengthSize(dataSize) + dataSize;

    uint32_t moreFollowsLen;

    if (encoder.moreFollows)
        moreFollowsLen = 3;
    else
        moreFollowsLen = 0;

    uint32_t readJournalLen = 2 + BerEncoder_determineLengthSize(listOfEntriesLen + moreFollowsLen) +
             (listOfEntriesLen + moreFollowsLen);

    uint32_t confirmedResponsePDUSize = readJournalLen + invokeIdSize;

    bufPos = BerEncoder_encodeTL(0xa1, confirmedResponsePDUSize, buffer, bufPos);

    bufPos = BerEncoder_encodeTL(0x02, invokeIdSize - 2, buffer, bufPos);
    bufPos = BerEncoder_encodeUInt32((uint32_t) invokeId, buffer, bufPos);

    buffer[bufPos++] = 0xbf;
    buffer[bufPos++] = 0x41;

    bufPos = BerEncoder_encodeLength(listOfEntriesLen + moreFollowsLen, buffer, bufPos);

    bufPos = BerEncoder_encodeTL(0xa0, dataSize, buffer, bufPos);

    /* move encoded JournalEntry data to continue the buffer header */

    if (DEBUG_MMS_SERVER)
        printf("MMS_SERVER: readJournal: Encoded message header with %i bytes\n", bufPos);

    memmove(buffer + bufPos, buffer + RESERVED_SPACE_FOR_HEADER, dataSize);

    bufPos = bufPos + dataSize;

    /* encode morefollows */
    if (encoder.moreFollows)
        bufPos = BerEncoder_encodeBoolean(0x81, true, buffer, bufPos);

    response->size = bufPos;
}

#endif /* (MMS_JOURNAL_SERVICE == 1) */
