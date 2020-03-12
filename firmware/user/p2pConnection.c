// Change to test for con broadcast look for msg con rather than length of message
// TODO side and otherSide could be renamed socket and otherSocket

/*============================================================================
 * Includes
 *==========================================================================*/

#include <osapi.h>
#include <user_interface.h>
#include <mem.h>

#include "user_main.h"
#include "p2pConnection.h"

/*============================================================================
 * Defines
 *==========================================================================*/

#define P2P_DEBUG_PRINT
#ifdef P2P_DEBUG_PRINT
    #define p2p_printf(...) do{os_printf("%s::%d ", __func__, __LINE__); os_printf(__VA_ARGS__);}while(0)
#else
    #define p2p_printf(...)
#endif

// The time we'll spend retrying messages
#define RETRY_TIME_MS 3000

// Time to wait between connection events and game rounds.
// Transmission can be 3s (see above), the round @ 12ms period is 3.636s
// (240 steps of rotation + (252/4) steps of decay) * 12ms
#define FAILURE_RESTART_MS 8000

// Indices into messages
#define CMD_IDX 4
#define SEQ_IDX 8
#define MAC_IDX 11
#define EXT_IDX 29

/*============================================================================
 * Variables
 *==========================================================================*/

// Messages to send.
const char p2pConnectionMsgFmt[] = "%s_con_%1X";

// Needs to be 31 chars or less!
const char p2pNoPayloadMsgFmt[]  = "%s_%s_%02d_%02X:%02X:%02X:%02X:%02X:%02X_%1X%1X";

// Needs to be 63 chars or less!
const char p2pPayloadMsgFmt[]    = "%s_%s_%02d_%02X:%02X:%02X:%02X:%02X:%02X_%1X%1X_%s";
const char p2pMacFmt[] = "%02X:%02X:%02X:%02X:%02X:%02X";

/*============================================================================
 * Function Prototypes
 *==========================================================================*/

void ICACHE_FLASH_ATTR p2pConnectionTimeout(void* arg);
void ICACHE_FLASH_ATTR p2pTxAllRetriesTimeout(void* arg);
void ICACHE_FLASH_ATTR p2pTxRetryTimeout(void* arg);
void ICACHE_FLASH_ATTR p2pStartRestartTimer(void* arg);
void ICACHE_FLASH_ATTR p2pProcConnectionEvt(p2pInfo* p2p, connectionEvt_t event);
void ICACHE_FLASH_ATTR p2pGameStartAckRecv(void* arg);
void ICACHE_FLASH_ATTR p2pSendAckToMac(p2pInfo* p2p, uint8_t* mac_addr);
void ICACHE_FLASH_ATTR p2pSendMsgEx(p2pInfo* p2p, char* msg, uint16_t len,
                                    bool shouldAck, void (*success)(void*), void (*failure)(void*));
void ICACHE_FLASH_ATTR p2pModeMsgSuccess(void* arg);
void ICACHE_FLASH_ATTR p2pModeMsgFailure(void* arg);

/*============================================================================
 * Functions
 *==========================================================================*/

/**
 * @brief Initialize the p2p connection protocol
 *
 * @param p2p           The p2pInfo struct with all the state information
 * @param msgId         A three character, null terminated message ID. Must be
 *                      unique among all swadge modes.
 * @param conCbFn A function pointer which will be called when connection
 *                      events occur
 * @param msgRxCbFn A function pointer which will be called when a packet
 *                      is received for the swadge mode
 * @param connectionRssi The strength needed to start a connection with another
 *                      swadge, 0 is first one to see around 55 the swadges need
 *                      to be right next to eachother.
 */
void ICACHE_FLASH_ATTR p2pInitialize(p2pInfo* p2p, char* msgId,
                                     p2pConCbFn conCbFn,
                                     p2pMsgRxCbFn msgRxCbFn, uint8_t connectionRssi)
{
    p2p_printf("%s\r\n", msgId);
    // Make sure everything is zero!
    ets_memset(p2p, 0, sizeof(p2pInfo));
    // Set the callback functions for connection and message events
    p2p->conCbFn = conCbFn;
    p2p->msgRxCbFn = msgRxCbFn;

    // Set the initial sequence number at 255 so that a 0 received is valid.
    p2p->cnc.lastSeqNum = 255;

    // Set the connection Rssi, the higher the value, the closer the swadges
    // need to be.
    p2p->connectionRssi = connectionRssi;

    // Set the three character message ID
    ets_strncpy(p2p->msgId, msgId, sizeof(p2p->msgId));

    // Get and save the string form of our MAC address
    uint8_t mymac[6];
    wifi_get_macaddr(SOFTAP_IF, mymac);
    ets_snprintf(p2p->cnc.macStr, sizeof(p2p->cnc.macStr), p2pMacFmt,
                 mymac[0],
                 mymac[1],
                 mymac[2],
                 mymac[3],
                 mymac[4],
                 mymac[5]);

    // Set up the connection message
    ets_snprintf(p2p->conMsg, sizeof(p2p->conMsg), p2pConnectionMsgFmt,
                 p2p->msgId, 0);

    // Set up dummy ACK message
    ets_snprintf(p2p->ackMsg, sizeof(p2p->ackMsg), p2pNoPayloadMsgFmt,
                 p2p->msgId,
                 "ack",
                 0,
                 0xFF,
                 0xFF,
                 0xFF,
                 0xFF,
                 0xFF,
                 0xFF,
                 0,
                 0);

    // Set up dummy start message
    ets_snprintf(p2p->startMsg, sizeof(p2p->startMsg), p2pNoPayloadMsgFmt,
                 p2p->msgId,
                 "str",
                 0,
                 0xFF,
                 0xFF,
                 0xFF,
                 0xFF,
                 0xFF,
                 0xFF,
                 0,
                 0);

    // Set up a timer for acking messages
    os_timer_disarm(&p2p->tmr.TxRetry);
    os_timer_setfn(&p2p->tmr.TxRetry, p2pTxRetryTimeout, p2p);

    // Set up a timer for when a message never gets ACKed
    os_timer_disarm(&p2p->tmr.TxAllRetries);
    os_timer_setfn(&p2p->tmr.TxAllRetries, p2pTxAllRetriesTimeout, p2p);

    // Set up a timer to restart after abject failure
    os_timer_disarm(&p2p->tmr.Reinit);
    os_timer_setfn(&p2p->tmr.Reinit, p2pRestart, p2p);

    // Set up a timer to do an initial connection
    os_timer_disarm(&p2p->tmr.Connection);
    os_timer_setfn(&p2p->tmr.Connection, p2pConnectionTimeout, p2p);
}

/**
 * Start the connection process by sending broadcasts and notify the mode
 *
 * @param p2p The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pStartConnection(p2pInfo* p2p)
{
    p2p_printf("%s\r\n", p2p->msgId);
    p2p->cnc.isConnecting = true;
    os_timer_arm(&p2p->tmr.Connection, 1, false);

    if(NULL != p2p->conCbFn)
    {
        p2p->conCbFn(p2p, CON_STARTED);
    }
}

/**
 * Stop a connection in progress. If the connection is already established,
 * this does nothing
 *
 * @param p2p The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pStopConnection(p2pInfo* p2p)
{
    if(true == p2p->cnc.isConnecting)
    {
        p2p_printf("%s\r\n", p2p->msgId);
        p2p->cnc.isConnecting = false;
        os_timer_disarm(&p2p->tmr.Connection);

        if(NULL != p2p->conCbFn)
        {
            p2p->conCbFn(p2p, CON_STOPPED);
        }

        p2pRestart((void*)p2p);
    }
}

/**
 * Stop up all timers and clear out p2p
 *
 * @param p2p The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pDeinit(p2pInfo* p2p)
{
    p2p_printf("%s\r\n", p2p->msgId);

    memset(&(p2p->msgId), 0, sizeof(p2p->msgId));
    memset(&(p2p->conMsg), 0, sizeof(p2p->conMsg));
    memset(&(p2p->ackMsg), 0, sizeof(p2p->ackMsg));
    memset(&(p2p->startMsg), 0, sizeof(p2p->startMsg));

    p2p->conCbFn = NULL;
    p2p->msgRxCbFn = NULL;
    p2p->msgTxCbFn = NULL;
    p2p->connectionRssi = 0;

    memset(&(p2p->cnc), 0, sizeof(p2p->cnc));
    memset(&(p2p->ack), 0, sizeof(p2p->ack));

    os_timer_disarm(&p2p->tmr.Connection);
    os_timer_disarm(&p2p->tmr.TxRetry);
    os_timer_disarm(&p2p->tmr.Reinit);
    os_timer_disarm(&p2p->tmr.TxAllRetries);
}

/**
 * Send a broadcast connection message
 *
 * Called periodically, with some randomness mixed in from the tmr.Connection
 * timer. The timer is set when connection starts and is stopped when we
 * receive a response to our connection broadcast
 *
 * @param arg The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pConnectionTimeout(void* arg)
{
    p2pInfo* p2p = (p2pInfo*)arg;
    // Send a connection broadcast
    ets_snprintf(p2p->conMsg, sizeof(p2p->conMsg), p2pConnectionMsgFmt,
                 p2p->msgId, p2p->side);
    p2pSendMsgEx(p2p, p2p->conMsg, ets_strlen(p2p->conMsg), false, NULL, NULL);

    // os_random returns a 32 bit number, so this is [500ms,1500ms]
    uint32_t timeoutMs = 100 * (5 + (os_random() % 11));

    // Start the timer again
    p2p_printf("%s retry broadcast in %dms\r\n", p2p->msgId, timeoutMs);
    os_timer_arm(&p2p->tmr.Connection, timeoutMs, false);
}

/**
 * Retries sending a message to be acked
 *
 * Called from the tmr.TxRetry timer. The timer is set when a message to be
 * ACKed is sent and cleared when an ACK is received
 *
 * @param arg The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pTxRetryTimeout(void* arg)
{
    p2pInfo* p2p = (p2pInfo*)arg;
    p2p_printf("%s\r\n", p2p->msgId);

    if(p2p->ack.msgToAckLen > 0)
    {
        p2p_printf("Retrying message \"%s\"\r\n", p2p->ack.msgToAck);
        p2pSendMsgEx(p2p, p2p->ack.msgToAck, p2p->ack.msgToAckLen, true, p2p->ack.SuccessFn, p2p->ack.FailureFn);
    }
}

/**
 * Stops a message transmission attempt after all retries have been exhausted
 * and calls p2p->ack.FailureFn() if a function was given
 *
 * Called from the tmr.TxAllRetries timer. The timer is set when a message to
 * be ACKed is sent for the first time and cleared when the message is ACKed.
 *
 * @param arg The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pTxAllRetriesTimeout(void* arg)
{
    p2pInfo* p2p = (p2pInfo*)arg;
    p2p_printf("%s\r\n", p2p->msgId);
    // Disarm all timers
    os_timer_disarm(&p2p->tmr.TxRetry);
    os_timer_disarm(&p2p->tmr.TxAllRetries);

    // Save the failure function
    void (*FailureFn)(void*) = p2p->ack.FailureFn;
    p2p_printf("Message totally failed \"%s\"\n", p2p->ack.msgToAck);

    // Clear out the ack variables
    ets_memset(&p2p->ack, 0, sizeof(p2p->ack));

    // Call the failure function
    if(NULL != FailureFn)
    {
        FailureFn(p2p);
    }
}

/**
 * Send a message from one Swadge to another.
 * TODO might allow This must not be called before the CON_ESTABLISHED event occurs.
 * TODO maybe if message is sent before CON_ESTABLISHED have receiving mac
 *      first take it as a request to connect, then look at actual message
 * Message addressing, ACKing, and retries
 * all happen automatically
 *
 * @param p2p       The p2pInfo struct with all the state information
 * @param msg       The mandatory three char message type
 * @param payload   An optional message payload string, may be NULL, up to 32 chars
 * //TODO can 32 chars be extended what is total length of espnow message?
 * @param len       The length of the optional message payload string. May be 0
 * @param msgTxCbFn A callback function when this message is ACKed or dropped
 */
void ICACHE_FLASH_ATTR p2pSendMsg(p2pInfo* p2p, char* msg, char* payload,
                                  uint16_t len, p2pMsgTxCbFn msgTxCbFn)
{
    p2p_printf("%s\r\n", p2p->msgId);
    char builtMsg[64] = {0};

    if(NULL == payload || len == 0)
    {
        ets_snprintf(builtMsg, sizeof(builtMsg), p2pNoPayloadMsgFmt,
                     p2p->msgId,
                     msg,
                     0, // sequence number
                     p2p->cnc.otherMac[0],
                     p2p->cnc.otherMac[1],
                     p2p->cnc.otherMac[2],
                     p2p->cnc.otherMac[3],
                     p2p->cnc.otherMac[4],
                     p2p->cnc.otherMac[5],
                     p2p->side,
                     p2p->cnc.otherSide);
    }
    else
    {
        ets_snprintf(builtMsg, sizeof(builtMsg), p2pPayloadMsgFmt,
                     p2p->msgId,
                     msg,
                     0, // sequence number, filled in later
                     p2p->cnc.otherMac[0],
                     p2p->cnc.otherMac[1],
                     p2p->cnc.otherMac[2],
                     p2p->cnc.otherMac[3],
                     p2p->cnc.otherMac[4],
                     p2p->cnc.otherMac[5],
                     p2p->side,
                     p2p->cnc.otherSide,
                     payload);
    }

    p2p->msgTxCbFn = msgTxCbFn;
    p2pSendMsgEx(p2p, builtMsg, strlen(builtMsg), true, p2pModeMsgSuccess, p2pModeMsgFailure);
}

/**
 * Callback function for when a message sent by the Swadge mode, not during
 * the connection process, is ACKed
 *
 * @param arg The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pModeMsgSuccess(void* arg)
{
    p2pInfo* p2p = (p2pInfo*)arg;
    p2p_printf("%s\r\n", p2p->msgId);
    if(NULL != p2p->msgTxCbFn)
    {
        p2p->msgTxCbFn(p2p, MSG_ACKED);
    }
}

/**
 * Callback function for when a message sent by the Swadge mode, not during
 * the connection process, is dropped
 *
 * @param arg The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pModeMsgFailure(void* arg)
{
    p2pInfo* p2p = (p2pInfo*)arg;
    p2p_printf("%s\r\n", p2p->msgId);
    if(NULL != p2p->msgTxCbFn)
    {
        p2p->msgTxCbFn(p2p, MSG_FAILED);
    }
}

/**
 * Wrapper for sending an ESP-NOW message. Handles ACKing and retries for
 * non-broadcast style messages
 * TODO What is a broadcast style message
 *
 * @param p2p       The p2pInfo struct with all the state information
 * @param msg       The message to send, may contain destination MAC
 * @param len       The length of the message to send
 * @param shouldAck true if this message should be acked, false if we don't care
 * @param success   A callback function if the message is acked. May be NULL
 * @param failure   A callback function if the message isn't acked. May be NULL
 */
void ICACHE_FLASH_ATTR p2pSendMsgEx(p2pInfo* p2p, char* msg, uint16_t len,
                                    bool shouldAck, void (*success)(void*), void (*failure)(void*))
{
    p2p_printf("%s\r\n", p2p->msgId);
    // If this is a first time message and longer than a connection message
    if( (p2p->ack.msgToAck != msg) && ets_strlen(p2p->conMsg) < len)
    {
        // Insert a sequence number
        msg[SEQ_IDX + 0] = '0' + (p2p->cnc.mySeqNum / 10);
        msg[SEQ_IDX + 1] = '0' + (p2p->cnc.mySeqNum % 10);

        // Increment the sequence number, 0-99
        p2p->cnc.mySeqNum++;
        if(100 == p2p->cnc.mySeqNum++)
        {
            p2p->cnc.mySeqNum = 0;
        }
    }

#ifdef P2P_DEBUG_PRINT
    char* dbgMsg = (char*)os_zalloc(sizeof(char) * (len + 1));
    ets_memcpy(dbgMsg, msg, len);
    p2p_printf("%s\r\n", dbgMsg);
    os_free(dbgMsg);
#endif

    if(shouldAck)
    {
        // Set the state to wait for an ack
        p2p->ack.isWaitingForAck = true;

        // If this is not a retry
        if(p2p->ack.msgToAck != msg)
        {
            p2p_printf("sending for the first time\r\n");

            // Store the message for potential retries
            ets_memcpy(p2p->ack.msgToAck, msg, len);
            p2p->ack.msgToAckLen = len;
            p2p->ack.SuccessFn = success;
            p2p->ack.FailureFn = failure;

            // Start a timer to retry for 3s total
            os_timer_disarm(&p2p->tmr.TxAllRetries);
            os_timer_arm(&p2p->tmr.TxAllRetries, RETRY_TIME_MS, false);
        }
        else
        {
            p2p_printf("this is a retry\r\n");
        }

        // Mark the time this transmission started, the retry timer gets
        // started in p2pSendCb()
        p2p->ack.timeSentUs = system_get_time();
    }
    espNowSend((const uint8_t*)msg, len);
}

button_mask ICACHE_FLASH_ATTR  p2pHex2Int(uint8_t in)
{
    if(((in >= '0') && (in <= '9')))
    {
        return in - '0';
    }
    if(((in >= 'A') && (in <= 'F')))
    {
        return in - 'A' + 10;
    }
    if(((in >= 'a') && (in <= 'f')))
    {
        return in - 'a' + 10;
    }
    return 0xF;
}

/**
 * This is must be called whenever an ESP NOW packet is received
 *
 * @param p2p      The p2pInfo struct with all the state information
 * @param mac_addr The MAC of the swadge that sent the data
 * @param data     The data
 * @param len      The length of the data
 * @param rssi     The RSSI of th received message, a proxy for distance
 * TODO fix so actually returns what says on the next line
 * @return false if the message was processed here,
 *         true if the message should be processed by the swadge mode
 */
void ICACHE_FLASH_ATTR p2pRecvCb(p2pInfo* p2p, uint8_t* mac_addr, uint8_t* data, uint8_t len, uint8_t rssi)
{
    //#define SHOW_DISCARD
#ifdef P2P_DEBUG_PRINT
#ifdef SHOW_DISCARD
    char* dbgMsg = (char*)os_zalloc(sizeof(char) * (len + 1));
    ets_memcpy(dbgMsg, data, len);
    p2p_printf("%s: %s\r\n", p2p->msgId, dbgMsg);
    os_free(dbgMsg);
#endif
#endif

    // Check if this message matches our message ID
    if(len < CMD_IDX ||
            (0 != ets_memcmp(data, p2p->conMsg, CMD_IDX)))
    {
        // This message is too short, or does not match our message ID
#ifdef SHOW_DISCARD
        p2p_printf("DISCARD: Not a message for '%s'\r\n", p2p->msgId);
#endif
        return;
    }

    // If this message has a MAC, check it
    if(len >= ets_strlen(p2p->ackMsg) &&
            0 != ets_memcmp(&data[MAC_IDX], p2p->cnc.macStr, ets_strlen(p2p->cnc.macStr)))
    {
        // This MAC isn't for us
#ifdef SHOW_DISCARD
        p2p_printf("DISCARD: Not for our MAC\r\n");
#endif
        return;
    }

    // If this is anything besides a con broadcast, check the other MAC
    if(p2p->cnc.otherMacReceived &&
            0 != ets_memcmp(data, p2p->conMsg, SEQ_IDX) &&
            0 != ets_memcmp(mac_addr, p2p->cnc.otherMac, sizeof(p2p->cnc.otherMac)))
    {
        // This directed message not from the other known swadge
#ifdef SHOW_DISCARD
        p2p_printf("DISCARD: Not from the other MAC\r\n");
#endif
        return;
    }

#ifdef P2P_DEBUG_PRINT
#ifndef SHOW_DISCARD
    char* dbgMsg = (char*)os_zalloc(sizeof(char) * (len + 1));
    ets_memcpy(dbgMsg, data, len);
    p2p_printf("%s: %s\r\n", p2p->msgId, dbgMsg);
    os_free(dbgMsg);
#endif
#endif

    // Let the mode handle RESTART message
    // The mode should reset to state as if just started with no button pushes
    if(len >= ets_strlen(p2p->ackMsg) &&
            0 == ets_memcmp(&data[CMD_IDX], "rst", ets_strlen("rst")))
    {
        if(NULL != p2p->msgRxCbFn)
        {
            p2p_printf("letting mode handle RESTART message\r\n");
            char msgType[4] = {0};
            memcpy(msgType, &data[CMD_IDX], 3 * sizeof(char));
            if (len > EXT_IDX)
            {
                p2p->msgRxCbFn(p2p, msgType, &data[EXT_IDX], len - EXT_IDX);
            }
            else
            {
                //TODO should this be NULL, for mode_ring param ignored, but for other applications might use
                p2p->msgRxCbFn(p2p, msgType, NULL, 0);
            }
        }
        //TODO why putting return here stops it working
    }

    // By here, we know the received message matches our message ID, either a
    // connection request broadcast or for us. If for us and not an ack message, ack it
    if(0 != ets_memcmp(data, p2p->conMsg, SEQ_IDX) &&
            0 != ets_memcmp(data, p2p->ackMsg, SEQ_IDX))
    {
        //Check if we are actually connected and if not
        //Could be receiving a (like str) message which is ok
        //     as maybe we were turned off then on
        // So  reconnect if isConnected is false and isConnecting is false
        //     using the info in the message
        // p2p_printf("%s BEFORE ACK\n", p2p->cnc.isConnected ? "CONNECTED" : "NOT CONNECTED");
        // p2p_printf("     Sender %s [%02X:%02X]\n", p2p->msgId, mac_addr[4], mac_addr[5]);
        // p2p_printf("     on side %X otherSide %X\n", p2p->side, p2p->cnc.otherSide);
        // p2p_printf("     isConnecting %s\n", p2p->cnc.isConnecting ? "TRUE" : "FALSE");
        // p2p_printf("     broadcastReceived %s\n", p2p->cnc.broadcastReceived ? "TRUE" : "FALSE");
        // p2p_printf("     rxGameStartMsg %s\n", p2p->cnc.rxGameStartMsg ? "TRUE" : "FALSE");
        // p2p_printf("     rxGameStartAck %s\n", p2p->cnc.rxGameStartAck ? "TRUE" : "FALSE");
        // p2p_printf("     playOrder %d\n", p2p->cnc.playOrder);
        // p2p_printf("     macStr %s\n", p2p->cnc.macStr);
        // p2p_printf("     %s [%02X:%02X]\n",
        //            p2p->cnc.otherMacReceived ? "OTHER MAC RECEIVED " : "NO OTHER MAC RECEIVED ",
        //            p2p->cnc.otherMac[4],
        //            p2p->cnc.otherMac[5]);
        // p2p_printf("     mySeqNum = %d, lastSeqNum = %d\n", p2p->cnc.mySeqNum, p2p->cnc.lastSeqNum);

        if (p2p->cnc.isConnecting == false && p2p->cnc.isConnected == false)
        {
            // Repair connection
            p2p->cnc.isConnected = true;
            p2p->cnc.isConnecting = false;
            p2p->cnc.broadcastReceived = true;
            p2p->cnc.rxGameStartAck = true;
            p2p->cnc.rxGameStartMsg = true;
            p2p->cnc.otherMacReceived = true;
            p2p->cnc.mySeqNum = 0;
            p2p->cnc.lastSeqNum = 255;
            uint8_t i;
            for (i = 0; i < 6; i++)
            {
                p2p->cnc.otherMac[i] = mac_addr[i];
            }
            // Restore side and otherSide
            p2p->side = p2pHex2Int(data[EXT_IDX + 1]);
            p2p->cnc.otherSide = p2pHex2Int(data[EXT_IDX + 0]);
            //TODO restore p2p->cnc.playOrder?

            p2p_printf("%s REPAIRED \n", p2p->cnc.isConnected ? "CONNECTED" : "NOT CONNECTED");
            p2p_printf("     Sender %s [%02X:%02X]\n", p2p->msgId, mac_addr[4], mac_addr[5]);
            p2p_printf("     on side %X otherSide %X\n", p2p->side, p2p->cnc.otherSide);
            p2p_printf("     isConnecting %s\n", p2p->cnc.isConnecting ? "TRUE" : "FALSE");
            p2p_printf("     broadcastReceived %s\n", p2p->cnc.broadcastReceived ? "TRUE" : "FALSE");
            p2p_printf("     rxGameStartMsg %s\n", p2p->cnc.rxGameStartMsg ? "TRUE" : "FALSE");
            p2p_printf("     rxGameStartAck %s\n", p2p->cnc.rxGameStartAck ? "TRUE" : "FALSE");
            p2p_printf("     playOrder %d\n", p2p->cnc.playOrder);
            p2p_printf("     macStr %s\n", p2p->cnc.macStr);
            p2p_printf("     %s [%02X:%02X]\n",
                       p2p->cnc.otherMacReceived ? "OTHER MAC RECEIVED " : "NO OTHER MAC RECEIVED ",
                       p2p->cnc.otherMac[4],
                       p2p->cnc.otherMac[5]);
            p2p_printf("     mySeqNum = %d, lastSeqNum = %d\n", p2p->cnc.mySeqNum, p2p->cnc.lastSeqNum);
        }
        // Acknowledge message
        p2pSendAckToMac(p2p, mac_addr);
    }

    // After ACKing the message, check the sequence number to see if we should
    // process it or ignore it (we already did!)
    if(len >= ets_strlen(p2p->ackMsg))
    {
        // Extract the sequence number
        uint8_t theirSeq = 0;
        theirSeq += (data[SEQ_IDX + 0] - '0') * 10;
        theirSeq += (data[SEQ_IDX + 1] - '0');

        // Check it against the last known sequence number
        if(theirSeq == p2p->cnc.lastSeqNum)
        {
            p2p_printf("DISCARD: Duplicate sequence number\r\n");
            return;
        }
        else
        {
            p2p->cnc.lastSeqNum = theirSeq;
            p2p_printf("Store lastSeqNum %d\n", p2p->cnc.lastSeqNum);
            //Extract senders side and save
            p2p->cnc.otherSide = p2pHex2Int(data[EXT_IDX + 0]);
            p2p_printf("Set p2p->cnc.otherSide = %d\n", p2p->cnc.otherSide);
        }
    }

    // ACKs can be received in any state
    if(p2p->ack.isWaitingForAck)
    {
        // Check if this is an ACK
        if(ets_strlen(p2p->ackMsg) == len &&
                0 == ets_memcmp(data, p2p->ackMsg, SEQ_IDX))
        {
            p2p_printf("ACK Received\r\n");

            // Clear ack timeout variables
            os_timer_disarm(&p2p->tmr.TxRetry);
            // Disarm the whole transmission ack timer
            os_timer_disarm(&p2p->tmr.TxAllRetries);
            // Save the success function
            void (*successFn)(void*) = p2p->ack.SuccessFn;
            // Clear out ACK variables
            ets_memset(&p2p->ack, 0, sizeof(p2p->ack));

            // Call the function after receiving the ack
            if(NULL != successFn)
            {
                successFn(p2p);
            }
        }
        // Don't process anything else when waiting for an ack
        return;
    }

    if(false == p2p->cnc.isConnected)
        // Handle non connection
    {
        if(true == p2p->cnc.isConnecting)
        {
            // TODO if broadcast for the other
            // Received another broadcast, Check if this RSSI is strong enough
            if(!p2p->cnc.broadcastReceived &&
                    rssi > p2p->connectionRssi &&
                    ets_strlen(p2p->conMsg) == len &&
                    0 == ets_memcmp(data, p2p->conMsg, len - 2)) // redundant as only broadcast is 10 long
            {
                // We received a broadcast, don't allow another
                p2p->cnc.broadcastReceived = true;

                //TODO move a bit further down?
                // And process this connection event
                p2pProcConnectionEvt(p2p, RX_BROADCAST);

                // Save the other's MAC
                ets_memcpy(p2p->cnc.otherMac, mac_addr, sizeof(p2p->cnc.otherMac));
                p2p->cnc.otherMacReceived = true;

                // Save the other's side
                p2p->cnc.otherSide = data[SEQ_IDX + 0] - '0'; // taking from con broadcast
                p2p_printf("p2p->cnc.otherSide = %d from con broadcast\n", p2p->cnc.otherSide);

                // Send a message to other to complete the connection.
                ets_snprintf(p2p->startMsg, sizeof(p2p->startMsg), p2pNoPayloadMsgFmt,
                             p2p->msgId,
                             "str",
                             0,
                             mac_addr[0],
                             mac_addr[1],
                             mac_addr[2],
                             mac_addr[3],
                             mac_addr[4],
                             mac_addr[5],
                             //TODO maybe reverse as want to other to know own side and confirm or set up their side?
                             p2p->side,
                             p2p->cnc.otherSide
                            );

                // If it's acked, call p2pGameStartAckRecv(), if not reinit with p2pRestart()
                p2pSendMsgEx(p2p, p2p->startMsg, ets_strlen(p2p->startMsg), true, p2pGameStartAckRecv, p2pRestart);
            }
            // Received a response to our broadcast
            else if (!p2p->cnc.rxGameStartMsg &&
                     ets_strlen(p2p->startMsg) == len &&
                     0 == ets_memcmp(data, p2p->startMsg, SEQ_IDX))
            {
                p2p_printf("Game start message received, ACKing\r\n");
                // TODO record otherSide and confirm own side consistant with message

                // This is another swadge trying to start a game, which means
                // they received our p2p->conMsg. First disable our p2p->conMsg
                os_timer_disarm(&p2p->tmr.Connection);

                // And process this connection event
                p2pProcConnectionEvt(p2p, RX_GAME_START_MSG);
            }
        }
        return;
    }
    else // (thinks) it's connected
    {
        // Look for con broadcast coming from the otherMac on otherSide
        if(0 == ets_memcmp(data, p2p->conMsg, SEQ_IDX)
                && 0 == ets_memcmp(mac_addr, p2p->cnc.otherMac, sizeof(p2p->cnc.otherMac))
                && p2p->cnc.otherSide == p2pHex2Int(data[EXT_IDX + 0]))
        {
            p2p_printf("Re connect from con broadcast %s from otherMac on side %d %d\r\n", p2p->conMsg, p2p->cnc.otherSide,
                       p2pHex2Int(data[EXT_IDX + 0]));
            // This is a con broadcast from p2p->cnc.otherMac (maybe because it was turned off then on and button pushed)
            // reponds automatically to it to reestablish connection
            // Light led with random hue on correct side
            uint8_t randomHue = os_random();
            // Send a repair message with hue
            char testMsg[256] = {0};
            ets_sprintf(testMsg, "%02X REPAIR hue", randomHue);

            // Send a message to other to complete the connection.
            ets_snprintf(p2p->startMsg, sizeof(p2p->startMsg), p2pNoPayloadMsgFmt,
                         p2p->msgId,
                         "rst",
                         0,
                         mac_addr[0],
                         mac_addr[1],
                         mac_addr[2],
                         mac_addr[3],
                         mac_addr[4],
                         mac_addr[5],
                         //TODO check no reverse here ok?
                         p2p->side,
                         p2p->cnc.otherSide
                        );
            // TODO gave no callbacks, can get by with no ack request?
            p2pSendMsgEx(p2p, p2p->startMsg, ets_strlen(p2p->startMsg), true, NULL, NULL);
            // TODO  check why return can be left out and still ok
            return;
        }


        p2p_printf("cnc.isconnected is true\r\n");
        // Let the mode handle it
        if(NULL != p2p->msgRxCbFn)
        {
            p2p_printf("letting mode handle message\r\n");
            char msgType[4] = {0};
            memcpy(msgType, &data[CMD_IDX], 3 * sizeof(char));
            if (len > EXT_IDX)
            {
                p2p->msgRxCbFn(p2p, msgType, &data[EXT_IDX], len - EXT_IDX);
            }
            else
            {
                //TODO should this be NULL, for mode_ring param ignored, but for other applications might use
                p2p->msgRxCbFn(p2p, msgType, NULL, 0);
            }
        }
    }
}

/**
 * Helper function to send an ACK message to the given MAC
 *
 * @param p2p      The p2pInfo struct with all the state information
 * @param mac_addr The MAC to address this ACK to
 */
void ICACHE_FLASH_ATTR p2pSendAckToMac(p2pInfo* p2p, uint8_t* mac_addr)
{
    p2p_printf("%s %s\r\n", __func__, p2p->msgId);
    ets_snprintf(p2p->ackMsg, sizeof(p2p->ackMsg), p2pNoPayloadMsgFmt,
                 p2p->msgId,
                 "ack",
                 0,
                 mac_addr[0],
                 mac_addr[1],
                 mac_addr[2],
                 mac_addr[3],
                 mac_addr[4],
                 mac_addr[5],
                 p2p->side,
                 p2p->cnc.otherSide
                );
    //TODO this didn't work when ackMsg was 32 chars
    p2p_printf("p2p->ackMsg %s len=%d\n", p2p->ackMsg, ets_strlen(p2p->ackMsg));
    p2pSendMsgEx(p2p, p2p->ackMsg, ets_strlen(p2p->ackMsg), false, NULL, NULL);
}

/**
 * This is called when p2p->startMsg is acked and processes the connection event
 *
 * @param arg The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pGameStartAckRecv(void* arg)
{
    p2pInfo* p2p = (p2pInfo*)arg;
    p2p_printf("%s\r\n", p2p->msgId);
    //TODO is p2p the one who ack'd, check side and otherSide
    p2pProcConnectionEvt(p2p, RX_GAME_START_ACK);
}

/**
 * Two steps are necessary to establish a connection in no particular order.
 * 1. This swadge has to receive a start message from another swadge
 * 2. This swadge has to receive an ack to a start message sent to another swadge
 * The order of events determines who is the 'client' and who is the 'server'
 *
 * @param p2p   The p2pInfo struct with all the state information
 * @param event The event that occurred
 */
void ICACHE_FLASH_ATTR p2pProcConnectionEvt(p2pInfo* p2p, connectionEvt_t event)
{
    p2p_printf("%s %s evt: %d, p2p->cnc.rxGameStartMsg %d, p2p->cnc.rxGameStartAck %d\r\n", __func__, p2p->msgId, event,
               p2p->cnc.rxGameStartMsg, p2p->cnc.rxGameStartAck);

    switch(event)
    {
        case RX_GAME_START_MSG:
        {
            // Already received the ack, become the client
            if(!p2p->cnc.rxGameStartMsg && p2p->cnc.rxGameStartAck)
            {
                p2p->cnc.playOrder = GOING_SECOND;
            }
            // Mark this event
            p2p->cnc.rxGameStartMsg = true;
            break;
        }
        case RX_GAME_START_ACK:
        {
            // Already received the msg, become the server
            if(!p2p->cnc.rxGameStartAck && p2p->cnc.rxGameStartMsg)
            {
                p2p->cnc.playOrder = GOING_FIRST;
            }
            // Mark this event
            p2p->cnc.rxGameStartAck = true;
            break;
        }
        case CON_STARTED:
        case RX_BROADCAST:
        case CON_ESTABLISHED:
        case CON_LOST:
        case CON_STOPPED:
        default:
        {
            break;
        }
    }

    if(NULL != p2p->conCbFn)
    {
        p2p->conCbFn(p2p, event);
    }

    // If both the game start messages are good, start the game
    if(p2p->cnc.rxGameStartMsg && p2p->cnc.rxGameStartAck)
    {
        // Connection was successful, so disarm the failure timer
        os_timer_disarm(&p2p->tmr.Reinit);

        p2p->cnc.isConnecting = false;
        p2p->cnc.isConnected = true;

        // tell the mode it's connected
        if(NULL != p2p->conCbFn)
        {
            p2p->conCbFn(p2p, CON_ESTABLISHED);
        }
    }
    else
    {
        // Start a timer to reinit if we never finish connection
        p2pStartRestartTimer(p2p);
    }
}

/**
 * This starts a timer to call p2pRestart(), used in case of a failure
 * The timer is set when one half of the necessary connection messages is received
 * The timer is disarmed when the connection is established
 *
 * @param arg The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pStartRestartTimer(void* arg)
{
    p2pInfo* p2p = (p2pInfo*)arg;
    p2p_printf("%s\r\n", p2p->msgId);
    // If the connection isn't established in FAILURE_RESTART_MS, restart
    os_timer_arm(&p2p->tmr.Reinit, FAILURE_RESTART_MS, false);
}

/**
 * Restart by deiniting then initing. Persist the msgId and p2p->conCbFn
 * fields
 *
 * @param arg The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pRestart(void* arg)
{
    p2pInfo* p2p = (p2pInfo*)arg;
    p2p_printf("%s\r\n", p2p->msgId);
    if(NULL != p2p->conCbFn)
    {
        p2p->conCbFn(p2p, CON_LOST);
    }

    // Save what's necessary for init
    char msgId[4] = {0};
    ets_strncpy(msgId, p2p->msgId, sizeof(msgId));
    p2pConCbFn conCbFn = p2p->conCbFn;
    p2pMsgRxCbFn msgRxCbFn = p2p->msgRxCbFn;
    uint8_t connectionRssi = p2p->connectionRssi;
    // Stop and clear everything
    p2pDeinit(p2p);
    // Start it up again
    p2pInitialize(p2p, msgId, conCbFn, msgRxCbFn, connectionRssi);
}

/**
 * This must be called by whatever function is registered to the Swadge mode's
 * fnEspNowSendCb
 *
 * This is called after an attempted transmission. If it was successful, and the
 * message should be acked, start a retry timer. If it wasn't successful, just
 * try again
 *
 * @param p2p      The p2pInfo struct with all the state information
 * @param mac_addr unused
 * @param status   Whether the transmission succeeded or failed
 */
void ICACHE_FLASH_ATTR p2pSendCb(p2pInfo* p2p, uint8_t* mac_addr __attribute__((unused)), mt_tx_status status)
{
    p2p_printf("s:%d, t:%d\n", status, p2p->ack.timeSentUs);
    switch(status)
    {
        case MT_TX_STATUS_OK:
        {
            if(0 != p2p->ack.timeSentUs)
            {
                uint32_t transmissionTimeUs = system_get_time() - p2p->ack.timeSentUs;
                p2p_printf("%s Transmission time %dus\r\n", p2p->msgId, transmissionTimeUs);
                // The timers are all millisecond, so make sure that
                // transmissionTimeUs is at least 1ms
                if(transmissionTimeUs < 1000)
                {
                    transmissionTimeUs = 1000;
                }

                // Round it to the nearest Ms, add 69ms (the measured worst case)
                // then add some randomness [0ms to 15ms random]
                uint32_t waitTimeMs = ((transmissionTimeUs + 500) / 1000) + 69 + (os_random() & 0b1111);

                // Start the timer
                p2p_printf("ack timer set for %dms\r\n", waitTimeMs);
                os_timer_arm(&p2p->tmr.TxRetry, waitTimeMs, false);
            }
            break;
        }
        case MT_TX_STATUS_FAILED:
        {
            // If a message is stored
            if(p2p->ack.msgToAckLen > 0)
            {
                // try again in 1ms
                os_timer_arm(&p2p->tmr.TxRetry, 1, false);
            }
            break;
        }
        default:
        {
            break;
        }
    }
}

/**
 * After the swadge is connected to another, return whether this Swadge is
 * player 1 or player 2. This can be used to determine client/server roles
 *
 * @param p2p The p2pInfo struct with all the state information
 * @return    GOING_SECOND, GOING_FIRST, or NOT_SET
 */
playOrder_t ICACHE_FLASH_ATTR p2pGetPlayOrder(p2pInfo* p2p)
{
    if(p2p->cnc.isConnected)
    {
        return p2p->cnc.playOrder;
    }
    else
    {
        return NOT_SET;
    }
}

/**
 * Override whether the Swadge is player 1 or player 2. You probably shouldn't
 * do this, but you might want to for single player modes
 *
 * @param p2p The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pSetPlayOrder(p2pInfo* p2p, playOrder_t order)
{
    p2p->cnc.playOrder = order;
}
