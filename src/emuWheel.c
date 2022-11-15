/*
* Copyright 1990,91 by Thomas Roell, Dinkelscherben, Germany.
* Copyright 1993 by David Dawes <dawes@xfree86.org>
* Copyright 2002 by SuSE Linux AG, Author: Egbert Eich
* Copyright 1994-2002 by The XFree86 Project, Inc.
* Copyright 2002 by Paul Elliott
* (Ported from xf86-input-mouse, above copyrights taken from there)
* Copyright 2008 by Chris Salch
* Copyright © 2008 Red Hat, Inc.
*
* Permission to use, copy, modify, distribute, and sell this software
* and its documentation for any purpose is hereby granted without
* fee, provided that the above copyright notice appear in all copies
* and that both that copyright notice and this permission notice
* appear in supporting documentation, and that the name of the authors
* not be used in advertising or publicity pertaining to distribution of the
* software without specific, written prior permission.  The authors make no
* representations about the suitability of this software for any
* purpose.  It is provided "as is" without express or implied
* warranty.
*
* THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
* INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
* NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
* CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
* OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
* NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
* CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*
*/

/* Mouse wheel emulation code. */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "evdev.h"

#include <X11/Xatom.h>
#include <xf86.h>
#include <xf86Xinput.h>
#include <exevents.h>

#include <evdev-properties.h>

#define WHEEL_NOT_CONFIGURED 0

static Atom prop_wheel_emu      = 0;
static Atom prop_wheel_axismap  = 0;
static Atom prop_wheel_inertia  = 0;
static Atom prop_wheel_timeout  = 0;
static Atom prop_wheel_button   = 0;

/* States for wheel emulation in 2 button mode */
enum EmuWheelState {
    EMWHEEL_OFF,             /* default state */
    EMWHEEL_TIMER,           /* timer pending - 1st button click was filtered, wait for other button */
    EMWHEEL_EMULATING,       /* emulation active  */
    EMWHEEL_LEAVING          /* leaving emulation - need to filter other button release */
};

/* Local Function Prototypes */
static int EvdevWheelEmuInertia(InputInfoPtr pInfo, WheelAxisPtr axis, int value);

/**
 * Timer function 2 button mode.
 * in EMWHEEL_TIMER state post a delayed button press event to the server.
 *
 * @param arg The InputInfoPtr for this device.
 */

CARD32
EvdevWheelEmuTimer(OsTimerPtr timer, CARD32 time, pointer arg)
{
    InputInfoPtr      pInfo    = (InputInfoPtr)arg;
    EvdevPtr          pEvdev   = pInfo->private;
    struct emulateWheel *emuWheel    = &pEvdev->emulateWheel;

#if HAVE_THREADED_INPUT
    input_lock();
#else
    int sigstate = xf86BlockSIGIO();
#endif

    if (emuWheel->state_2B != EMWHEEL_TIMER || !emuWheel->delayed_button) {
        xf86IDrvMsg(pInfo, X_WARNING, "EmuWheel2B timeout - unexpected state %d or no delayed button\n",
                    emuWheel->state_2B);
    }
    if (emuWheel->state_2B == EMWHEEL_TIMER) {
        EvdevPostButtonEvent(pInfo, emuWheel->delayed_button, BUTTON_PRESS);
        emuWheel->delayed_button = 0;
        emuWheel->state_2B = EMWHEEL_OFF;
    }
#if HAVE_THREADED_INPUT
    input_unlock();
#else
    xf86UnblockSIGIO(sigstate);
#endif
    return 0;
}


void
EvdevWheelEmuFinalize(InputInfoPtr pInfo)
{
    EvdevPtr pEvdev = pInfo->private;
    struct emulateWheel *emulateWheel  = &pEvdev->emulateWheel;

    TimerFree(emulateWheel->timer);
    emulateWheel->timer = NULL;
}


/* Filter mouse button events */
BOOL
EvdevWheelEmuFilterButton(InputInfoPtr pInfo, unsigned int button, int value)
{
    EvdevPtr pEvdev = (EvdevPtr)pInfo->private;
    int ms;

    /* Has wheel emulation been configured to be enabled? */
    if (!pEvdev->emulateWheel.enabled)
	return FALSE;

    /* EmulateWheelButton - in 2 button mode. Process events from that buttons */

    if (pEvdev->emulateWheel.button2 &&
       (button == pEvdev->emulateWheel.button || button == pEvdev->emulateWheel.button2)) {
        /* button press events */
        if (value) {
            /* State0 - EMWHEEL_OFF */
            if (pEvdev->emulateWheel.state_2B == EMWHEEL_OFF) {
                /* 1st button pressed */
                pEvdev->emulateWheel.delayed_button = button;
                pEvdev->emulateWheel.state_2B = EMWHEEL_TIMER;
                /* Start the timer when the button is pressed */
                pEvdev->emulateWheel.expires = pEvdev->emulateWheel.timeout + GetTimeInMillis();
                pEvdev->emulateWheel.timer = TimerSet(pEvdev->emulateWheel.timer, 0,
                                                      pEvdev->emulateWheel.timeout,
                                                      EvdevWheelEmuTimer, pInfo);
            }
            /* State1 - EMWHEEL_TIMER */
            else if (pEvdev->emulateWheel.state_2B == EMWHEEL_TIMER &&
                     button != pEvdev->emulateWheel.delayed_button) {
                /* If the other button was pressed within time limit */
                ms = pEvdev->emulateWheel.expires - GetTimeInMillis();
                if (ms < 0) {
                    xf86IDrvMsg(pInfo, X_WARNING,
                                "EmuWheel2B unexpected timeout, ms %d button %d delayed button %d\n",
                                ms, button, pEvdev->emulateWheel.delayed_button);
                    TimerCancel(pEvdev->emulateWheel.timer);
                    pEvdev->emulateWheel.state_2B = EMWHEEL_OFF;
                    EvdevQueueButtonEvent(pInfo, pEvdev->emulateWheel.delayed_button, 1);
                    pEvdev->emulateWheel.delayed_button = 0;
                    return FALSE; /* pass also 2nd button press event */
                }
                pEvdev->emulateWheel.state_2B = EMWHEEL_EMULATING;
                pEvdev->emulateWheel.delayed_button = 0;
                TimerCancel(pEvdev->emulateWheel.timer);
                pEvdev->emulateWheel.button_state = 1; /* emulation active */
            }
            /* State2 - EMWHEEL_EMULATING - no state change. both buttons are held
                                            no press event is expected
               State3 - EMWHEEL_LEAVING - no state change.
                                          button press events are not considered for simplicity */
            else if (pEvdev->emulateWheel.state_2B == EMWHEEL_LEAVING
                     && button != pEvdev->emulateWheel.filter_button) {
                return TRUE;
            }
            else {
                xf86IDrvMsg(pInfo, X_WARNING, "EmuWheel2B unexpected event\n");
            }
            return TRUE; /* Filter this button press event */
        }
        /* button release events */
        else {
            /* State2 - EMWHEEL_EMULATING */
            if (pEvdev->emulateWheel.state_2B == EMWHEEL_EMULATING) {
                /* stop emulation, then filter release event of other button */
                pEvdev->emulateWheel.state_2B = EMWHEEL_LEAVING;
                pEvdev->emulateWheel.button_state = 0; /* emulation inactive */
                pEvdev->emulateWheel.filter_button = (button == pEvdev->emulateWheel.button) ?
                        pEvdev->emulateWheel.button2 : pEvdev->emulateWheel.button;
            }
            /* State3 - EMWHEEL_LEAVING */
            else if (pEvdev->emulateWheel.state_2B == EMWHEEL_LEAVING) {
                if (button == pEvdev->emulateWheel.filter_button) {
                    /* filter release event of other button, then return to default state */
                    pEvdev->emulateWheel.state_2B = EMWHEEL_OFF;
                    pEvdev->emulateWheel.filter_button = 0;
                }
            }
            /* State1 - EMWHEEL_TIMER */
            else if (pEvdev->emulateWheel.state_2B == EMWHEEL_TIMER) {
                /* stop waiting for the other button and pass delayed press event */
                TimerCancel(pEvdev->emulateWheel.timer);
                pEvdev->emulateWheel.state_2B = EMWHEEL_OFF;
                EvdevQueueButtonEvent(pInfo, pEvdev->emulateWheel.delayed_button, 1);
                pEvdev->emulateWheel.delayed_button = 0;
                return FALSE; /* pass this release event */
            }
            /* State0 - EMWHEEL_OFF - no state change */
            else
                return FALSE; /* pass this release event */
            return TRUE; /* Filter this button release event */
        }
    }

    /* EmulateWheelButton 2 button mode - events from other buttons */
    if (pEvdev->emulateWheel.button2) {
        /* when waiting for other button press, any other button event ends waiting */
        if (pEvdev->emulateWheel.state_2B == EMWHEEL_TIMER) {
            TimerCancel(pEvdev->emulateWheel.timer);
            pEvdev->emulateWheel.state_2B = EMWHEEL_OFF;
            EvdevQueueButtonEvent(pInfo, pEvdev->emulateWheel.delayed_button, 1);
            pEvdev->emulateWheel.delayed_button = 0;
        }
        return FALSE;
    }

    /* Check for EmulateWheelButton - in one button mode */
    if (pEvdev->emulateWheel.button == button) {
	pEvdev->emulateWheel.button_state = value;

        if (value)
            /* Start the timer when the button is pressed */
            pEvdev->emulateWheel.expires = pEvdev->emulateWheel.timeout +
                                           GetTimeInMillis();
        else {
            ms = pEvdev->emulateWheel.expires - GetTimeInMillis();
            if (ms > 0) {
                /*
                 * If the button is released early enough emit the button
                 * press/release events
                 */
                EvdevQueueButtonClicks(pInfo, button, 1);
            }
        }

	return TRUE;
    }

    /* Don't care about this event */
    return FALSE;
}

/* Filter mouse wheel events */
BOOL
EvdevWheelEmuFilterMotion(InputInfoPtr pInfo, struct input_event *pEv)
{
    EvdevPtr pEvdev = (EvdevPtr)pInfo->private;
    WheelAxisPtr pAxis = NULL;
    int value = pEv->value;

    /* Has wheel emulation been configured to be enabled? */
    if (!pEvdev->emulateWheel.enabled)
	return FALSE;

    /* Handle our motion events if the emuWheel button is pressed
     * wheel button of 0 means always emulate wheel.
     */
    if (pEvdev->emulateWheel.button_state || !pEvdev->emulateWheel.button) {
        /* Just return if the timeout hasn't expired yet */
        if (pEvdev->emulateWheel.button)
        {
            int ms = pEvdev->emulateWheel.expires - GetTimeInMillis();
            if (ms > 0)
                return TRUE;
        }

	if(pEv->type == EV_ABS) {
	    int axis = pEvdev->abs_axis_map[pEv->code];
	    int oldValue;

	    if (axis > -1 && valuator_mask_fetch(pEvdev->old_vals, axis, &oldValue)) {
		valuator_mask_set(pEvdev->abs_vals, axis, value);
		value -= oldValue; /* make value into a differential measurement */
	    } else
                value = 0; /* avoid a jump on the first touch */
	}

	switch(pEv->code) {

	/* ABS_X has the same value as REL_X, so this case catches both */
	case REL_X:
	    pAxis = &(pEvdev->emulateWheel.X);
	    break;

	/* ABS_Y has the same value as REL_Y, so this case catches both */
	case REL_Y:
	    pAxis = &(pEvdev->emulateWheel.Y);
	    break;

	default:
	    break;
	}

	/* If we found REL_X, REL_Y, ABS_X or ABS_Y then emulate a mouse
	   wheel.
	 */
	if (pAxis)
	    EvdevWheelEmuInertia(pInfo, pAxis, value);

	/* Eat motion events while emulateWheel button pressed. */
	return TRUE;
    }

    return FALSE;
}

/* Simulate inertia for our emulated mouse wheel.
   Returns the number of wheel events generated.
 */
static int
EvdevWheelEmuInertia(InputInfoPtr pInfo, WheelAxisPtr axis, int value)
{
    EvdevPtr pEvdev = (EvdevPtr)pInfo->private;
    int button;
    int inertia;
    int rc = 0;

    /* if this axis has not been configured, just eat the motion */
    if (!axis->up_button)
	return rc;

    axis->traveled_distance += value;

    if (axis->traveled_distance < 0) {
	button = axis->up_button;
	inertia = -pEvdev->emulateWheel.inertia;
    } else {
	button = axis->down_button;
	inertia = pEvdev->emulateWheel.inertia;
    }

    /* Produce button press events for wheel motion */
    while(abs(axis->traveled_distance) > pEvdev->emulateWheel.inertia) {
	axis->traveled_distance -= inertia;
	EvdevQueueButtonClicks(pInfo, button, 1);
	rc++;
    }
    return rc;
}

/* Handle button mapping here to avoid code duplication,
returns true if a button mapping was found. */
static BOOL
EvdevWheelEmuHandleButtonMap(InputInfoPtr pInfo, WheelAxisPtr pAxis,
                             const char *axis_name)
{
    EvdevPtr pEvdev = (EvdevPtr)pInfo->private;
    char *option_string;

    pAxis->up_button = WHEEL_NOT_CONFIGURED;

    /* Check to see if there is configuration for this axis */
    option_string = xf86SetStrOption(pInfo->options, axis_name, NULL);
    if (option_string) {
	int up_button = 0;
	int down_button = 0;
	char *msg = NULL;

	if ((sscanf(option_string, "%d %d", &up_button, &down_button) == 2) &&
	    ((up_button > 0) && (up_button <= EVDEV_MAXBUTTONS)) &&
	    ((down_button > 0) && (down_button <= EVDEV_MAXBUTTONS))) {

	    /* Use xstrdup to allocate a string for us */
	    msg = xstrdup("buttons XX and YY");

	    if (msg)
		sprintf(msg, "buttons %d and %d", up_button, down_button);

	    pAxis->up_button = up_button;
	    pAxis->down_button = down_button;

	    /* Update the number of buttons if needed */
	    if (up_button > pEvdev->num_buttons) pEvdev->num_buttons = up_button;
	    if (down_button > pEvdev->num_buttons) pEvdev->num_buttons = down_button;

	} else {
	    xf86IDrvMsg(pInfo, X_WARNING, "Invalid %s value:\"%s\"\n",
		        axis_name, option_string);
	}
	free(option_string);

	/* Clean up and log what happened */
	if (msg) {
	    xf86IDrvMsg(pInfo, X_CONFIG, "%s: %s\n", axis_name, msg);
	    free(msg);
	    return TRUE;
	}
    }
    return FALSE;
}

/* Setup the basic configuration options used by mouse wheel emulation */
void
EvdevWheelEmuPreInit(InputInfoPtr pInfo)
{
    EvdevPtr pEvdev = (EvdevPtr)pInfo->private;
    int wheelButton, wheelButton2;
    int inertia;
    int timeout;
    char *option_string = NULL;

    if (xf86SetBoolOption(pInfo->options, "EmulateWheel", FALSE)) {
	pEvdev->emulateWheel.enabled = TRUE;
    } else
        pEvdev->emulateWheel.enabled = FALSE;

    wheelButton = 4;
    wheelButton2 = 0;
    option_string = xf86SetStrOption(pInfo->options, "EmulateWheelButton", NULL);

    if (option_string) {
        int buttons = sscanf(option_string, "%d %d", &wheelButton, &wheelButton2);
        if (buttons >= 1) {
            int buttons_ok = TRUE;
            if (wheelButton < 0 || wheelButton > EVDEV_MAXBUTTONS) {
                xf86IDrvMsg(pInfo, X_WARNING, "Invalid EmulateWheelButton value: %d\n",
                                               wheelButton);
                buttons_ok = FALSE;
            }
            if ((buttons == 2) &&
                (wheelButton2 <= 0 || wheelButton2 > EVDEV_MAXBUTTONS || wheelButton2 == wheelButton)) {
                xf86IDrvMsg(pInfo, X_WARNING, "Invalid EmulateWheelButton 2nd value: %d\n",
                                               wheelButton2);
                buttons_ok = FALSE;
            }
            if (!buttons_ok) {
                pEvdev->emulateWheel.enabled = FALSE;
                xf86IDrvMsg(pInfo, X_WARNING, "Wheel emulation disabled.\n");
            }
        }
        else {
            xf86IDrvMsg(pInfo, X_WARNING, "Invalid EmulateWheelButton option\n");
        }
        free(option_string);
    }

    pEvdev->emulateWheel.button = wheelButton;
    pEvdev->emulateWheel.button2 = wheelButton2;

    inertia = xf86SetIntOption(pInfo->options, "EmulateWheelInertia", 10);

    if (inertia <= 0) {
        xf86IDrvMsg(pInfo, X_WARNING, "Invalid EmulateWheelInertia value: %d\n",
                    inertia);
        xf86IDrvMsg(pInfo, X_WARNING, "Using built-in inertia value.\n");

        inertia = 10;
    }

    pEvdev->emulateWheel.inertia = inertia;

    timeout = xf86SetIntOption(pInfo->options, "EmulateWheelTimeout", 200);

    if (timeout < 0) {
        xf86IDrvMsg(pInfo, X_WARNING, "Invalid EmulateWheelTimeout value: %d\n",
                    timeout);
        xf86IDrvMsg(pInfo, X_WARNING, "Using built-in timeout value.\n");

        timeout = 200;
    }

    pEvdev->emulateWheel.timeout = timeout;

    /* Configure the Y axis or default it */
    if (!EvdevWheelEmuHandleButtonMap(pInfo, &(pEvdev->emulateWheel.Y),
                "YAxisMapping")) {
        /* Default the Y axis to sane values */
        pEvdev->emulateWheel.Y.up_button = 4;
        pEvdev->emulateWheel.Y.down_button = 5;

        /* Simpler to check just the largest value in this case */
        /* XXX: we should post this to the server */
        if (5 > pEvdev->num_buttons)
            pEvdev->num_buttons = 5;

        /* Display default Configuration */
        xf86IDrvMsg(pInfo, X_CONFIG, "YAxisMapping: buttons %d and %d\n",
                    pEvdev->emulateWheel.Y.up_button,
                    pEvdev->emulateWheel.Y.down_button);
    }


    /* This axis should default to an unconfigured state as most people
       are not going to expect a Horizontal wheel. */
    EvdevWheelEmuHandleButtonMap(pInfo, &(pEvdev->emulateWheel.X),
            "XAxisMapping");

    /* Used by the inertia code */
    pEvdev->emulateWheel.X.traveled_distance = 0;
    pEvdev->emulateWheel.Y.traveled_distance = 0;

    xf86IDrvMsg(pInfo, X_CONFIG,
                "EmulateWheelButton: %d,%d "
                "EmulateWheelInertia: %d, "
                "EmulateWheelTimeout: %d\n",
                pEvdev->emulateWheel.button,
                pEvdev->emulateWheel.button2, inertia, timeout);

    pEvdev->emulateWheel.timer = TimerSet(NULL, 0, 0, NULL, NULL);
}

static int
EvdevWheelEmuSetProperty(DeviceIntPtr dev, Atom atom, XIPropertyValuePtr val,
                         BOOL checkonly)
{
    InputInfoPtr pInfo  = dev->public.devicePrivate;
    EvdevPtr     pEvdev = pInfo->private;

    if (atom == prop_wheel_emu)
    {
        if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        if (!checkonly)
        {
            pEvdev->emulateWheel.enabled = *((BOOL*)val->data);
            xf86IDrvMsg(pInfo, X_INFO, "Mouse wheel emulation %s.\n",
                        (pEvdev->emulateWheel.enabled) ? "enabled" : "disabled");
            /* Don't enable with zero inertia, otherwise we may get stuck in an
             * infinite loop */
            if (pEvdev->emulateWheel.inertia <= 0)
            {
                pEvdev->emulateWheel.inertia = 10;
                /* We may get here before the property is actually enabled */
                if (prop_wheel_inertia)
                    XIChangeDeviceProperty(dev, prop_wheel_inertia, XA_INTEGER,
                            16, PropModeReplace, 1,
                            &pEvdev->emulateWheel.inertia, TRUE);
            }
            pEvdev->emulateWheel.state_2B = EMWHEEL_OFF;  /* reset 2B state */
        }
    }
    else if (atom == prop_wheel_button)
    {
        int bt, bt2 = 0;

        if (val->format != 8 || val->size < 1 || val->size > 2 || val->type != XA_INTEGER)
            return BadMatch;

        bt = *((CARD8*)val->data);

        if (val->size == 2) {
            bt2 = ((CARD8*)val->data)[1];
            if (bt2 <= 0 || bt2 >= EVDEV_MAXBUTTONS || bt2 == bt) {
                xf86IDrvMsg(pInfo, X_WARNING, "Invalid EmulateWheelButton 2nd value: %d\n",
                                               bt2);
                return BadValue;
            }
        }
        if (bt < 0 || bt >= EVDEV_MAXBUTTONS) {
            xf86IDrvMsg(pInfo, X_WARNING, "Invalid EmulateWheelButton value: %d\n",
                                           bt);
            return BadValue;
        }
        if (!checkonly) {
            pEvdev->emulateWheel.button = bt;
            pEvdev->emulateWheel.button2 = bt2;
            pEvdev->emulateWheel.state_2B = EMWHEEL_OFF; /* reset state to default */
        }
    } else if (atom == prop_wheel_axismap)
    {
        if (val->format != 8 || val->size != 4 || val->type != XA_INTEGER)
            return BadMatch;

        if (!checkonly)
        {
            pEvdev->emulateWheel.X.up_button = *((CARD8*)val->data);
            pEvdev->emulateWheel.X.down_button = *(((CARD8*)val->data) + 1);
            pEvdev->emulateWheel.Y.up_button = *(((CARD8*)val->data) + 2);
            pEvdev->emulateWheel.Y.down_button = *(((CARD8*)val->data) + 3);
        }
    } else if (atom == prop_wheel_inertia)
    {
        int inertia;

        if (val->format != 16 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        inertia = *((CARD16*)val->data);

        if (inertia <= 0)
            return BadValue;

        if (!checkonly)
            pEvdev->emulateWheel.inertia = inertia;
    } else if (atom == prop_wheel_timeout)
    {
        int timeout;

        if (val->format != 16 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        timeout = *((CARD16*)val->data);

        if (timeout < 0)
            return BadValue;

        if (!checkonly)
            pEvdev->emulateWheel.timeout = timeout;
    }
    return Success;
}

void
EvdevWheelEmuInitProperty(DeviceIntPtr dev)
{
    InputInfoPtr pInfo  = dev->public.devicePrivate;
    EvdevPtr     pEvdev = pInfo->private;
    int          rc     = TRUE;
    char         vals[4];

    if (!dev->button) /* don't init prop for keyboards */
        return;

    prop_wheel_emu = MakeAtom(EVDEV_PROP_WHEEL, strlen(EVDEV_PROP_WHEEL), TRUE);
    rc = XIChangeDeviceProperty(dev, prop_wheel_emu, XA_INTEGER, 8,
                                PropModeReplace, 1,
                                &pEvdev->emulateWheel.enabled, FALSE);
    if (rc != Success)
        return;

    XISetDevicePropertyDeletable(dev, prop_wheel_emu, FALSE);

    vals[0] = pEvdev->emulateWheel.X.up_button;
    vals[1] = pEvdev->emulateWheel.X.down_button;
    vals[2] = pEvdev->emulateWheel.Y.up_button;
    vals[3] = pEvdev->emulateWheel.Y.down_button;

    prop_wheel_axismap = MakeAtom(EVDEV_PROP_WHEEL_AXES, strlen(EVDEV_PROP_WHEEL_AXES), TRUE);
    rc = XIChangeDeviceProperty(dev, prop_wheel_axismap, XA_INTEGER, 8,
                                PropModeReplace, 4, vals, FALSE);

    if (rc != Success)
        return;

    XISetDevicePropertyDeletable(dev, prop_wheel_axismap, FALSE);

    prop_wheel_inertia = MakeAtom(EVDEV_PROP_WHEEL_INERTIA, strlen(EVDEV_PROP_WHEEL_INERTIA), TRUE);
    rc = XIChangeDeviceProperty(dev, prop_wheel_inertia, XA_INTEGER, 16,
                                PropModeReplace, 1,
                                &pEvdev->emulateWheel.inertia, FALSE);
    if (rc != Success)
        return;

    XISetDevicePropertyDeletable(dev, prop_wheel_inertia, FALSE);

    prop_wheel_timeout = MakeAtom(EVDEV_PROP_WHEEL_TIMEOUT, strlen(EVDEV_PROP_WHEEL_TIMEOUT), TRUE);
    rc = XIChangeDeviceProperty(dev, prop_wheel_timeout, XA_INTEGER, 16,
                                PropModeReplace, 1,
                                &pEvdev->emulateWheel.timeout, FALSE);
    if (rc != Success)
        return;

    XISetDevicePropertyDeletable(dev, prop_wheel_timeout, FALSE);

    prop_wheel_button = MakeAtom(EVDEV_PROP_WHEEL_BUTTON, strlen(EVDEV_PROP_WHEEL_BUTTON), TRUE);
    vals[0] = pEvdev->emulateWheel.button;
    vals[1] = pEvdev->emulateWheel.button2;
    rc = XIChangeDeviceProperty(dev, prop_wheel_button, XA_INTEGER, 8,
                                PropModeReplace, 2,
                                vals, FALSE);
    if (rc != Success)
        return;

    XISetDevicePropertyDeletable(dev, prop_wheel_button, FALSE);

    XIRegisterPropertyHandler(dev, EvdevWheelEmuSetProperty, NULL, NULL);
}
