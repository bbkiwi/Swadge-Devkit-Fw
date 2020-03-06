/*
 * magfeston ring based on
 * mode_joust_game.c by Aaron Angert
 *
 *  Created on: 14 Feb, 2020
 *      Author: bbkiwi
 *
 */


// Almost
// A Button 1,2 or start mode, 2 wait any time then B quick 1,2 connects
// If on B wait between 1 and 2 get time out and both got to start up screen
// push 2 on each and connect BUT sometimes accelerometer timer gets killed
// so display stops. Will say FOUND PLAYER first time, then blank.
// Must restart to fix
// With multiple swadges nearby more sensitive to the problem
// TODO Why

/*============================================================================
 * Includes
 *==========================================================================*/

#include <osapi.h>
#include <user_interface.h>
#include <p2pConnection.h>
#include "espNowUtils.h"
#include <math.h>
#include <stdlib.h>
#include <mem.h>

#include "user_main.h"
#include "mode_joust_game.h"
#include "custom_commands.h"
#include "buttons.h"
#include "oled.h"
#include "font.h"
#include "embeddedout.h"
#include "bresenham.h"
#include "mode_tiltrads.h"

/*============================================================================
 * Defines
 *==========================================================================*/

#define JOUST_DEBUG_PRINT
#ifdef JOUST_DEBUG_PRINT
    #define joust_printf(...) os_printf(__VA_ARGS__)
#else
    #define joust_printf(...)
#endif
#define WARNING_THRESHOLD 20

/*============================================================================
 * Enums
 *==========================================================================*/

typedef enum
{
    R_MENU,
    R_SEARCHING,
    R_CONNECTING,
    R_SHOW_CONNECTION,
    R_PLAYING,
    R_SHOW_GAME_RESULT,
    R_GAME_OVER,
} joustGameState_t;

char stateName[9][19] =
{
    "R_MENU",
    "R_SEARCHING",
    "R_CONNECTING",
    "R_SHOW_CONNECTION",
    "R_PLAYING",
    "R_SHOW_GAME_RESULT",
    "R_GAME_OVER",
};

typedef enum
{
    LED_OFF,
    LED_ON_1,
    LED_DIM_1,
    LED_ON_2,
    LED_DIM_2,
    LED_OFF_WAIT,
    LED_CONNECTED_BRIGHT,
    LED_CONNECTED_DIM,
} connLedState_t;

/*============================================================================
 * Prototypes
 *==========================================================================*/

// SwadgeMode Callbacks
void ICACHE_FLASH_ATTR joustInit(void);
void ICACHE_FLASH_ATTR joustDeinit(void);
void ICACHE_FLASH_ATTR joustButton(uint8_t state __attribute__((unused)),
                                   int button, int down);
void ICACHE_FLASH_ATTR joustRecvCb(uint8_t* mac_addr, uint8_t* data, uint8_t len, uint8_t rssi);
void ICACHE_FLASH_ATTR joustSendCb(uint8_t* mac_addr, mt_tx_status status);

// Helper function
void ICACHE_FLASH_ATTR joustRestart(void* arg __attribute__((unused)));
void ICACHE_FLASH_ATTR joustRestartPlay(void* arg __attribute__((unused)));
void ICACHE_FLASH_ATTR joustConnectionCallback(p2pInfo* p2p, connectionEvt_t event);
void ICACHE_FLASH_ATTR joustMsgCallbackFn(p2pInfo* p2p, char* msg, uint8_t* payload, uint8_t len);
void ICACHE_FLASH_ATTR joustMsgTxCbFn(p2pInfo* p2p, messageStatus_t status);
uint32_t ICACHE_FLASH_ATTR joust_rand(uint32_t upperBound);
// Transmission Functions
void ICACHE_FLASH_ATTR joustSendMsg(char* msg, uint16_t len, bool shouldAck, void (*success)(void*),
                                    void (*failure)(void*));
void ICACHE_FLASH_ATTR joustTxAllRetriesTimeout(void* arg __attribute__((unused)) );
void ICACHE_FLASH_ATTR joustTxRetryTimeout(void* arg);

// Connection functions
void ICACHE_FLASH_ATTR joustConnectionTimeout(void* arg __attribute__((unused)));

// Game functions
void ICACHE_FLASH_ATTR joustStartPlaying(void* arg __attribute__((unused)));
void ICACHE_FLASH_ATTR joustStartRound(void);
void ICACHE_FLASH_ATTR joustSendRoundLossMsg(void);
void ICACHE_FLASH_ATTR joustAccelerometerHandler(accel_t* accel);

// LED Functions
void ICACHE_FLASH_ATTR joustDisarmAllLedTimers(void);
void ICACHE_FLASH_ATTR joustConnLedTimeout(void* arg __attribute__((unused)));
void ICACHE_FLASH_ATTR joustShowConnectionLedTimeout(void* arg __attribute__((unused)));
void ICACHE_FLASH_ATTR joustGameLedTimeout(void* arg __attribute__((unused)));
void ICACHE_FLASH_ATTR joustRoundResultLed(void* arg __attribute__((unused)));
void ICACHE_FLASH_ATTR joustRoundResult(int);

void ICACHE_FLASH_ATTR joustUpdateDisplay(void);
void ICACHE_FLASH_ATTR joustDrawMenu(void);
void ICACHE_FLASH_ATTR joustScrollInstructions(void* arg);

/*============================================================================
 * Variables
 *==========================================================================*/

swadgeMode joustGameMode =
{
    .modeName = "joust",
    .fnEnterMode = joustInit,
    .fnExitMode = joustDeinit,
    .fnButtonCallback = joustButton,
    .wifiMode = ESP_NOW,
    .fnEspNowRecvCb = joustRecvCb,
    .fnEspNowSendCb = joustSendCb,
    .fnAccelerometerCallback = joustAccelerometerHandler,
    .menuImageData = mnu_joust_0,
    .menuImageLen = sizeof(mnu_joust_0)

};

struct
{
    joustGameState_t gameState;
    accel_t joustAccel;
    uint16_t rolling_average;
    uint32_t con_color;
    playOrder_t playOrder;
    uint16_t mov;
    uint16_t meterSize;
    // Game state variables
    struct
    {
        bool shouldTurnOnLeds;
        bool round_winner;
        uint32_t joustWins;
        uint32_t win_score;
        uint32_t lose_score;
    } gam;

    // Timers
    struct
    {
        //os_timer_t StartPlaying;
        os_timer_t ConnLed;
        os_timer_t ShowConnectionLed;
        os_timer_t GameLed;
        os_timer_t RoundResultLed;
        os_timer_t RestartJoust;
        os_timer_t RestartJoustPlay;
        os_timer_t ScrollInstructions;
    } tmr;

    // LED variables
    struct
    {
        led_t Leds[6];
        connLedState_t ConnLedState;
        uint8_t connectionDim;
        uint8_t digitToDisplay;
        uint8_t ledsLit;
        uint8_t currBrightness;
    } led;

    p2pInfo p2pJoust;

    int16_t instructionTextIdx;
} joust;

bool joustWarningShown = true;

/*============================================================================
 * Functions
 *==========================================================================*/

/**
 * Get a random number from a range.
 *
 * This isn't true-random, unless bound is a power of 2. But it's close enough?
 * The problem is that os_random() returns a number between [0, 2^64), and the
 * size of the range may not be even divisible by bound
 *
 * For what it's worth, this is what Arduino's random() does. It lies!
 *
 * @param bound An upper bound of the random range to return
 * @return A number in the range [0,bound), which does not include bound
 */
uint32_t ICACHE_FLASH_ATTR joust_rand(uint32_t bound)
{
    if(bound == 0)
    {
        return 0;
    }
    return os_random() % bound;
}

void ICACHE_FLASH_ATTR joustConnectionCallback(p2pInfo* p2p __attribute__((unused)), connectionEvt_t event)
{
    os_printf("%s %s >>>\n", __func__, conEvtName[event]);
    espNowPrintInfo();
    switch(event)
    {
        case CON_STARTED:
        {
            break;
        }
        case RX_GAME_START_ACK:
        {
            break;
        }
        case RX_GAME_START_MSG:
        {
            break;
        }
        case CON_ESTABLISHED:
        {
            joust.playOrder = p2pGetPlayOrder(&joust.p2pJoust);
            if(GOING_FIRST == joust.playOrder)
            {
                char color_string[32] = {0};
                joust.con_color =  joust_rand(255);
                ets_snprintf(color_string, sizeof(color_string), "%d", joust.con_color);
                p2pSendMsg(&joust.p2pJoust, "col", color_string, sizeof(color_string), joustMsgTxCbFn);
            }
            joust_printf("   connection established\n");
            clearDisplay();
            plotText(0, 0, "Found Player", IBM_VGA_8, WHITE);
            plotText(0, OLED_HEIGHT - (4 * (FONT_HEIGHT_IBMVGA8 + 1)), "Move theirs", IBM_VGA_8, WHITE);
            plotText(0, OLED_HEIGHT - (3 * (FONT_HEIGHT_IBMVGA8 + 1)), "Not yours!", IBM_VGA_8, WHITE);

            joustDisarmAllLedTimers();
            // 6ms * ~500 steps == 3s animation
            //This is the start of the game
            joust.led.currBrightness = 0;
            joust.led.ConnLedState = LED_CONNECTED_BRIGHT;
            os_timer_arm(&joust.tmr.ShowConnectionLed, 50, true);

            break;
        }
        default:
        case CON_LOST:
        {
            //TODO Did this fix hang?
            joustDisarmAllLedTimers();
            os_timer_arm(&joust.tmr.RestartJoust, 500, false);
            break;
        }
    }
}

/**
 * @brief
 *
 * @param msg
 * @param payload
 * @param len
 */
void ICACHE_FLASH_ATTR joustMsgCallbackFn(p2pInfo* p2p __attribute__((unused)), char* msg, uint8_t* payload,
        uint8_t len __attribute__((unused)))
//TODO fix print below of len and payload as could be funny characters
{
    joust_printf("%s %s %s ", __func__, stateName[joust.gameState], msg);
    if(len > 0)
    {
        joust_printf(" len=%d, %s\n", len, payload);
    }
    else
    {
        joust_printf("\n");

    }

    if(0 == ets_memcmp(msg, "col", 3))
    {
        joust.con_color =  atoi((const char*)payload);
        joust_printf("Got message with color %d\r\n", joust.con_color);
    }

    switch(joust.gameState)
    {
        case R_CONNECTING:
        {
            break;
        }
        case R_PLAYING:
        {
            if(0 == ets_memcmp(msg, "los", 3))
            {
                p2pSendMsg(&joust.p2pJoust, "win", NULL, 0, joustMsgTxCbFn);
                joustRoundResult(true);
            }
            // Currently playing a game, if a message is sent, then update score
            break;
        }
        case R_GAME_OVER:
        {
            if(0 == ets_memcmp(msg, "los", 3))
            {
                p2pSendMsg(&joust.p2pJoust, "tie", NULL, 0, joustMsgTxCbFn);
                joustRoundResult(2);
            }
            else if(0 == ets_memcmp(msg, "tie", 3))
            {
                joustRoundResult(2);
            }
            else if(0 == ets_memcmp(msg, "win", 3))
            {
                joustRoundResult(false);
            }
            // Currently playing a game, if a message is sent, then update score
            break;
        }
        case R_MENU:
        case R_SEARCHING:
        {
            //TODO needed?
            if(0 == ets_memcmp(msg, "col", 3))
            {
                clearDisplay();
                plotText(0, 0, "Found Player", IBM_VGA_8, WHITE);
                plotText(0, OLED_HEIGHT - (4 * (FONT_HEIGHT_IBMVGA8 + 1)), "Move theirs", IBM_VGA_8, WHITE);
                plotText(0, OLED_HEIGHT - (3 * (FONT_HEIGHT_IBMVGA8 + 1)), "Not yours!", IBM_VGA_8, WHITE);
            }
        }
        case R_SHOW_CONNECTION:
        case R_SHOW_GAME_RESULT:
        {
            // Just LED animations, don't do anything with messages
            break;
        }
        default:
        {
            break;
        }
    }
}

//were gonna need a lobby with lots of players
/**
 * Initialize everything and start sending broadcast messages
 */
void ICACHE_FLASH_ATTR joustInit(void)
{
    uint8_t i;
    for(i = 0; i < 6; i++)
    {
        joust.led.Leds[i].r = 0;
        joust.led.Leds[i].g = 0;
        joust.led.Leds[i].b = 0;
    }

    setLeds(joust.led.Leds, sizeof(joust.led.Leds));

    ets_memset(&joust, 0, sizeof(joust));
    joust_printf("%s\r\n", __func__);
    joust.gam.joustWins = 0; //getJoustWins();
    clearDisplay();

    // Set up a timer to scroll the instructions
    os_timer_disarm(&joust.tmr.ScrollInstructions);
    os_timer_setfn(&joust.tmr.ScrollInstructions, joustScrollInstructions, NULL);

    joust.gameState = R_MENU;
    // Draw the  menu
    joust.instructionTextIdx = OLED_WIDTH;
    joustDrawMenu();
    // Start the timer to scroll text
    os_timer_arm(&joust.tmr.ScrollInstructions, 34, true);


    // Enable button debounce for consistent 1p/2p and difficulty config
    enableDebounce(true);

    p2pInitialize(&joust.p2pJoust, "jou", joustConnectionCallback, joustMsgCallbackFn, 10);

    //we don't need a timer to show a successful connection, but we do need
    //to start the game eventually
    // Set up a timer for showing a successful connection, don't start it
    os_timer_disarm(&joust.tmr.ShowConnectionLed);
    os_timer_setfn(&joust.tmr.ShowConnectionLed, joustShowConnectionLedTimeout, NULL);

    // Set up a timer to update LEDs, start it
    os_timer_disarm(&joust.tmr.ConnLed);
    os_timer_setfn(&joust.tmr.ConnLed, joustConnLedTimeout, NULL);

    os_timer_disarm(&joust.tmr.GameLed);
    os_timer_setfn(&joust.tmr.GameLed, joustGameLedTimeout, NULL);

    os_timer_disarm(&joust.tmr.RestartJoust);
    os_timer_setfn(&joust.tmr.RestartJoust, joustRestart, NULL);

    os_timer_disarm(&joust.tmr.RestartJoustPlay);
    os_timer_setfn(&joust.tmr.RestartJoustPlay, joustRestartPlay, NULL);

    os_timer_disarm(&joust.tmr.RoundResultLed);
    os_timer_setfn(&joust.tmr.RoundResultLed, joustRoundResultLed, NULL);
}

/**
 * @brief Draw the Joust menu with a title, levels, instructions, and button labels
 */
void ICACHE_FLASH_ATTR joustDrawMenu(void)
{
#define Y_MARGIN 6
    uint8_t textY = 0;

    clearDisplay();

    // Draw title
    plotText(5, textY, "Magfestons", RADIOSTARS, WHITE);
    textY += FONT_HEIGHT_RADIOSTARS + Y_MARGIN;
    // Draw instruction ticker
    if (0 > plotText(joust.instructionTextIdx, textY,
                     "Magfeston Pass is a game in a ring where you pass balls of energy to your neighbors. It is a fast paced game and if the balls go too fast they might explode. Enjoy!",
                     IBM_VGA_8, WHITE))
    {
        joust.instructionTextIdx = OLED_WIDTH;
    }

    // Draw button labels
    plotRect(
        -1,
        OLED_HEIGHT - FONT_HEIGHT_TOMTHUMB - 4,
        getTextWidth("Connect Left", TOM_THUMB) + 3,
        OLED_HEIGHT + 1,
        WHITE);
    plotText(
        0,
        OLED_HEIGHT - FONT_HEIGHT_TOMTHUMB,
        "Connect Left",
        TOM_THUMB,
        WHITE);

    plotRect(
        OLED_WIDTH - getTextWidth("Connect Right", TOM_THUMB) - 4,
        OLED_HEIGHT - FONT_HEIGHT_TOMTHUMB - 4,
        OLED_WIDTH + 1,
        OLED_HEIGHT + 1,
        WHITE);
    plotText(
        OLED_WIDTH - getTextWidth("Connect Right", TOM_THUMB),
        OLED_HEIGHT - FONT_HEIGHT_TOMTHUMB,
        "Connect Right",
        TOM_THUMB,
        WHITE);
}

/**
 * @brief Decrement the index to draw the instructions at, then draw the menu
 *
 * @param arg unused
 */
void ICACHE_FLASH_ATTR joustScrollInstructions(void* arg __attribute__((unused)))
{
    joust.instructionTextIdx--;
    joustDrawMenu();
}

/**
 * Clean up all timers
 */
void ICACHE_FLASH_ATTR joustDeinit(void)
{
    joust_printf("%s\r\n", __func__);
    p2pDeinit(&joust.p2pJoust);
    os_timer_disarm(&joust.tmr.RestartJoust);
    os_timer_disarm(&joust.tmr.RestartJoustPlay);
    os_timer_disarm(&joust.tmr.ScrollInstructions);
    joustDisarmAllLedTimers();
}

/**
 * Restart by deiniting then initing
 *
 * @param arg unused
 */
void ICACHE_FLASH_ATTR joustRestart(void* arg __attribute__((unused)))
{
    joust_printf("%s\r\n", __func__);
    //p2pRestart(&joust.p2pJoust); // does nothing!
    //use this instead
    joustDeinit();
    joustInit();
}

/**
 * Restart Play so keep existing connection
 *
 * @param arg unused
 */
void ICACHE_FLASH_ATTR joustRestartPlay(void* arg __attribute__((unused)))
{
    if(GOING_FIRST == joust.playOrder)
    {
        joust_printf("Restart Play as going first\n");
        char color_string[32] = {0};
        joust.con_color =  joust_rand(255);
        ets_snprintf(color_string, sizeof(color_string), "%d", joust.con_color);
        p2pSendMsg(&joust.p2pJoust, "col", color_string, sizeof(color_string), joustMsgTxCbFn);
    }
    else
    {
        joust_printf("Restart Play as going second\n");
        joust.gameState = R_SEARCHING;
    };

    clearDisplay();
    // plotText(0, 0, "Same Player", IBM_VGA_8, WHITE);
    // plotText(0, OLED_HEIGHT - (4 * (FONT_HEIGHT_IBMVGA8 + 1)), "Move theirs", IBM_VGA_8, WHITE);
    // plotText(0, OLED_HEIGHT - (3 * (FONT_HEIGHT_IBMVGA8 + 1)), "Not yours!", IBM_VGA_8, WHITE);

    joustDisarmAllLedTimers();
    // 6ms * ~500 steps == 3s animation
    //This is the start of the game
    joust.led.currBrightness = 0;
    joust.led.ConnLedState = LED_CONNECTED_BRIGHT;
    os_timer_arm(&joust.tmr.ShowConnectionLed, 50, false);

}

/**
 * Disarm any timers which control LEDs
 */
void ICACHE_FLASH_ATTR joustDisarmAllLedTimers(void)
{
    os_timer_disarm(&joust.tmr.ConnLed);
    os_timer_disarm(&joust.tmr.ShowConnectionLed);
    os_timer_disarm(&joust.tmr.GameLed);
    os_timer_disarm(&joust.tmr.RoundResultLed);
}

/**
 * This is called after an attempted transmission. If it was successful, and the
 * message should be acked, start a retry timer. If it wasn't successful, just
 * try again
 *
 * @param mac_addr
 * @param status
 */
void ICACHE_FLASH_ATTR joustSendCb(uint8_t* mac_addr __attribute__((unused)),
                                   mt_tx_status status)
{
    p2pSendCb(&joust.p2pJoust, mac_addr, status);
}

/**
 * This is called whenever an ESP NOW packet is received
 *
 * @param mac_addr The MAC of the swadge that sent the data
 * @param data     The data
 * @param len      The length of the data
 */
void ICACHE_FLASH_ATTR joustRecvCb(uint8_t* mac_addr, uint8_t* data, uint8_t len, uint8_t rssi)
{
    p2pRecvCb(&joust.p2pJoust, mac_addr, data, len, rssi);
}

/**
 * This LED handling timer fades in and fades out white LEDs to indicate
 * a successful connection. After the animation, the game will start
 *
 * @param arg unused
 */
void ICACHE_FLASH_ATTR joustShowConnectionLedTimeout(void* arg __attribute__((unused)) )
{
    uint8_t i;
    for(i = 0; i < 6; i++)
    {
        joust.led.Leds[i].r = (EHSVtoHEX(joust.con_color, 255,  joust.led.currBrightness) >>  0) & 0xFF;
        joust.led.Leds[i].g = (EHSVtoHEX(joust.con_color, 255,  joust.led.currBrightness) >>  8) & 0xFF;
        joust.led.Leds[i].b = (EHSVtoHEX(joust.con_color, 255,  joust.led.currBrightness) >> 16) & 0xFF;
    }
    setLeds(joust.led.Leds, sizeof(joust.led.Leds));
    joustStartPlaying(NULL);
}



/**
 * This LED handling timer fades in and fades out white LEDs to indicate
 * a successful connection. After the animation, the game will start
 *
 * @param arg unused
 */
void ICACHE_FLASH_ATTR joustGameLedTimeout(void* arg __attribute__((unused)) )
{
    switch(joust.led.ConnLedState)
    {
        case LED_CONNECTED_BRIGHT:
        {
            joust.led.currBrightness++;
            if(joust.led.currBrightness == 64)
            {
                joust.led.ConnLedState = LED_CONNECTED_DIM;
            }
            break;
        }
        case LED_CONNECTED_DIM:
        {
            joust.led.currBrightness--;
            if(joust.led.currBrightness == 0)
            {
                joust.led.ConnLedState = LED_CONNECTED_BRIGHT;
            }
            break;
        }
        case LED_OFF:
        case LED_ON_1:
        case LED_DIM_1:
        case LED_ON_2:
        case LED_DIM_2:
        case LED_OFF_WAIT:
        default:
        {
            // No other cases handled
            break;
        }
    }

    // When there's a warning, flash the LEDs
    uint32_t ledColor = joust.con_color;
    uint8_t ledBright = joust.led.currBrightness;
    if(joust.mov > joust.rolling_average + WARNING_THRESHOLD)
    {
        ledBright = 150;
        ledColor = 0;
    }

    uint8_t i;
    for(i = 0; i < 6; i++)
    {
        joust.led.Leds[i].r = (EHSVtoHEX(ledColor, 255, ledBright) >>  0) & 0xFF;
        joust.led.Leds[i].g = (EHSVtoHEX(ledColor, 255, ledBright) >>  8) & 0xFF;
        joust.led.Leds[i].b = (EHSVtoHEX(ledColor, 255, ledBright) >> 16) & 0xFF;
    }

    setLeds(joust.led.Leds, sizeof(joust.led.Leds));
}

/**
 * This is called after connection is all done. Start the game!
 *
 * @param arg unused
 */
void ICACHE_FLASH_ATTR joustStartPlaying(void* arg __attribute__((unused)))
{
    joust_printf("%s\r\n", __func__);
    joust_printf("\nstarting the game\n");

    // Disable button debounce for minimum latency
    enableDebounce(false);

    // Turn off the LEDs
    joustDisarmAllLedTimers();
    joust.led.currBrightness = 0;
    joust.led.ConnLedState = LED_CONNECTED_BRIGHT;
    os_timer_arm(&joust.tmr.GameLed, 6, true);
    joust.gameState = R_PLAYING;
}


void ICACHE_FLASH_ATTR joustUpdateDisplay(void)
{
    // Clear the display
    clearDisplay();

    // Figure out what title to draw
    static uint8_t showWarningFrames = 0;
    if(joust.mov > joust.rolling_average + WARNING_THRESHOLD)
    {
        showWarningFrames = 15;
    }
    else if(0 < showWarningFrames)
    {
        showWarningFrames--;
    }

    // Draw a title
    plotCenteredText(0, 0, OLED_WIDTH, &joust.p2pJoust.cnc.macStr[0], IBM_VGA_8, WHITE);
    plotCenteredText(0, 12, OLED_WIDTH, &joust.p2pJoust.cnc.otherMacStr[0], IBM_VGA_8, WHITE);
    // Display the acceleration on the display
    plotRect(
        0,
        (OLED_HEIGHT / 2 - 13) + 10,
        OLED_WIDTH - 2,
        (OLED_HEIGHT / 2 + 13) + 10,
        WHITE);

    // Find the difference from the rolling average, scale it to 220px (5px margin on each side)
    int16_t diffFromAvg = ((joust.mov - joust.rolling_average) * 220) / 43;
    // Clamp it to the meter's draw range
    if(diffFromAvg < 0)
    {
        diffFromAvg = 0;
    }
    if(diffFromAvg > 118)
    {
        diffFromAvg = 118;
    }

    // Either set the meter if the difference is high, or slowly clear it
    if(diffFromAvg > joust.meterSize)
    {
        joust.meterSize = diffFromAvg;
    }
    else
    {
        if(joust.meterSize >= 12)
        {
            joust.meterSize -= 12;
        }
        else
        {
            joust.meterSize = 0;
        }
    }

    // draw it
    fillDisplayArea(
        5,
        (OLED_HEIGHT / 2 - 10) + 10,
        5 + joust.meterSize,
        (OLED_HEIGHT / 2 + 10) + 10,
        WHITE);
}

/**
 * Update the acceleration for the Joust mode
 */
void ICACHE_FLASH_ATTR joustAccelerometerHandler(accel_t* accel)
{
    //joust_printf("a"); // to see if getting called, sometimes stops
    if(joust.gameState != R_MENU)
    {
        joust.joustAccel.x = accel->x;
        joust.joustAccel.y = accel->y;
        joust.joustAccel.z = accel->z;
        joust.mov = (uint16_t) sqrt(pow(joust.joustAccel.x, 2) + pow(joust.joustAccel.y, 2) + pow(joust.joustAccel.z, 2));
        joust.rolling_average = (joust.rolling_average * 2 + joust.mov) / 3;
        if (joust.gameState == R_PLAYING)
        {
            joustUpdateDisplay();
        }
    }
}

/**
 * Called every 4ms, this updates the LEDs during connection
 */
void ICACHE_FLASH_ATTR joustConnLedTimeout(void* arg __attribute__((unused)))
{
    switch(joust.led.ConnLedState)
    {
        case LED_OFF:
        {
            // Reset this timer to LED_PERIOD_MS
            joustDisarmAllLedTimers();
            os_timer_arm(&joust.tmr.ConnLed, 4, true);

            joust.led.connectionDim = 0;
            ets_memset(joust.led.Leds, 0, sizeof(joust.led.Leds));

            joust.led.ConnLedState = LED_ON_1;
            break;
        }
        case LED_ON_1:
        {
            // Turn LEDs on
            joust.led.connectionDim = 50;

            // Prepare the first dimming
            joust.led.ConnLedState = LED_DIM_1;
            break;
        }
        case LED_DIM_1:
        {
            // Dim leds
            joust.led.connectionDim--;
            // If its kind of dim, turn it on again
            if(joust.led.connectionDim == 1)
            {
                joust.led.ConnLedState = LED_ON_2;
            }
            break;
        }
        case LED_ON_2:
        {
            // Turn LEDs on
            joust.led.connectionDim = 50;
            // Prepare the second dimming
            joust.led.ConnLedState = LED_DIM_2;
            break;
        }
        case LED_DIM_2:
        {
            // Dim leds
            joust.led.connectionDim -= 1;
            // If its off, start waiting
            if(joust.led.connectionDim == 0)
            {
                joust.led.ConnLedState = LED_OFF_WAIT;
            }
            break;
        }
        case LED_OFF_WAIT:
        {
            // Start a timer to update LEDs
            joustDisarmAllLedTimers();
            os_timer_arm(&joust.tmr.ConnLed, 1000, true);

            // When it fires, start all over again
            joust.led.ConnLedState = LED_OFF;

            // And dont update the LED state this time
            return;
        }
        case LED_CONNECTED_BRIGHT:
        case LED_CONNECTED_DIM:
        {
            // Handled in joustShowConnectionLedTimeout()
            break;
        }
        default:
        {
            break;
        }
    }

    // Copy the color value to all LEDs
    ets_memset(joust.led.Leds, 0, sizeof(joust.led.Leds));
    //just do blue for now
    uint8_t i;
    for(i = 0; i < 6; i ++)
    {
        joust.led.Leds[i].b = joust.led.connectionDim;
    }

    // Physically set the LEDs
    setLeds(joust.led.Leds, sizeof(joust.led.Leds));
}

/**
 * This is called whenever a button is pressed
 *
 * If a game is being played, check for button down events and either succeed
 * or fail the round and pass the result to the other swadge
 *
 * @param state  A bitmask of all button states
 * @param button The button which triggered this action
 * @param down   true if the button was pressed, false if it was released
 */
void ICACHE_FLASH_ATTR joustButton( uint8_t state __attribute__((unused)),
                                    int button __attribute__((unused)), int down __attribute__((unused)))
{
    if(!down)
    {
        // Ignore all button releases
        return;
    }

    // No matter what state left button bails out and restarts
    if(1 == button)
    {
        joustDisarmAllLedTimers();
        joustRestart(NULL);
        //os_timer_arm(&joust.tmr.RestartJoust, 100, false);
        //os_timer_arm(&joust.tmr.RestartJoust, 10, false);
    }

    if(joust.gameState == R_MENU)
    {
        // Stop the scrolling text
        os_timer_disarm(&joust.tmr.ScrollInstructions);

        if(2 == button)
        {
            joust.gameState =  R_SEARCHING;
            joustDisarmAllLedTimers();
            os_timer_arm(&joust.tmr.ConnLed, 1, true);
            p2pStartConnection(&joust.p2pJoust);
            clearDisplay();
            plotText(0, 0, "R_Searching", IBM_VGA_8, WHITE);
        }
    }
    else if(joust.gameState == R_PLAYING)
    {
        if(2 == button)
        {
            joust.gameState = R_GAME_OVER;
            joustSendRoundLossMsg();
        }
    }
}

/**
 * This is called when a round is lost. It tallies the loss, calls
 * joustRoundResultLed() to display the wins/losses and set up the
 * potential next round, and sends a message to the other swadge
 * that the round was lost and
 *
 * Send a round loss message to the other swadge
 */
void ICACHE_FLASH_ATTR joustSendRoundLossMsg(void)
{
    // Send a message to that ESP that we lost the round
    // If it's acked, start a timer to reinit if another message is never received
    // If it's not acked, reinit with refRestart()
    p2pSendMsg(&joust.p2pJoust, "los", NULL, 0, joustMsgTxCbFn);
    // Show the current wins & losses
}

/**
 * @brief TODO
 *
 * @param p2p
 * @param status
 */
void ICACHE_FLASH_ATTR joustMsgTxCbFn(p2pInfo* p2p __attribute__((unused)),
                                      messageStatus_t status)
{
    switch(status)
    {
        case MSG_ACKED:
        {
            joust_printf("%s MSG_ACKED\n", __func__);
            break;
        }
        case MSG_FAILED:
        {
            //TODO test case for this
            joust_printf("%s MSG_FAILED\n", __func__);
            joustDisarmAllLedTimers();
            os_timer_arm(&joust.tmr.RestartJoust, 100, false);
            break;
        }
        default:
        {
            joust_printf("%s UNKNOWN\n", __func__);
            break;
        }
    }
}

void ICACHE_FLASH_ATTR joustRoundResultLed(void* arg __attribute__((unused)))
{
    joust.led.connectionDim = 255;
    uint8_t i;
    if(joust.gam.round_winner)
    {
        for(i = 0; i < 6; i++)
        {
            joust.led.Leds[i].g = 40;
            joust.led.Leds[i].r = 0;
            joust.led.Leds[i].b = 0;
        }
    }
    else
    {
        for(i = 0; i < 6; i++)
        {
            joust.led.Leds[i].r = 40;
            joust.led.Leds[i].g = 0;
            joust.led.Leds[i].b = 0;
        }
    }

    setLeds(joust.led.Leds, sizeof(joust.led.Leds));
}

/**
 * Show the wins and losses
 *
 * @param roundWinner true if this swadge was a winner, false if the other
 *                    swadge won
 */
void ICACHE_FLASH_ATTR joustRoundResult(int roundWinner)
{
    joust.gameState = R_SHOW_GAME_RESULT;
    joustDisarmAllLedTimers();
    joust.gam.round_winner = roundWinner;
    os_timer_arm(&joust.tmr.RoundResultLed, 6, true);
    joust.gameState = R_SHOW_GAME_RESULT;
    if(roundWinner == 2)
    {
        clearDisplay();
        plotText(0, 0, "Tie!!", IBM_VGA_8, WHITE);
        joust.gam.joustWins = joust.gam.joustWins + 1;
    }
    else if(roundWinner)
    {
        clearDisplay();
        plotText(0, 0, "Winner!!", IBM_VGA_8, WHITE);
        joust.gam.joustWins = joust.gam.joustWins + 1;
    }
    else
    {
        clearDisplay();
        plotText(0, 0, "Loser", IBM_VGA_8, WHITE);
    }
    //Stop saving results in flash
    //setJoustWins(joust.gam.joustWins);
    os_timer_arm(&joust.tmr.RestartJoustPlay, 1000, false);
}
