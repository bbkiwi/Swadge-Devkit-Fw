/*
 * mode_test.c
 *
 *  Example of zeroing out an armed timer killing the accelerometer timer
 * Pushing Left button will fail to disarm a timer and then testEnterMode
 *  again. testEnterMode zeros out all os_timer_t.
 *  You may need to push L a few times for it to kill
 *  static os_timer_t timerHandlePollAccel set up in user_main.c
 * Serial output is button state and . for each call to testAccelerometerHandler
 * The oled is driven by timerHandlePollAccel inside testAccelerometerHandler
 * The LEDs are driven by both timerHandlePollAccel AND timerHandleLeds
 *    so when timerHandlePollAccel dies, the readings stop, the . stop printing
 *    and LEDs slow down
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
#include "galleryImages.h"

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
void ICACHE_FLASH_ATTR testBadExitMode(void);
void ICACHE_FLASH_ATTR testButtonCallback(uint8_t state __attribute__((unused)),
        int button, int down);
void ICACHE_FLASH_ATTR testAccelerometerHandler(accel_t* accel);

void ICACHE_FLASH_ATTR testUpdateDisplay(void);
static void ICACHE_FLASH_ATTR testDummyCallback(void* arg __attribute__((unused)));
static void ICACHE_FLASH_ATTR testLedFunc(void* arg __attribute__((unused)));


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
    .fnAccelerometerCallback = testAccelerometerHandler,
    .menuImageData = gal_bongo_0,
    .menuImageLen = sizeof(gal_bongo_0)
};

struct
{
    // Callback variables
    accel_t Accel;
    uint8_t ButtonState;

    // Timer variables
    os_timer_t timerHandleLeds;
    os_timer_t timerKiller;
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


    // Test the LEDs
    os_timer_disarm(&test.timerHandleLeds);
    os_timer_setfn(&test.timerHandleLeds, (os_timer_func_t*)testLedFunc, NULL);
    os_timer_arm(&test.timerHandleLeds, 1000, 1);

    // Set up a timer that won't be disarmed if L button pushed
    os_timer_disarm(&test.timerKiller);
    os_timer_setfn(&test.timerKiller, (os_timer_func_t*)testDummyCallback, NULL);
    os_timer_arm(&test.timerKiller, 100, 1);

}

/**
 * Called when test is exited
 */
void ICACHE_FLASH_ATTR testExitMode(void)
{
    os_timer_disarm(&test.timerKiller);
    os_timer_disarm(&test.timerHandleLeds);
}

void ICACHE_FLASH_ATTR testBadExitMode(void)
{
    // Forget to disarm &test.timerKiller
    //os_timer_disarm(&test.timerKiller);
    os_timer_disarm(&test.timerHandleLeds);
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
 * @brief Called on a timer
 *
 * @param arg unused
 */
static void ICACHE_FLASH_ATTR testDummyCallback(void* arg __attribute__((unused)))
{

}


/**
 * TODO
 */
void ICACHE_FLASH_ATTR testUpdateDisplay(void)
{
    // Clear the display
    clearDisplay();

    // Draw a title
    plotText(0, 0, "L kills accel", IBM_VGA_8, WHITE);

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

}

/**
 * TODO
 *
 * @param state  A bitmask of all button states, unused
 * @param button The button which triggered this event
 * @param down   true if the button was pressed, false if it was released
 */
void ICACHE_FLASH_ATTR testButtonCallback( uint8_t state, int button, int down)
{
    os_printf("btn: %d\n", state);
    test.ButtonState = state;

    if(!down)
    {
        // Ignore all button releases
        return;
    }
    if(1 == button)
    {
        //Kill accelerometer timer!
        testBadExitMode();
        testEnterMode();
    }
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
    os_printf(".");
    test.Accel.x = accel->x;
    test.Accel.y = accel->y;
    test.Accel.z = accel->z;
    testUpdateDisplay();
    testLedFunc(NULL);
}

#endif