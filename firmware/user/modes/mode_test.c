/*
 * mode_test.c
 *
 *  Created on: Mar 27, 2019
 *      Author: adam
 */
#include "user_config.h"
#ifdef TEST_MODE

/*============================================================================
 * Includes
 *==========================================================================*/

#include <osapi.h>
#include <stdlib.h>

#include "user_main.h"
#include "mode_test.h"
#include "DFT32.h"
#include "embeddedout.h"
#include "oled.h"
#include "sprite.h"
#include "font.h"
#include "bresenham.h"
#include "buttons.h"
#include "hpatimer.h"

/*============================================================================
 * Defines
 *==========================================================================*/

#define BTN_CTR_X 96
#define BTN_CTR_Y 40
#define BTN_RAD    8
#define BTN_OFF   12

/*============================================================================
 * Prototypes
 *==========================================================================*/

void ICACHE_FLASH_ATTR testEnterMode(void);
void ICACHE_FLASH_ATTR testExitMode(void);
void ICACHE_FLASH_ATTR testButtonCallback(uint8_t state __attribute__((unused)),
        int button, int down);
void ICACHE_FLASH_ATTR testAccelerometerHandler(accel_t* accel);

void ICACHE_FLASH_ATTR testUpdateDisplay(void);
static void ICACHE_FLASH_ATTR testRotateBanana(void* arg __attribute__((unused)));
static void ICACHE_FLASH_ATTR testLedFunc(void* arg __attribute__((unused)));

/*============================================================================
 * Const data
 *==========================================================================*/

const song_t BlackDog ICACHE_RODATA_ATTR =
{
    .notes = {
        {.note = E_5, .timeMs = 188},
        {.note = G_5, .timeMs = 188},
        {.note = G_SHARP_5, .timeMs = 188},
        {.note = A_5, .timeMs = 188},
        {.note = E_5, .timeMs = 188},
        {.note = C_6, .timeMs = 375},
        {.note = A_5, .timeMs = 375},
        {.note = D_6, .timeMs = 188},
        {.note = E_6, .timeMs = 188},
        {.note = C_6, .timeMs = 94},
        {.note = D_6, .timeMs = 94},
        {.note = C_6, .timeMs = 188},
        {.note = A_5, .timeMs = 188},
        {.note = A_5, .timeMs = 188},
        {.note = C_6, .timeMs = 375},
        {.note = A_5, .timeMs = 375},
        {.note = G_5, .timeMs = 188},
        {.note = A_5, .timeMs = 188},
        {.note = A_5, .timeMs = 188},
        {.note = D_5, .timeMs = 188},
        {.note = E_5, .timeMs = 188},
        {.note = C_5, .timeMs = 188},
        {.note = D_5, .timeMs = 188},
        {.note = A_4, .timeMs = 375},
        {.note = A_4, .timeMs = 750},
    },
    .numNotes = 25,
    .shouldLoop = true
};

const sprite_t rotating_banana[] ICACHE_RODATA_ATTR =
{
    // frame_0_delay-0.07s.png
    {
        .width = 16,
        .height = 16,
        .data =
        {
            0b0000000010000000,
            0b0000000110000000,
            0b0000000011000000,
            0b0000000010100000,
            0b0000000010010000,
            0b0000000100010000,
            0b0000000100001000,
            0b0000000100001000,
            0b0000000100001000,
            0b0000001000001000,
            0b0000010000001000,
            0b0001100000010000,
            0b0110000000100000,
            0b0100000011000000,
            0b0110011100000000,
            0b0001100000000000,
        }
    },
    // frame_1_delay-0.07s.png
    {
        .width = 16,
        .height = 16,
        .data =
        {
            0b0000000100000000,
            0b0000000110000000,
            0b0000000110000000,
            0b0000000110000000,
            0b0000001001000000,
            0b0000001001000000,
            0b0000001000100000,
            0b0000001000100000,
            0b0000010000100000,
            0b0000010000100000,
            0b0000010000100000,
            0b0000100000100000,
            0b0001000001000000,
            0b0001000011000000,
            0b0000111100000000,
            0b0000000000000000,
        }
    },
    // frame_2_delay-0.07s.png
    {
        .width = 16,
        .height = 16,
        .data =
        {
            0b0000000100000000,
            0b0000001100000000,
            0b0000001100000000,
            0b0000010100000000,
            0b0000010100000000,
            0b0000100100000000,
            0b0000100010000000,
            0b0001000010000000,
            0b0001000010000000,
            0b0001000010000000,
            0b0001000001000000,
            0b0000100001000000,
            0b0000100001000000,
            0b0000011001000000,
            0b0000000110000000,
            0b0000000000000000,
        }
    },
    // frame_3_delay-0.07s.png
    {
        .width = 16,
        .height = 16,
        .data =
        {
            0b0000001100000000,
            0b0000001100000000,
            0b0000011000000000,
            0b0000101000000000,
            0b0001001000000000,
            0b0010001000000000,
            0b0010000100000000,
            0b0010000100000000,
            0b0010000100000000,
            0b0010000010000000,
            0b0001000001000000,
            0b0001000000110000,
            0b0000100000001000,
            0b0000011000000100,
            0b0000000111111000,
            0b0000000000000000,
        }
    },
    // frame_4_delay-0.07s.png
    {
        .width = 16,
        .height = 16,
        .data =
        {
            0b0000001100000000,
            0b0000001100000000,
            0b0000011000000000,
            0b0000101000000000,
            0b0001001100000000,
            0b0010000100000000,
            0b0010000100000000,
            0b0010000100000000,
            0b0010000010000000,
            0b0010000010000000,
            0b0001000001000000,
            0b0001000000100000,
            0b0000100000011000,
            0b0000010000000100,
            0b0000001100000100,
            0b0000000011111100,
        }
    },
    // frame_5_delay-0.07s.png
    {
        .width = 16,
        .height = 16,
        .data =
        {
            0b0000000100000000,
            0b0000001100000000,
            0b0000001100000000,
            0b0000010010000000,
            0b0000010010000000,
            0b0000100010000000,
            0b0000100001000000,
            0b0000100001000000,
            0b0000100001000000,
            0b0000100001000000,
            0b0000100000100000,
            0b0000010000100000,
            0b0000010000100000,
            0b0000001000010000,
            0b0000001000010000,
            0b0000000111110000,
        }
    },
    // frame_6_delay-0.07s.png
    {
        .width = 16,
        .height = 16,
        .data =
        {
            0b0000000100000000,
            0b0000000110000000,
            0b0000000101000000,
            0b0000000100100000,
            0b0000000100100000,
            0b0000001000100000,
            0b0000001000010000,
            0b0000001000010000,
            0b0000001000010000,
            0b0000010000010000,
            0b0000010000100000,
            0b0000010000100000,
            0b0000010000100000,
            0b0000010000100000,
            0b0000100001000000,
            0b0000111110000000,
        }
    },
    // frame_7_delay-0.07s.png
    {
        .width = 16,
        .height = 16,
        .data =
        {
            0b0000000110000000,
            0b0000000011000000,
            0b0000000010100000,
            0b0000000010100000,
            0b0000000010010000,
            0b0000000100001000,
            0b0000000100001000,
            0b0000000100001000,
            0b0000000100001000,
            0b0000001000001000,
            0b0000010000010000,
            0b0000100000010000,
            0b0001000000100000,
            0b0010000001000000,
            0b0100000010000000,
            0b0111111100000000,
        }
    },
};

/*============================================================================
 * Variables
 *==========================================================================*/

swadgeMode testMode =
{
    .modeName = "test",
    .fnEnterMode = testEnterMode,
    .fnExitMode = testExitMode,
    .fnButtonCallback = testButtonCallback,
    .wifiMode = SOFT_AP,
    .fnEspNowRecvCb = NULL,
    .fnEspNowSendCb = NULL,
    .fnAccelerometerCallback = testAccelerometerHandler
};

struct
{
    // Callback variables
    accel_t Accel;
    uint8_t ButtonState;

    // Timer variables
    os_timer_t TimerHandleLeds;
    os_timer_t timerHandleBanana;
    uint8_t BananaIdx;
} test;

/*============================================================================
 * Functions
 *==========================================================================*/

/**
 * Initializer for test
 */
void ICACHE_FLASH_ATTR testEnterMode(void)
{
    enableDebounce(false);

    // Clear everything
    memset(&test, 0, sizeof(test));

    // Test the buzzer
    startBuzzerSong(&BlackDog);

    // Test the display with a rotating banana
    os_timer_disarm(&test.timerHandleBanana);
    os_timer_setfn(&test.timerHandleBanana, (os_timer_func_t*)testRotateBanana, NULL);
    os_timer_arm(&test.timerHandleBanana, 100, 1);

    // Test the LEDs
    os_timer_disarm(&test.TimerHandleLeds);
    os_timer_setfn(&test.TimerHandleLeds, (os_timer_func_t*)testLedFunc, NULL);
    os_timer_arm(&test.TimerHandleLeds, 1000, 1);
}

/**
 * Called when test is exited
 */
void ICACHE_FLASH_ATTR testExitMode(void)
{
    stopBuzzerSong();
    os_timer_disarm(&test.timerHandleBanana);
    os_timer_disarm(&test.TimerHandleLeds);
}

/**
 * @brief called on a timer, this blinks an LED pattern
 *
 * @param arg
 */
static void ICACHE_FLASH_ATTR testLedFunc(void* arg __attribute__((unused)))
{
    led_t leds[NUM_LIN_LEDS] = {{0}};
    static int ledPos = 0;
    ledPos = (ledPos + 1) % NUM_LIN_LEDS;
    leds[(ledPos + 0) % NUM_LIN_LEDS].r = 16;
    leds[(ledPos + 1) % NUM_LIN_LEDS].g = 16;
    leds[(ledPos + 2) % NUM_LIN_LEDS].b = 16;
    setLeds(leds, sizeof(leds));
}

/**
 * @brief Called on a timer, this rotates the banana by picking the next sprite
 *
 * @param arg unused
 */
static void ICACHE_FLASH_ATTR testRotateBanana(void* arg __attribute__((unused)))
{
    test.BananaIdx = (test.BananaIdx + 1) % (sizeof(rotating_banana) / sizeof(rotating_banana[0]));
    testUpdateDisplay();
}

/**
 * TODO
 */
void ICACHE_FLASH_ATTR testUpdateDisplay(void)
{
    // Clear the display
    clearDisplay();

    // Draw a title
    plotText(0, 0, "TEST MODE", RADIOSTARS, WHITE);

    // Display the acceleration on the display
    char accelStr[32] = {0};

    ets_snprintf(accelStr, sizeof(accelStr), "X:%d", test.Accel.x);
    plotText(0, OLED_HEIGHT - (3 * (FONT_HEIGHT_IBMVGA8 + 1)), accelStr, IBM_VGA_8, WHITE);

    ets_snprintf(accelStr, sizeof(accelStr), "Y:%d", test.Accel.y);
    plotText(0, OLED_HEIGHT - (2 * (FONT_HEIGHT_IBMVGA8 + 1)), accelStr, IBM_VGA_8, WHITE);

    ets_snprintf(accelStr, sizeof(accelStr), "Z:%d", test.Accel.z);
    plotText(0, OLED_HEIGHT - (1 * (FONT_HEIGHT_IBMVGA8 + 1)), accelStr, IBM_VGA_8, WHITE);

    if(abs(test.Accel.x) > abs(test.Accel.y) &&
            abs(test.Accel.x) > abs(test.Accel.z))
    {
        // X is biggest
        if(test.Accel.x > 0)
        {
            plotText(64, OLED_HEIGHT - FONT_HEIGHT_IBMVGA8, "+X", IBM_VGA_8, WHITE);
        }
        else if(test.Accel.x < 0)
        {
            plotText(64, OLED_HEIGHT - FONT_HEIGHT_IBMVGA8, "-X", IBM_VGA_8, WHITE);
        }
        else
        {
            plotText(64, OLED_HEIGHT - FONT_HEIGHT_IBMVGA8, " X", IBM_VGA_8, WHITE);
        }
    }
    else if (abs(test.Accel.y) > abs(test.Accel.z))
    {
        // Y is biggest
        if(test.Accel.y > 0)
        {
            plotText(64, OLED_HEIGHT - FONT_HEIGHT_IBMVGA8, "+Y", IBM_VGA_8, WHITE);
        }
        else if(test.Accel.y < 0)
        {
            plotText(64, OLED_HEIGHT - FONT_HEIGHT_IBMVGA8, "-Y", IBM_VGA_8, WHITE);
        }
        else
        {
            plotText(64, OLED_HEIGHT - FONT_HEIGHT_IBMVGA8, " Y", IBM_VGA_8, WHITE);
        }
    }
    else
    {
        // Z is biggest
        if(test.Accel.z > 0)
        {
            plotText(64, OLED_HEIGHT - FONT_HEIGHT_IBMVGA8, "+Z", IBM_VGA_8, WHITE);
        }
        else if(test.Accel.z < 0)
        {
            plotText(64, OLED_HEIGHT - FONT_HEIGHT_IBMVGA8, "-Z", IBM_VGA_8, WHITE);
        }
        else
        {
            plotText(64, OLED_HEIGHT - FONT_HEIGHT_IBMVGA8, " Z", IBM_VGA_8, WHITE);
        }
    }

    if(test.ButtonState & UP)
    {
        // Up
        plotCircle(BTN_CTR_X, BTN_CTR_Y - BTN_OFF, BTN_RAD, WHITE);
    }
    if(test.ButtonState & LEFT)
    {
        // Left
        plotCircle(BTN_CTR_X - BTN_OFF, BTN_CTR_Y, BTN_RAD, WHITE);
    }
    if(test.ButtonState & RIGHT)
    {
        // Right
        plotCircle(BTN_CTR_X + BTN_OFF, BTN_CTR_Y, BTN_RAD, WHITE);
    }

    // Draw the banana
    plotSprite(50, 32, &rotating_banana[test.BananaIdx], WHITE);
}

/**
 * TODO
 *
 * @param state  A bitmask of all button states, unused
 * @param button The button which triggered this event
 * @param down   true if the button was pressed, false if it was released
 */
void ICACHE_FLASH_ATTR testButtonCallback( uint8_t state,
        int button __attribute__((unused)), int down __attribute__((unused)))
{
    os_printf("btn: %d\n", state);
    test.ButtonState = state;
    testUpdateDisplay();
}

/**
 * Store the acceleration data to be displayed later
 *
 * @param x
 * @param x
 * @param z
 */
void ICACHE_FLASH_ATTR testAccelerometerHandler(accel_t* accel)
{
    test.Accel.x = accel->x;
    test.Accel.y = accel->y;
    test.Accel.z = accel->z;
    testUpdateDisplay();
}

#endif