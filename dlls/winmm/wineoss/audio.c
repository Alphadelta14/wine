/* -*- tab-width: 8; c-basic-offset: 4 -*- */
/*				   
 * Sample Wine Driver for Open Sound System (featured in Linux and FreeBSD)
 *
 * Copyright 1994 Martin Ayotte
 *           1999 Eric Pouech (async playing in waveOut/waveIn)
 *	     2000 Eric Pouech (loops in waveOut)
 */
/*
 * FIXME:
 *	pause in waveOut does not work correctly
 *	full duplex (in/out) is not working (device is opened twice for Out 
 *	and In) (OSS is known for its poor duplex capabilities, alsa is
 *	better)
 */

/*#define EMULATE_SB16*/

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#ifdef HAVE_SYS_MMAN_H
# include <sys/mman.h>
#endif
#include "windef.h"
#include "wingdi.h"
#include "winerror.h"
#include "wine/winuser16.h"
#include "mmddk.h"
#include "dsound.h"
#include "dsdriver.h"
#include "oss.h"
#include "debugtools.h"

DEFAULT_DEBUG_CHANNEL(wave);

/* Allow 1% deviation for sample rates (some ES137x cards) */
#define NEAR_MATCH(rate1,rate2) (((100*((int)(rate1)-(int)(rate2)))/(rate1))==0)

#define REFILL_BUFFER_WHEN 3/4 /* fill the buffer when it's 3/4 empty */

#ifdef HAVE_OSS

#define SOUND_DEV "/dev/dsp"
#define MIXER_DEV "/dev/mixer"

#define MAX_WAVEOUTDRV 	(1)
#define MAX_WAVEINDRV 	(1)

/* state diagram for waveOut writing:
 *
 * +---------+-------------+---------------+---------------------------------+
 * |  state  |  function   |     event     |            new state	     |
 * +---------+-------------+---------------+---------------------------------+
 * |	     | open()	   |		   | STOPPED		       	     |
 * | PAUSED  | write()	   | 		   | PAUSED		       	     |
 * | STOPPED | write()	   | <thrd create> | PLAYING		  	     |
 * | PLAYING | write()	   | HEADER        | PLAYING		  	     |
 * | (other) | write()	   | <error>       |		       		     |
 * | (any)   | pause()	   | PAUSING	   | PAUSED		       	     |
 * | PAUSED  | restart()   | RESTARTING    | PLAYING (if no thrd => STOPPED) |
 * | (any)   | reset()	   | RESETTING     | STOPPED		      	     |
 * | (any)   | close()	   | CLOSING	   | CLOSED		      	     |
 * +---------+-------------+---------------+---------------------------------+
 */

/* states of the playing device */
#define	WINE_WS_PLAYING		0
#define	WINE_WS_PAUSED		1
#define	WINE_WS_STOPPED		2
#define WINE_WS_CLOSED		3

/* events to be send to device */
enum win_wm_message {
    WINE_WM_PAUSING = WM_USER + 1, WINE_WM_RESTARTING, WINE_WM_RESETTING, WINE_WM_HEADER,
    WINE_WM_UPDATE, WINE_WM_BREAKLOOP, WINE_WM_CLOSING
};

typedef struct {
    enum win_wm_message 	msg;	/* message identifier */
    DWORD	                param;  /* parameter for this message */
    HANDLE	                hEvent;	/* if message is synchronous, handle of event for synchro */
} OSS_MSG;

/* implement an in-process message ring for better performance 
 * (compared to passing thru the server)
 * this ring will be used by the input (resp output) record (resp playback) routine
 */
typedef struct {
#define OSS_RING_BUFFER_SIZE	30
    OSS_MSG			messages[OSS_RING_BUFFER_SIZE];
    int				msg_tosave;
    int				msg_toget;
    HANDLE			msg_event;
    CRITICAL_SECTION		msg_crst;
} OSS_MSG_RING;

typedef struct {
    int				unixdev;
    volatile int		state;			/* one of the WINE_WS_ manifest constants */
    WAVEOPENDESC		waveDesc;
    WORD			wFlags;
    PCMWAVEFORMAT		format;
    WAVEOUTCAPSA		caps;

    /* OSS information */
    DWORD			dwFragmentSize;		/* size of OSS buffer fragment */
    DWORD                       dwBufferSize;           /* size of whole OSS buffer in bytes */
    WORD                        uWaitForFragments;      /* The number of OSS buffer fragments we would like to be free
							 * before trying to write to the DSP
							 */
    LPWAVEHDR			lpQueuePtr;		/* start of queued WAVEHDRs (waiting to be notified) */
    LPWAVEHDR			lpPlayPtr;		/* start of not yet fully played buffers */
    DWORD			dwPartialOffset;	/* Offset of not yet written bytes in lpPlayPtr */

    LPWAVEHDR			lpLoopPtr;              /* pointer of first buffer in loop, if any */
    DWORD			dwLoops;		/* private copy of loop counter */
    
    DWORD			dwPlayedTotal;		/* number of bytes actually played since opening */
    DWORD                       dwWrittenTotal;         /* number of bytes written to OSS buffer since opening */

    /* synchronization stuff */
    HANDLE			hStartUpEvent;
    HANDLE			hThread;
    DWORD			dwThreadID;
    OSS_MSG_RING		msgRing;

    /* DirectSound stuff */
    LPBYTE			mapping;
    DWORD			maplen;
} WINE_WAVEOUT;

typedef struct {
    int				unixdev;
    volatile int		state;
    DWORD			dwFragmentSize;		/* OpenSound '/dev/dsp' give us that size */
    WAVEOPENDESC		waveDesc;
    WORD			wFlags;
    PCMWAVEFORMAT		format;
    LPWAVEHDR			lpQueuePtr;
    DWORD			dwTotalRecorded;
    WAVEINCAPSA			caps;
    BOOL                        bTriggerSupport;

    /* synchronization stuff */
    HANDLE			hThread;
    DWORD			dwThreadID;
    HANDLE			hStartUpEvent;
    OSS_MSG_RING		msgRing;
} WINE_WAVEIN;

static WINE_WAVEOUT	WOutDev   [MAX_WAVEOUTDRV];
static WINE_WAVEIN	WInDev    [MAX_WAVEINDRV ];

static DWORD wodDsCreate(UINT wDevID, PIDSDRIVER* drv);
 
/* These strings used only for tracing */
static const char *wodPlayerCmdString[] = {
    "WINE_WM_PAUSING",
    "WINE_WM_RESTARTING",
    "WINE_WM_RESETTING",
    "WINE_WM_HEADER",
    "WINE_WM_UPDATE",
    "WINE_WM_CLOSING",
    "WINE_WM_BREAKLOOP",
};

/*======================================================================*
 *                  Low level WAVE implementation			*
 *======================================================================*/

/******************************************************************
 *		OSS_WaveInit
 *
 * Initialize internal structures from OSS information
 */
LONG OSS_WaveInit(void)
{
    int 	audio;
    int		smplrate;
    int		samplesize = 16;
    int		dsp_stereo = 1;
    int		bytespersmpl;
    int 	caps;
    int		mask;
    int 	i;

    
    /* start with output device */

    /* initialize all device handles to -1 */
    for (i = 0; i < MAX_WAVEOUTDRV; ++i)
    {
	WOutDev[i].unixdev = -1;
    }

    /* FIXME: only one device is supported */
    memset(&WOutDev[0].caps, 0, sizeof(WOutDev[0].caps));

    if (access(SOUND_DEV,0) != 0 ||
	(audio = open(SOUND_DEV, O_WRONLY|O_NDELAY, 0)) == -1) {
	WARN("Couldn't open out %s (%s)\n", SOUND_DEV, strerror(errno));
	return -1;
    }

    ioctl(audio, SNDCTL_DSP_RESET, 0);

    /* FIXME: some programs compare this string against the content of the registry
     * for MM drivers. The names have to match in order for the program to work 
     * (e.g. MS win9x mplayer.exe)
     */
#ifdef EMULATE_SB16
    WOutDev[0].caps.wMid = 0x0002;
    WOutDev[0].caps.wPid = 0x0104;
    strcpy(WOutDev[0].caps.szPname, "SB16 Wave Out");
#else
    WOutDev[0].caps.wMid = 0x00FF; 	/* Manufac ID */
    WOutDev[0].caps.wPid = 0x0001; 	/* Product ID */
    /*    strcpy(WOutDev[0].caps.szPname, "OpenSoundSystem WAVOUT Driver");*/
    strcpy(WOutDev[0].caps.szPname, "CS4236/37/38");
#endif
    WOutDev[0].caps.vDriverVersion = 0x0100;
    WOutDev[0].caps.dwFormats = 0x00000000;
    WOutDev[0].caps.dwSupport = WAVECAPS_VOLUME;
    
    IOCTL(audio, SNDCTL_DSP_GETFMTS, mask);
    TRACE("OSS dsp out mask=%08x\n", mask);

    /* First bytespersampl, then stereo */
    bytespersmpl = (IOCTL(audio, SNDCTL_DSP_SAMPLESIZE, samplesize) != 0) ? 1 : 2;
    
    WOutDev[0].caps.wChannels = (IOCTL(audio, SNDCTL_DSP_STEREO, dsp_stereo) != 0) ? 1 : 2;
    if (WOutDev[0].caps.wChannels > 1) WOutDev[0].caps.dwSupport |= WAVECAPS_LRVOLUME;
    
    smplrate = 44100;
    if (IOCTL(audio, SNDCTL_DSP_SPEED, smplrate) == 0) {
	if (mask & AFMT_U8) {
	    WOutDev[0].caps.dwFormats |= WAVE_FORMAT_4M08;
	    if (WOutDev[0].caps.wChannels > 1)
		WOutDev[0].caps.dwFormats |= WAVE_FORMAT_4S08;
	}
	if ((mask & AFMT_S16_LE) && bytespersmpl > 1) {
	    WOutDev[0].caps.dwFormats |= WAVE_FORMAT_4M16;
	    if (WOutDev[0].caps.wChannels > 1)
		WOutDev[0].caps.dwFormats |= WAVE_FORMAT_4S16;
	}
    }
    smplrate = 22050;
    if (IOCTL(audio, SNDCTL_DSP_SPEED, smplrate) == 0) {
	if (mask & AFMT_U8) {
	    WOutDev[0].caps.dwFormats |= WAVE_FORMAT_2M08;
	    if (WOutDev[0].caps.wChannels > 1)
		WOutDev[0].caps.dwFormats |= WAVE_FORMAT_2S08;
	}
	if ((mask & AFMT_S16_LE) && bytespersmpl > 1) {
	    WOutDev[0].caps.dwFormats |= WAVE_FORMAT_2M16;
	    if (WOutDev[0].caps.wChannels > 1)
		WOutDev[0].caps.dwFormats |= WAVE_FORMAT_2S16;
	}
    }
    smplrate = 11025;
    if (IOCTL(audio, SNDCTL_DSP_SPEED, smplrate) == 0) {
	if (mask & AFMT_U8) {
	    WOutDev[0].caps.dwFormats |= WAVE_FORMAT_1M08;
	    if (WOutDev[0].caps.wChannels > 1)
		WOutDev[0].caps.dwFormats |= WAVE_FORMAT_1S08;
	}
	if ((mask & AFMT_S16_LE) && bytespersmpl > 1) {
	    WOutDev[0].caps.dwFormats |= WAVE_FORMAT_1M16;
	    if (WOutDev[0].caps.wChannels > 1)
		WOutDev[0].caps.dwFormats |= WAVE_FORMAT_1S16;
	}
    }
    if (IOCTL(audio, SNDCTL_DSP_GETCAPS, caps) == 0) {
	TRACE("OSS dsp out caps=%08X\n", caps);
	if ((caps & DSP_CAP_REALTIME) && !(caps & DSP_CAP_BATCH)) {
	    WOutDev[0].caps.dwSupport |= WAVECAPS_SAMPLEACCURATE;
	}
	/* well, might as well use the DirectSound cap flag for something */
	if ((caps & DSP_CAP_TRIGGER) && (caps & DSP_CAP_MMAP) &&
	    !(caps & DSP_CAP_BATCH))
	    WOutDev[0].caps.dwSupport |= WAVECAPS_DIRECTSOUND;
    }
    close(audio);
    TRACE("out dwFormats = %08lX, dwSupport = %08lX\n",
	  WOutDev[0].caps.dwFormats, WOutDev[0].caps.dwSupport);

    /* then do input device */
    samplesize = 16;
    dsp_stereo = 1;
   
	for (i = 0; i < MAX_WAVEINDRV; ++i)
	{
		WInDev[i].unixdev = -1;
	}

	memset(&WInDev[0].caps, 0, sizeof(WInDev[0].caps));

    if (access(SOUND_DEV,0) != 0 ||
	(audio = open(SOUND_DEV, O_RDONLY|O_NDELAY, 0)) == -1) {
	WARN("Couldn't open in %s (%s)\n", SOUND_DEV, strerror(errno));
	return -1;
    }

    ioctl(audio, SNDCTL_DSP_RESET, 0);

#ifdef EMULATE_SB16
    WInDev[0].caps.wMid = 0x0002;
    WInDev[0].caps.wPid = 0x0004;
    strcpy(WInDev[0].caps.szPname, "SB16 Wave In");
#else
    WInDev[0].caps.wMid = 0x00FF; 	/* Manufac ID */
    WInDev[0].caps.wPid = 0x0001; 	/* Product ID */
    strcpy(WInDev[0].caps.szPname, "OpenSoundSystem WAVIN Driver");
#endif
    WInDev[0].caps.dwFormats = 0x00000000;
    WInDev[0].caps.wChannels = (IOCTL(audio, SNDCTL_DSP_STEREO, dsp_stereo) != 0) ? 1 : 2;
    
    WInDev[0].bTriggerSupport = FALSE;
    if (IOCTL(audio, SNDCTL_DSP_GETCAPS, caps) == 0) {
	TRACE("OSS dsp in caps=%08X\n", caps);
	if (caps & DSP_CAP_TRIGGER)
            WInDev[0].bTriggerSupport = TRUE;
    }

    IOCTL(audio, SNDCTL_DSP_GETFMTS, mask);
    TRACE("OSS in dsp mask=%08x\n", mask);

    bytespersmpl = (IOCTL(audio, SNDCTL_DSP_SAMPLESIZE, samplesize) != 0) ? 1 : 2;
    smplrate = 44100;
    if (IOCTL(audio, SNDCTL_DSP_SPEED, smplrate) == 0) {
	if (mask & AFMT_U8) {
	    WInDev[0].caps.dwFormats |= WAVE_FORMAT_4M08;
	    if (WInDev[0].caps.wChannels > 1)
		WInDev[0].caps.dwFormats |= WAVE_FORMAT_4S08;
	}
	if ((mask & AFMT_S16_LE) && bytespersmpl > 1) {
	    WInDev[0].caps.dwFormats |= WAVE_FORMAT_4M16;
	    if (WInDev[0].caps.wChannels > 1)
		WInDev[0].caps.dwFormats |= WAVE_FORMAT_4S16;
	}
    }
    smplrate = 22050;
    if (IOCTL(audio, SNDCTL_DSP_SPEED, smplrate) == 0) {
	if (mask & AFMT_U8) {
	    WInDev[0].caps.dwFormats |= WAVE_FORMAT_2M08;
	    if (WInDev[0].caps.wChannels > 1)
		WInDev[0].caps.dwFormats |= WAVE_FORMAT_2S08;
	}
	if ((mask & AFMT_S16_LE) && bytespersmpl > 1) {
	    WInDev[0].caps.dwFormats |= WAVE_FORMAT_2M16;
	    if (WInDev[0].caps.wChannels > 1)
		WInDev[0].caps.dwFormats |= WAVE_FORMAT_2S16;
	}
    }
    smplrate = 11025;
    if (IOCTL(audio, SNDCTL_DSP_SPEED, smplrate) == 0) {
	if (mask & AFMT_U8) {
	    WInDev[0].caps.dwFormats |= WAVE_FORMAT_1M08;
	    if (WInDev[0].caps.wChannels > 1)
		WInDev[0].caps.dwFormats |= WAVE_FORMAT_1S08;
	}
	if ((mask & AFMT_S16_LE) && bytespersmpl > 1) {
	    WInDev[0].caps.dwFormats |= WAVE_FORMAT_1M16;
	    if (WInDev[0].caps.wChannels > 1)
		WInDev[0].caps.dwFormats |= WAVE_FORMAT_1S16;
	}
    }
    close(audio);
    TRACE("in dwFormats = %08lX\n", WInDev[0].caps.dwFormats);

    return 0;
}

/******************************************************************
 *		OSS_InitRingMessage
 *
 * Initialize the ring of messages for passing between driver's caller and playback/record
 * thread
 */
static int OSS_InitRingMessage(OSS_MSG_RING* omr)
{
    omr->msg_toget = 0;
    omr->msg_tosave = 0;
    omr->msg_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    memset(omr->messages, 0, sizeof(OSS_MSG) * OSS_RING_BUFFER_SIZE);
    InitializeCriticalSection(&omr->msg_crst);
    return 0;
}

/******************************************************************
 *		OSS_AddRingMessage
 *
 * Inserts a new message into the ring (should be called from DriverProc derivated routines)
 */
static int OSS_AddRingMessage(OSS_MSG_RING* omr, enum win_wm_message msg, DWORD param, BOOL wait)
{
    HANDLE	hEvent;

    EnterCriticalSection(&omr->msg_crst);
    if ((omr->msg_tosave == omr->msg_toget) /* buffer overflow ? */
	&& (omr->messages[omr->msg_toget].msg))
    {
	ERR("buffer overflow !?\n");
        LeaveCriticalSection(&omr->msg_crst);
	return 0;
    }

    hEvent = wait ? CreateEventA(NULL, FALSE, FALSE, NULL) : INVALID_HANDLE_VALUE;

    omr->messages[omr->msg_tosave].msg = msg;
    omr->messages[omr->msg_tosave].param = param;
    omr->messages[omr->msg_tosave].hEvent = hEvent;

    omr->msg_tosave++;
    if (omr->msg_tosave > OSS_RING_BUFFER_SIZE-1)
	omr->msg_tosave = 0;
    LeaveCriticalSection(&omr->msg_crst);
    /* signal a new message */
    SetEvent(omr->msg_event);
    if (wait)
    {
	    WaitForSingleObject(hEvent, INFINITE);
	    CloseHandle(hEvent);
    }
    return 1;
}

/******************************************************************
 *		OSS_RetrieveRingMessage
 *
 * Get a message from the ring. Should be called by the playback/record thread.
 */
static int OSS_RetrieveRingMessage(OSS_MSG_RING* omr, 
                                   enum win_wm_message *msg, DWORD *param, HANDLE *hEvent)
{
    EnterCriticalSection(&omr->msg_crst);

    if (omr->msg_toget == omr->msg_tosave) /* buffer empty ? */
    {
        LeaveCriticalSection(&omr->msg_crst);
	return 0;
    }
	
    *msg = omr->messages[omr->msg_toget].msg;
    omr->messages[omr->msg_toget].msg = 0;
    *param = omr->messages[omr->msg_toget].param;
    *hEvent = omr->messages[omr->msg_toget].hEvent;
    omr->msg_toget++;
    if (omr->msg_toget > OSS_RING_BUFFER_SIZE-1)
	omr->msg_toget = 0;
    LeaveCriticalSection(&omr->msg_crst);
    return 1;
}

/*======================================================================*
 *                  Low level WAVE OUT implementation			*
 *======================================================================*/

/**************************************************************************
 * 			wodNotifyClient			[internal]
 */
static DWORD wodNotifyClient(WINE_WAVEOUT* wwo, WORD wMsg, DWORD dwParam1, DWORD dwParam2)
{
    TRACE("wMsg = 0x%04x dwParm1 = %04lX dwParam2 = %04lX\n", wMsg, dwParam1, dwParam2);
    
    switch (wMsg) {
    case WOM_OPEN:
    case WOM_CLOSE:
    case WOM_DONE:
	if (wwo->wFlags != DCB_NULL && 
	    !DriverCallback(wwo->waveDesc.dwCallback, wwo->wFlags, wwo->waveDesc.hWave, 
			    wMsg, wwo->waveDesc.dwInstance, dwParam1, dwParam2)) {
	    WARN("can't notify client !\n");
	    return MMSYSERR_ERROR;
	}
	break;
    default:
	FIXME("Unknown CB message %u\n", wMsg);
        return MMSYSERR_INVALPARAM;
    }
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				wodUpdatePlayedTotal	[internal]
 *
 */
static void wodUpdatePlayedTotal(WINE_WAVEOUT* wwo, DWORD availInQ)
{
    wwo->dwPlayedTotal = wwo->dwWrittenTotal - (wwo->dwBufferSize - availInQ);
}

/**************************************************************************
 * 				wodPlayer_BeginWaveHdr          [internal]
 *
 * Makes the specified lpWaveHdr the currently playing wave header.
 * If the specified wave header is a begin loop and we're not already in
 * a loop, setup the loop.
 */
static void wodPlayer_BeginWaveHdr(WINE_WAVEOUT* wwo, LPWAVEHDR lpWaveHdr)
{
    wwo->lpPlayPtr = lpWaveHdr;

    if (!lpWaveHdr) return;

    if (lpWaveHdr->dwFlags & WHDR_BEGINLOOP) {
	if (wwo->lpLoopPtr) {
	    WARN("Already in a loop. Discarding loop on this header (%p)\n", lpWaveHdr);
	} else {
	    TRACE("Starting loop (%ldx) with %p\n", lpWaveHdr->dwLoops, lpWaveHdr);
	    wwo->lpLoopPtr = lpWaveHdr;
	    /* Windows does not touch WAVEHDR.dwLoops,
	     * so we need to make an internal copy */
	    wwo->dwLoops = lpWaveHdr->dwLoops;
	}
    }
    wwo->dwPartialOffset = 0;
}

/**************************************************************************
 * 				wodPlayer_PlayPtrNext	        [internal]
 *
 * Advance the play pointer to the next waveheader, looping if required.
 */
static LPWAVEHDR wodPlayer_PlayPtrNext(WINE_WAVEOUT* wwo)
{
    LPWAVEHDR lpWaveHdr = wwo->lpPlayPtr;

    wwo->dwPartialOffset = 0;
    if ((lpWaveHdr->dwFlags & WHDR_ENDLOOP) && wwo->lpLoopPtr) {
	/* We're at the end of a loop, loop if required */
	if (--wwo->dwLoops > 0) {
	    wwo->lpPlayPtr = wwo->lpLoopPtr;
	} else {
	    /* Handle overlapping loops correctly */
	    if (wwo->lpLoopPtr != lpWaveHdr && (lpWaveHdr->dwFlags & WHDR_BEGINLOOP)) {
		FIXME("Correctly handled case ? (ending loop buffer also starts a new loop)\n");
		/* shall we consider the END flag for the closing loop or for
		 * the opening one or for both ???
		 * code assumes for closing loop only
		 */
	    } else {
                lpWaveHdr = lpWaveHdr->lpNext;
            }
            wwo->lpLoopPtr = NULL;
            wodPlayer_BeginWaveHdr(wwo, lpWaveHdr);
	}
    } else {
	/* We're not in a loop.  Advance to the next wave header */
	wodPlayer_BeginWaveHdr(wwo, lpWaveHdr = lpWaveHdr->lpNext);
    }

    return lpWaveHdr;
}

/**************************************************************************
 * 			     wodPlayer_DSPWait			[internal]
 * Returns the number of milliseconds to wait for the DSP buffer to clear.
 * This is based on the number of fragments we want to be clear before
 * writing and the number of free fragments we already have.
 */
static DWORD wodPlayer_DSPWait(const WINE_WAVEOUT *wwo, DWORD availInQ)
{
    DWORD       uFreeFragments = availInQ / wwo->dwFragmentSize;

    return (uFreeFragments >= wwo->uWaitForFragments) 
        ? 1 : (wwo->dwFragmentSize * 1000 / wwo->format.wf.nAvgBytesPerSec) * 
              (wwo->uWaitForFragments - uFreeFragments);
}

/**************************************************************************
 * 			     wodPlayer_NotifyWait               [internal]
 * Returns the number of milliseconds to wait before attempting to notify
 * completion of the specified wavehdr.
 * This is based on the number of bytes remaining to be written in the
 * wave.
 */
static DWORD wodPlayer_NotifyWait(const WINE_WAVEOUT* wwo, LPWAVEHDR lpWaveHdr)
{
    DWORD dwMillis;

    if (lpWaveHdr->reserved < wwo->dwPlayedTotal) {
	dwMillis = 1;
    } else {
	dwMillis = (lpWaveHdr->reserved - wwo->dwPlayedTotal) * 1000 / wwo->format.wf.nAvgBytesPerSec;
	if (!dwMillis) dwMillis = 1;
    }

    return dwMillis;
}


/**************************************************************************
 * 			     wodPlayer_WriteMaxFrags            [internal]
 * Writes the maximum number of bytes possible to the DSP and returns
 * the number of bytes written.
 */
static int wodPlayer_WriteMaxFrags(WINE_WAVEOUT* wwo, DWORD* bytes)
{
    /* Only attempt to write to free bytes */
    DWORD dwLength = wwo->lpPlayPtr->dwBufferLength - wwo->dwPartialOffset;
    int toWrite = min(dwLength, *bytes);
    int written;

    TRACE("Writing wavehdr %p.%lu[%lu]\n", 
          wwo->lpPlayPtr, wwo->dwPartialOffset, wwo->lpPlayPtr->dwBufferLength);
    written = write(wwo->unixdev, wwo->lpPlayPtr->lpData + wwo->dwPartialOffset, toWrite);
    if (written <= 0) return written;

    if (written >= dwLength) {
        /* If we wrote all current wavehdr, skip to the next one */
        wodPlayer_PlayPtrNext(wwo);
    } else {
        /* Remove the amount written */
        wwo->dwPartialOffset += written;
    }
    *bytes -= written;
    wwo->dwWrittenTotal += written;

    return written;
}


/**************************************************************************
 * 				wodPlayer_NotifyCompletions	[internal]
 *
 * Notifies and remove from queue all wavehdrs which have been played to
 * the speaker (ie. they have cleared the OSS buffer).  If force is true,
 * we notify all wavehdrs and remove them all from the queue even if they
 * are unplayed or part of a loop.
 */
static DWORD wodPlayer_NotifyCompletions(WINE_WAVEOUT* wwo, BOOL force)
{
    LPWAVEHDR		lpWaveHdr;

    /* Start from lpQueuePtr and keep notifying until:
     * - we hit an unwritten wavehdr
     * - we hit the beginning of a running loop
     * - we hit a wavehdr which hasn't finished playing
     */
    while ((lpWaveHdr = wwo->lpQueuePtr) && 
           (force || 
            (lpWaveHdr != wwo->lpPlayPtr &&
             lpWaveHdr != wwo->lpLoopPtr &&
             lpWaveHdr->reserved <= wwo->dwPlayedTotal))) {

	wwo->lpQueuePtr = lpWaveHdr->lpNext;

	lpWaveHdr->dwFlags &= ~WHDR_INQUEUE;
	lpWaveHdr->dwFlags |= WHDR_DONE;

	wodNotifyClient(wwo, WOM_DONE, (DWORD)lpWaveHdr, 0);
    }
    return  (lpWaveHdr && lpWaveHdr != wwo->lpPlayPtr && lpWaveHdr != wwo->lpLoopPtr) ? 
        wodPlayer_NotifyWait(wwo, lpWaveHdr) : INFINITE;
}

/**************************************************************************
 * 				wodPlayer_Reset			[internal]
 *
 * wodPlayer helper. Resets current output stream.
 */
static	void	wodPlayer_Reset(WINE_WAVEOUT* wwo, BOOL reset)
{
    /* updates current notify list */
    wodPlayer_NotifyCompletions(wwo, FALSE);

    /* flush all possible output */
    if (ioctl(wwo->unixdev, SNDCTL_DSP_RESET, 0) == -1) {
	perror("ioctl SNDCTL_DSP_RESET");
	wwo->hThread = 0;
	wwo->state = WINE_WS_STOPPED;
	ExitThread(-1);
    }

    /* Clear partial wavehdr */
    wwo->dwPartialOffset = 0;

    if (reset) {
	/* empty notify list */
	wodPlayer_NotifyCompletions(wwo, TRUE);

	wwo->lpPlayPtr = wwo->lpQueuePtr = wwo->lpLoopPtr = NULL;
	wwo->state = WINE_WS_STOPPED;
	wwo->dwPlayedTotal = 0;
        wwo->dwWrittenTotal = 0;
    } else {
	/* FIXME: this is not accurate when looping, but can we do better ? */
	wwo->lpPlayPtr = (wwo->lpLoopPtr) ? wwo->lpLoopPtr : wwo->lpQueuePtr;
	wwo->state = WINE_WS_PAUSED;
    }
}

/**************************************************************************
 * 		      wodPlayer_ProcessMessages			[internal]
 */
static void wodPlayer_ProcessMessages(WINE_WAVEOUT* wwo)
{
    LPWAVEHDR           lpWaveHdr;
    enum win_wm_message	msg;
    DWORD		param;
    HANDLE		ev;

    while (OSS_RetrieveRingMessage(&wwo->msgRing, &msg, &param, &ev)) {
	TRACE("Received %s %lx\n", wodPlayerCmdString[msg - WM_USER - 1], param);
	switch (msg) {
	case WINE_WM_PAUSING:
	    wodPlayer_Reset(wwo, FALSE);
	    wwo->state = WINE_WS_PAUSED;
	    SetEvent(ev);
	    break;
	case WINE_WM_RESTARTING:
	    wwo->state = WINE_WS_PLAYING;
	    SetEvent(ev);
	    break;
	case WINE_WM_HEADER:
	    lpWaveHdr = (LPWAVEHDR)param;
	    
	    /* insert buffer at the end of queue */
	    {
		LPWAVEHDR*	wh;
		for (wh = &(wwo->lpQueuePtr); *wh; wh = &((*wh)->lpNext));
		*wh = lpWaveHdr;
	    }
            if (!wwo->lpPlayPtr)
                wodPlayer_BeginWaveHdr(wwo,lpWaveHdr);
	    if (wwo->state == WINE_WS_STOPPED)
		wwo->state = WINE_WS_PLAYING;
	    break;
	case WINE_WM_RESETTING:
	    wodPlayer_Reset(wwo, TRUE);
	    SetEvent(ev);
	    break;
        case WINE_WM_UPDATE:
            {
                audio_buf_info      info;

                if (ioctl(wwo->unixdev, SNDCTL_DSP_GETOSPACE, &info) < 0) {
                    ERR("ioctl failed (%s)\n", strerror(errno));
                } else {
                    wodUpdatePlayedTotal(wwo, info.bytes);
                }
            }
	    SetEvent(ev);
            break;
        case WINE_WM_BREAKLOOP:
            if (wwo->state == WINE_WS_PLAYING && wwo->lpLoopPtr != NULL) {
                /* ensure exit at end of current loop */
                wwo->dwLoops = 1;
            }
	    SetEvent(ev);
            break;
	case WINE_WM_CLOSING:
	    /* sanity check: this should not happen since the device must have been reset before */
	    if (wwo->lpQueuePtr || wwo->lpPlayPtr) ERR("out of sync\n");
	    wwo->hThread = 0;
	    wwo->state = WINE_WS_CLOSED;
	    SetEvent(ev);
	    ExitThread(0);
	    /* shouldn't go here */
	default:
	    FIXME("unknown message %d\n", msg);
	    break;
	}
    }
}

/**************************************************************************
 * 			     wodPlayer_FeedDSP			[internal]
 * Feed as much sound data as we can into the DSP and return the number of
 * milliseconds before it will be necessary to feed the DSP again.
 */
static DWORD wodPlayer_FeedDSP(WINE_WAVEOUT* wwo)
{
    audio_buf_info dspspace;
    DWORD       availInQ;

    if (ioctl(wwo->unixdev, SNDCTL_DSP_GETOSPACE, &dspspace) < 0) {
        ERR("IOCTL can't 'SNDCTL_DSP_GETOSPACE' !\n");
        return INFINITE;
    }
    TRACE("fragments=%d/%d, fragsize=%d, bytes=%d\n",
          dspspace.fragments, dspspace.fragstotal, dspspace.fragsize, dspspace.bytes);

    availInQ = dspspace.bytes;
    wodUpdatePlayedTotal(wwo, availInQ);

    /* input queue empty and output buffer with less than one fragment to play */
    if (!wwo->lpPlayPtr && wwo->dwBufferSize < availInQ + 2 * wwo->dwFragmentSize) {
        TRACE("Run out of wavehdr:s... flushing\n");
        ioctl(wwo->unixdev, SNDCTL_DSP_SYNC, 0);
        wodUpdatePlayedTotal(wwo, wwo->dwBufferSize);
        return INFINITE;
    }

    /* no more room... no need to try to feed */
    if (dspspace.fragments == 0) return wodPlayer_DSPWait(wwo, 0);

    /* Feed from partial wavehdr */
    if (wwo->lpPlayPtr && wwo->dwPartialOffset != 0) {
        wodPlayer_WriteMaxFrags(wwo, &availInQ);
    }

    /* Feed wavehdrs until we run out of wavehdrs or DSP space */
    if (wwo->dwPartialOffset == 0) while (wwo->lpPlayPtr && availInQ > 0) {
	/* note the value that dwPlayedTotal will return when this wave finishes playing */
	wwo->lpPlayPtr->reserved = wwo->dwWrittenTotal + wwo->lpPlayPtr->dwBufferLength;
	wodPlayer_WriteMaxFrags(wwo, &availInQ);
    }

    return wodPlayer_DSPWait(wwo, availInQ);
}


/**************************************************************************
 * 				wodPlayer			[internal]
 */
static	DWORD	CALLBACK	wodPlayer(LPVOID pmt)
{
    WORD	  uDevID = (DWORD)pmt;
    WINE_WAVEOUT* wwo = (WINE_WAVEOUT*)&WOutDev[uDevID];
    DWORD         dwNextFeedTime = INFINITE;   /* Time before DSP needs feeding */
    DWORD         dwNextNotifyTime = INFINITE; /* Time before next wave completion */
    DWORD         dwSleepTime;

    wwo->state = WINE_WS_STOPPED;
    SetEvent(wwo->hStartUpEvent);

    for (;;) {
        /** Wait for the shortest time before an action is required.  If there
         *  are no pending actions, wait forever for a command.
         */
        dwSleepTime = min(dwNextFeedTime, dwNextNotifyTime);
        TRACE("waiting %lums (%lu,%lu)\n", dwSleepTime, dwNextFeedTime, dwNextNotifyTime);
        WaitForSingleObject(wwo->msgRing.msg_event, dwSleepTime);
	wodPlayer_ProcessMessages(wwo);
	if (wwo->state == WINE_WS_PLAYING) {
	    dwNextFeedTime = wodPlayer_FeedDSP(wwo);
	    dwNextNotifyTime = wodPlayer_NotifyCompletions(wwo, FALSE);
	} else {
	    dwNextFeedTime = dwNextNotifyTime = INFINITE;
	}
    }
}

/**************************************************************************
 * 			wodGetDevCaps				[internal]
 */
static DWORD wodGetDevCaps(WORD wDevID, LPWAVEOUTCAPSA lpCaps, DWORD dwSize)
{
    TRACE("(%u, %p, %lu);\n", wDevID, lpCaps, dwSize);
    
    if (lpCaps == NULL) return MMSYSERR_NOTENABLED;
    
    if (wDevID >= MAX_WAVEOUTDRV) {
	TRACE("MAX_WAVOUTDRV reached !\n");
	return MMSYSERR_BADDEVICEID;
    }

    memcpy(lpCaps, &WOutDev[wDevID].caps, min(dwSize, sizeof(*lpCaps)));
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				wodOpen				[internal]
 */
static DWORD wodOpen(WORD wDevID, LPWAVEOPENDESC lpDesc, DWORD dwFlags)
{
    int 	 	audio;
    int			format;
    int			sample_rate;
    int			dsp_stereo;
    int			audio_fragment;
    WINE_WAVEOUT*	wwo;
    audio_buf_info      info;

    TRACE("(%u, %p, %08lX);\n", wDevID, lpDesc, dwFlags);
    if (lpDesc == NULL) {
	WARN("Invalid Parameter !\n");
	return MMSYSERR_INVALPARAM;
    }
    if (wDevID >= MAX_WAVEOUTDRV) {
	TRACE("MAX_WAVOUTDRV reached !\n");
	return MMSYSERR_BADDEVICEID;
    }

    /* only PCM format is supported so far... */
    if (lpDesc->lpFormat->wFormatTag != WAVE_FORMAT_PCM ||
	lpDesc->lpFormat->nChannels == 0 ||
	lpDesc->lpFormat->nSamplesPerSec == 0) {
	WARN("Bad format: tag=%04X nChannels=%d nSamplesPerSec=%ld !\n", 
	     lpDesc->lpFormat->wFormatTag, lpDesc->lpFormat->nChannels,
	     lpDesc->lpFormat->nSamplesPerSec);
	return WAVERR_BADFORMAT;
    }

    if (dwFlags & WAVE_FORMAT_QUERY) {
	TRACE("Query format: tag=%04X nChannels=%d nSamplesPerSec=%ld !\n", 
	     lpDesc->lpFormat->wFormatTag, lpDesc->lpFormat->nChannels,
	     lpDesc->lpFormat->nSamplesPerSec);
	return MMSYSERR_NOERROR;
    }

    wwo = &WOutDev[wDevID];

    if ((dwFlags & WAVE_DIRECTSOUND) && !(wwo->caps.dwSupport & WAVECAPS_DIRECTSOUND))
	/* not supported, ignore it */
	dwFlags &= ~WAVE_DIRECTSOUND;

    if (access(SOUND_DEV, 0) != 0)
	return MMSYSERR_NOTENABLED;
    if (dwFlags & WAVE_DIRECTSOUND)
	/* we want to be able to mmap() the device, which means it must be opened readable,
	 * otherwise mmap() will fail (at least under Linux) */
	audio = open(SOUND_DEV, O_RDWR|O_NDELAY, 0);
    else
	audio = open(SOUND_DEV, O_WRONLY|O_NDELAY, 0);
    if (audio == -1) {
	WARN("can't open sound device %s (%s)!\n", SOUND_DEV, strerror(errno));
	return MMSYSERR_ALLOCATED;
    }
    fcntl(audio, F_SETFD, 1); /* set close on exec flag */
    wwo->unixdev = audio;
    wwo->wFlags = HIWORD(dwFlags & CALLBACK_TYPEMASK);
    
    memcpy(&wwo->waveDesc, lpDesc, 	     sizeof(WAVEOPENDESC));
    memcpy(&wwo->format,   lpDesc->lpFormat, sizeof(PCMWAVEFORMAT));
    
    if (wwo->format.wBitsPerSample == 0) {
	WARN("Resetting zeroed wBitsPerSample\n");
	wwo->format.wBitsPerSample = 8 *
	    (wwo->format.wf.nAvgBytesPerSec /
	     wwo->format.wf.nSamplesPerSec) /
	    wwo->format.wf.nChannels;
    }
    
    if (dwFlags & WAVE_DIRECTSOUND) {
        if (wwo->caps.dwSupport & WAVECAPS_SAMPLEACCURATE)
	    /* we have realtime DirectSound, fragments just waste our time,
	     * but a large buffer is good, so choose 64KB (32 * 2^11) */
	    audio_fragment = 0x0020000B;
	else
	    /* to approximate realtime, we must use small fragments,
	     * let's try to fragment the above 64KB (256 * 2^8) */
	    audio_fragment = 0x01000008;
    } else {
	/* shockwave player uses only 4 1k-fragments at a rate of 22050 bytes/sec
	 * thus leading to 46ms per fragment, and a turnaround time of 185ms
	 */
	/* 16 fragments max, 2^10=1024 bytes per fragment */
	audio_fragment = 0x000F000A;
    }
    sample_rate = wwo->format.wf.nSamplesPerSec;
    dsp_stereo = (wwo->format.wf.nChannels > 1) ? 1 : 0;
    format = (wwo->format.wBitsPerSample == 16) ? AFMT_S16_LE : AFMT_U8;

    IOCTL(audio, SNDCTL_DSP_SETFRAGMENT, audio_fragment);
    /* First size and stereo then samplerate */
    IOCTL(audio, SNDCTL_DSP_SETFMT, format);
    IOCTL(audio, SNDCTL_DSP_STEREO, dsp_stereo);
    IOCTL(audio, SNDCTL_DSP_SPEED, sample_rate);

    /* paranoid checks */
    if (format != ((wwo->format.wBitsPerSample == 16) ? AFMT_S16_LE : AFMT_U8))
	ERR("Can't set format to %d (%d)\n", 
	    (wwo->format.wBitsPerSample == 16) ? AFMT_S16_LE : AFMT_U8, format);
    if (dsp_stereo != (wwo->format.wf.nChannels > 1) ? 1 : 0) 
	ERR("Can't set stereo to %u (%d)\n", 
	    (wwo->format.wf.nChannels > 1) ? 1 : 0, dsp_stereo);
    if (!NEAR_MATCH(sample_rate, wwo->format.wf.nSamplesPerSec))
	ERR("Can't set sample_rate to %lu (%d)\n", 
	    wwo->format.wf.nSamplesPerSec, sample_rate);

    /* Read output space info for future reference */
    if (ioctl(wwo->unixdev, SNDCTL_DSP_GETOSPACE, &info) < 0) {
	ERR("IOCTL can't 'SNDCTL_DSP_GETOSPACE' !\n");
	close(audio);
	wwo->unixdev = -1;
	return MMSYSERR_NOTENABLED;
    }

    /* Check that fragsize is correct per our settings above */
    if ((info.fragsize > 1024) && (LOWORD(audio_fragment) <= 10)) {
	/* we've tried to set 1K fragments or less, but it didn't work */
	ERR("fragment size set failed, size is now %d\n", info.fragsize);
	MESSAGE("Your Open Sound System driver did not let us configure small enough sound fragments.\n");
	MESSAGE("This may cause delays and other problems in audio playback with certain applications.\n");
    }

    /* Remember fragsize and total buffer size for future use */
    wwo->dwFragmentSize = info.fragsize;
    wwo->dwBufferSize = info.fragstotal * info.fragsize;
    wwo->uWaitForFragments = info.fragstotal * REFILL_BUFFER_WHEN;
    wwo->dwPlayedTotal = 0;
    wwo->dwWrittenTotal = 0;
    TRACE("wait for %d fragments\n", wwo->uWaitForFragments);

    OSS_InitRingMessage(&wwo->msgRing);

    if (!(dwFlags & WAVE_DIRECTSOUND)) {
	wwo->hStartUpEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
	wwo->hThread = CreateThread(NULL, 0, wodPlayer, (LPVOID)(DWORD)wDevID, 0, &(wwo->dwThreadID));
	WaitForSingleObject(wwo->hStartUpEvent, INFINITE);
	CloseHandle(wwo->hStartUpEvent);
    } else {
	wwo->hThread = INVALID_HANDLE_VALUE;
	wwo->dwThreadID = 0;
    }
    wwo->hStartUpEvent = INVALID_HANDLE_VALUE;

    TRACE("fd=%d fragmentSize=%ld\n", 
	  wwo->unixdev, wwo->dwFragmentSize);
    if (wwo->dwFragmentSize % wwo->format.wf.nBlockAlign)
	ERR("Fragment doesn't contain an integral number of data blocks\n");

    TRACE("wBitsPerSample=%u, nAvgBytesPerSec=%lu, nSamplesPerSec=%lu, nChannels=%u nBlockAlign=%u!\n", 
	  wwo->format.wBitsPerSample, wwo->format.wf.nAvgBytesPerSec, 
	  wwo->format.wf.nSamplesPerSec, wwo->format.wf.nChannels,
	  wwo->format.wf.nBlockAlign);
    
    return wodNotifyClient(wwo, WOM_OPEN, 0L, 0L);
}

/**************************************************************************
 * 				wodClose			[internal]
 */
static DWORD wodClose(WORD wDevID)
{
    DWORD		ret = MMSYSERR_NOERROR;
    WINE_WAVEOUT*	wwo;

    TRACE("(%u);\n", wDevID);
    
    if (wDevID >= MAX_WAVEOUTDRV || WOutDev[wDevID].unixdev == -1) {
	WARN("bad device ID !\n");
	return MMSYSERR_BADDEVICEID;
    }
    
    wwo = &WOutDev[wDevID];
    if (wwo->lpQueuePtr) {
	WARN("buffers still playing !\n");
	ret = WAVERR_STILLPLAYING;
    } else {
	TRACE("imhere[3-close]\n");
	if (wwo->hThread != INVALID_HANDLE_VALUE) {
	    OSS_AddRingMessage(&wwo->msgRing, WINE_WM_CLOSING, 0, TRUE);
	}
	if (wwo->mapping) {
	    munmap(wwo->mapping, wwo->maplen);
	    wwo->mapping = NULL;
	}

	close(wwo->unixdev);
	wwo->unixdev = -1;
	wwo->dwFragmentSize = 0;
	ret = wodNotifyClient(wwo, WOM_CLOSE, 0L, 0L);
    }
    return ret;
}

/**************************************************************************
 * 				wodWrite			[internal]
 * 
 */
static DWORD wodWrite(WORD wDevID, LPWAVEHDR lpWaveHdr, DWORD dwSize)
{
    TRACE("(%u, %p, %08lX);\n", wDevID, lpWaveHdr, dwSize);
    
    /* first, do the sanity checks... */
    if (wDevID >= MAX_WAVEOUTDRV || WOutDev[wDevID].unixdev == -1) {
        WARN("bad dev ID !\n");
	return MMSYSERR_BADDEVICEID;
    }
    
    if (lpWaveHdr->lpData == NULL || !(lpWaveHdr->dwFlags & WHDR_PREPARED)) 
	return WAVERR_UNPREPARED;
    
    if (lpWaveHdr->dwFlags & WHDR_INQUEUE) 
	return WAVERR_STILLPLAYING;

    lpWaveHdr->dwFlags &= ~WHDR_DONE;
    lpWaveHdr->dwFlags |= WHDR_INQUEUE;
    lpWaveHdr->lpNext = 0;

    TRACE("imhere[3-HEADER]\n");
    OSS_AddRingMessage(&WOutDev[wDevID].msgRing, WINE_WM_HEADER, (DWORD)lpWaveHdr, FALSE);

    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				wodPrepare			[internal]
 */
static DWORD wodPrepare(WORD wDevID, LPWAVEHDR lpWaveHdr, DWORD dwSize)
{
    TRACE("(%u, %p, %08lX);\n", wDevID, lpWaveHdr, dwSize);
    
    if (wDevID >= MAX_WAVEOUTDRV) {
	WARN("bad device ID !\n");
	return MMSYSERR_BADDEVICEID;
    }
    
    if (lpWaveHdr->dwFlags & WHDR_INQUEUE)
	return WAVERR_STILLPLAYING;
    
    lpWaveHdr->dwFlags |= WHDR_PREPARED;
    lpWaveHdr->dwFlags &= ~WHDR_DONE;
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				wodUnprepare			[internal]
 */
static DWORD wodUnprepare(WORD wDevID, LPWAVEHDR lpWaveHdr, DWORD dwSize)
{
    TRACE("(%u, %p, %08lX);\n", wDevID, lpWaveHdr, dwSize);
    
    if (wDevID >= MAX_WAVEOUTDRV) {
	WARN("bad device ID !\n");
	return MMSYSERR_BADDEVICEID;
    }
    
    if (lpWaveHdr->dwFlags & WHDR_INQUEUE)
	return WAVERR_STILLPLAYING;
    
    lpWaveHdr->dwFlags &= ~WHDR_PREPARED;
    lpWaveHdr->dwFlags |= WHDR_DONE;
    
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 			wodPause				[internal]
 */
static DWORD wodPause(WORD wDevID)
{
    TRACE("(%u);!\n", wDevID);
    
    if (wDevID >= MAX_WAVEOUTDRV || WOutDev[wDevID].unixdev == -1) {
	WARN("bad device ID !\n");
	return MMSYSERR_BADDEVICEID;
    }
    
    TRACE("imhere[3-PAUSING]\n");
    OSS_AddRingMessage(&WOutDev[wDevID].msgRing, WINE_WM_PAUSING, 0, TRUE);
    
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 			wodRestart				[internal]
 */
static DWORD wodRestart(WORD wDevID)
{
    TRACE("(%u);\n", wDevID);
    
    if (wDevID >= MAX_WAVEOUTDRV || WOutDev[wDevID].unixdev == -1) {
	WARN("bad device ID !\n");
	return MMSYSERR_BADDEVICEID;
    }
    
    if (WOutDev[wDevID].state == WINE_WS_PAUSED) {
	TRACE("imhere[3-RESTARTING]\n");
	OSS_AddRingMessage(&WOutDev[wDevID].msgRing, WINE_WM_RESTARTING, 0, TRUE);
    }
    
    /* FIXME: is NotifyClient with WOM_DONE right ? (Comet Busters 1.3.3 needs this notification) */
    /* FIXME: Myst crashes with this ... hmm -MM
       return wodNotifyClient(wwo, WOM_DONE, 0L, 0L);
    */
    
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 			wodReset				[internal]
 */
static DWORD wodReset(WORD wDevID)
{
    TRACE("(%u);\n", wDevID);
    
    if (wDevID >= MAX_WAVEOUTDRV || WOutDev[wDevID].unixdev == -1) {
	WARN("bad device ID !\n");
	return MMSYSERR_BADDEVICEID;
    }
    
    TRACE("imhere[3-RESET]\n");
    OSS_AddRingMessage(&WOutDev[wDevID].msgRing, WINE_WM_RESETTING, 0, TRUE);
    
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				wodGetPosition			[internal]
 */
static DWORD wodGetPosition(WORD wDevID, LPMMTIME lpTime, DWORD uSize)
{
    int			time;
    DWORD		val;
    WINE_WAVEOUT*	wwo;

    TRACE("(%u, %p, %lu);\n", wDevID, lpTime, uSize);
    
    if (wDevID >= MAX_WAVEOUTDRV || WOutDev[wDevID].unixdev == -1) {
	WARN("bad device ID !\n");
	return MMSYSERR_BADDEVICEID;
    }
    
    if (lpTime == NULL)	return MMSYSERR_INVALPARAM;

    wwo = &WOutDev[wDevID];
    OSS_AddRingMessage(&wwo->msgRing, WINE_WM_UPDATE, 0, TRUE);
    val = wwo->dwPlayedTotal;

    TRACE("wType=%04X wBitsPerSample=%u nSamplesPerSec=%lu nChannels=%u nAvgBytesPerSec=%lu\n", 
	  lpTime->wType, wwo->format.wBitsPerSample, 
	  wwo->format.wf.nSamplesPerSec, wwo->format.wf.nChannels, 
	  wwo->format.wf.nAvgBytesPerSec); 
    TRACE("dwPlayedTotal=%lu\n", val);
    
    switch (lpTime->wType) {
    case TIME_BYTES:
	lpTime->u.cb = val;
	TRACE("TIME_BYTES=%lu\n", lpTime->u.cb);
	break;
    case TIME_SAMPLES:
	lpTime->u.sample = val * 8 / wwo->format.wBitsPerSample /wwo->format.wf.nChannels;
	TRACE("TIME_SAMPLES=%lu\n", lpTime->u.sample);
	break;
    case TIME_SMPTE:
	time = val / (wwo->format.wf.nAvgBytesPerSec / 1000);
	lpTime->u.smpte.hour = time / 108000;
	time -= lpTime->u.smpte.hour * 108000;
	lpTime->u.smpte.min = time / 1800;
	time -= lpTime->u.smpte.min * 1800;
	lpTime->u.smpte.sec = time / 30;
	time -= lpTime->u.smpte.sec * 30;
	lpTime->u.smpte.frame = time;
	lpTime->u.smpte.fps = 30;
	TRACE("TIME_SMPTE=%02u:%02u:%02u:%02u\n",
	      lpTime->u.smpte.hour, lpTime->u.smpte.min,
	      lpTime->u.smpte.sec, lpTime->u.smpte.frame);
	break;
    default:
	FIXME("Format %d not supported ! use TIME_MS !\n", lpTime->wType);
	lpTime->wType = TIME_MS;
    case TIME_MS:
	lpTime->u.ms = val / (wwo->format.wf.nAvgBytesPerSec / 1000);
	TRACE("TIME_MS=%lu\n", lpTime->u.ms);
	break;
    }
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				wodBreakLoop			[internal]
 */
static DWORD wodBreakLoop(WORD wDevID)
{
    TRACE("(%u);\n", wDevID);
    
    if (wDevID >= MAX_WAVEOUTDRV || WOutDev[wDevID].unixdev == -1) {
	WARN("bad device ID !\n");
	return MMSYSERR_BADDEVICEID;
    }
    OSS_AddRingMessage(&WOutDev[wDevID].msgRing, WINE_WM_BREAKLOOP, 0, TRUE);
    return MMSYSERR_NOERROR;
}
    
/**************************************************************************
 * 				wodGetVolume			[internal]
 */
static DWORD wodGetVolume(WORD wDevID, LPDWORD lpdwVol)
{
    int 	mixer;
    int		volume;
    DWORD	left, right;
    
    TRACE("(%u, %p);\n", wDevID, lpdwVol);
    
    if (lpdwVol == NULL) 
	return MMSYSERR_NOTENABLED;
    if ((mixer = open(MIXER_DEV, O_RDONLY|O_NDELAY)) < 0) {
	WARN("mixer device not available !\n");
	return MMSYSERR_NOTENABLED;
    }
    if (ioctl(mixer, SOUND_MIXER_READ_PCM, &volume) == -1) {
	WARN("unable to read mixer !\n");
	return MMSYSERR_NOTENABLED;
    }
    close(mixer);
    left = LOBYTE(volume);
    right = HIBYTE(volume);
    TRACE("left=%ld right=%ld !\n", left, right);
    *lpdwVol = ((left * 0xFFFFl) / 100) + (((right * 0xFFFFl) / 100) << 16);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				wodSetVolume			[internal]
 */
static DWORD wodSetVolume(WORD wDevID, DWORD dwParam)
{
    int 	mixer;
    int		volume;
    DWORD	left, right;

    TRACE("(%u, %08lX);\n", wDevID, dwParam);

    left  = (LOWORD(dwParam) * 100) / 0xFFFFl;
    right = (HIWORD(dwParam) * 100) / 0xFFFFl;
    volume = left + (right << 8);
    
    if ((mixer = open(MIXER_DEV, O_WRONLY|O_NDELAY)) < 0) {
	WARN("mixer device not available !\n");
	return MMSYSERR_NOTENABLED;
    }
    if (ioctl(mixer, SOUND_MIXER_WRITE_PCM, &volume) == -1) {
	WARN("unable to set mixer !\n");
	return MMSYSERR_NOTENABLED;
    } else {
	TRACE("volume=%04x\n", (unsigned)volume);
    }
    close(mixer);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				wodGetNumDevs			[internal]
 */
static	DWORD	wodGetNumDevs(void)
{
    DWORD	ret = 1;
    
    /* FIXME: For now, only one sound device (SOUND_DEV) is allowed */
    int audio = open(SOUND_DEV, O_WRONLY|O_NDELAY, 0);
    
    if (audio == -1) {
	if (errno != EBUSY)
	    ret = 0;
    } else {
	close(audio);
    }
    return ret;
}

/**************************************************************************
 * 				wodMessage (WINEOSS.7)
 */
DWORD WINAPI OSS_wodMessage(UINT wDevID, UINT wMsg, DWORD dwUser, 
			    DWORD dwParam1, DWORD dwParam2)
{
    TRACE("(%u, %04X, %08lX, %08lX, %08lX);\n",
	  wDevID, wMsg, dwUser, dwParam1, dwParam2);
    
    switch (wMsg) {
    case DRVM_INIT:
    case DRVM_EXIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
	/* FIXME: Pretend this is supported */
	return 0;
    case WODM_OPEN:	 	return wodOpen		(wDevID, (LPWAVEOPENDESC)dwParam1,	dwParam2);
    case WODM_CLOSE:	 	return wodClose		(wDevID);
    case WODM_WRITE:	 	return wodWrite		(wDevID, (LPWAVEHDR)dwParam1,		dwParam2);
    case WODM_PAUSE:	 	return wodPause		(wDevID);
    case WODM_GETPOS:	 	return wodGetPosition	(wDevID, (LPMMTIME)dwParam1, 		dwParam2);
    case WODM_BREAKLOOP: 	return wodBreakLoop     (wDevID);
    case WODM_PREPARE:	 	return wodPrepare	(wDevID, (LPWAVEHDR)dwParam1, 		dwParam2);
    case WODM_UNPREPARE: 	return wodUnprepare	(wDevID, (LPWAVEHDR)dwParam1, 		dwParam2);
    case WODM_GETDEVCAPS:	return wodGetDevCaps	(wDevID, (LPWAVEOUTCAPSA)dwParam1,	dwParam2);
    case WODM_GETNUMDEVS:	return wodGetNumDevs	();
    case WODM_GETPITCH:	 	return MMSYSERR_NOTSUPPORTED;
    case WODM_SETPITCH:	 	return MMSYSERR_NOTSUPPORTED;
    case WODM_GETPLAYBACKRATE:	return MMSYSERR_NOTSUPPORTED;
    case WODM_SETPLAYBACKRATE:	return MMSYSERR_NOTSUPPORTED;
    case WODM_GETVOLUME:	return wodGetVolume	(wDevID, (LPDWORD)dwParam1);
    case WODM_SETVOLUME:	return wodSetVolume	(wDevID, dwParam1);
    case WODM_RESTART:		return wodRestart	(wDevID);
    case WODM_RESET:		return wodReset		(wDevID);

    case DRV_QUERYDSOUNDIFACE:	return wodDsCreate(wDevID, (PIDSDRIVER*)dwParam1);
    default:
	FIXME("unknown message %d!\n", wMsg);
    }
    return MMSYSERR_NOTSUPPORTED;
}

/*======================================================================*
 *                  Low level DSOUND implementation			*
 *======================================================================*/

typedef struct IDsDriverImpl IDsDriverImpl;
typedef struct IDsDriverBufferImpl IDsDriverBufferImpl;

struct IDsDriverImpl
{
    /* IUnknown fields */
    ICOM_VFIELD(IDsDriver);
    DWORD		ref;
    /* IDsDriverImpl fields */
    UINT		wDevID;
    IDsDriverBufferImpl*primary;
};

struct IDsDriverBufferImpl
{
    /* IUnknown fields */
    ICOM_VFIELD(IDsDriverBuffer);
    DWORD		ref;
    /* IDsDriverBufferImpl fields */
    IDsDriverImpl*	drv;
    DWORD		buflen;
};

static HRESULT DSDB_MapPrimary(IDsDriverBufferImpl *dsdb)
{
    WINE_WAVEOUT *wwo = &(WOutDev[dsdb->drv->wDevID]);
    if (!wwo->mapping) {
	wwo->mapping = mmap(NULL, wwo->maplen, PROT_WRITE, MAP_SHARED,
			    wwo->unixdev, 0);
	if (wwo->mapping == (LPBYTE)-1) {
	    ERR("(%p): Could not map sound device for direct access (errno=%d)\n", dsdb, errno);
	    return DSERR_GENERIC;
	}
	TRACE("(%p): sound device has been mapped for direct access at %p, size=%ld\n", dsdb, wwo->mapping, wwo->maplen);

	/* for some reason, es1371 and sblive! sometimes have junk in here.
	 * clear it, or we get junk noise */
	/* some libc implementations are buggy: their memset reads from the buffer...
	 * to work around it, we have to zero the block by hand. We don't do the expected:
	 * memset(wwo->mapping,0, wwo->maplen); 
	 */
	{
	    char*	p1 = wwo->mapping;
	    unsigned	len = wwo->maplen;

	    if (len >= 16) /* so we can have at least a 4 long area to store... */
	    {
		/* the mmap:ed value is (at least) dword aligned
		 * so, start filling the complete unsigned long:s 
		 */
		int		b = len >> 2;
		unsigned long*	p4 = (unsigned long*)p1;

		while (b--) *p4++ = 0;
		/* prepare for filling the rest */
		len &= 3;
		p1 = (unsigned char*)p4;
	    }
	    /* in all cases, fill the remaining bytes */
	    while (len-- != 0) *p1++ = 0;
	}
    }
    return DS_OK;
}

static HRESULT DSDB_UnmapPrimary(IDsDriverBufferImpl *dsdb)
{
    WINE_WAVEOUT *wwo = &(WOutDev[dsdb->drv->wDevID]);
    if (wwo->mapping) {
	if (munmap(wwo->mapping, wwo->maplen) < 0) {
	    ERR("(%p): Could not unmap sound device (errno=%d)\n", dsdb, errno);
	    return DSERR_GENERIC;
	}
	wwo->mapping = NULL;
	TRACE("(%p): sound device unmapped\n", dsdb);
    }
    return DS_OK;
}

static HRESULT WINAPI IDsDriverBufferImpl_QueryInterface(PIDSDRIVERBUFFER iface, REFIID riid, LPVOID *ppobj)
{
    /* ICOM_THIS(IDsDriverBufferImpl,iface); */
    FIXME("(): stub!\n");
    return DSERR_UNSUPPORTED;
}

static ULONG WINAPI IDsDriverBufferImpl_AddRef(PIDSDRIVERBUFFER iface)
{
    ICOM_THIS(IDsDriverBufferImpl,iface);
    This->ref++;
    return This->ref;
}

static ULONG WINAPI IDsDriverBufferImpl_Release(PIDSDRIVERBUFFER iface)
{
    ICOM_THIS(IDsDriverBufferImpl,iface);
    if (--This->ref)
	return This->ref;
    if (This == This->drv->primary)
	This->drv->primary = NULL;
    DSDB_UnmapPrimary(This);
    HeapFree(GetProcessHeap(),0,This);
    return 0;
}

static HRESULT WINAPI IDsDriverBufferImpl_Lock(PIDSDRIVERBUFFER iface,
					       LPVOID*ppvAudio1,LPDWORD pdwLen1,
					       LPVOID*ppvAudio2,LPDWORD pdwLen2,
					       DWORD dwWritePosition,DWORD dwWriteLen,
					       DWORD dwFlags)
{
    /* ICOM_THIS(IDsDriverBufferImpl,iface); */
    /* since we (GetDriverDesc flags) have specified DSDDESC_DONTNEEDPRIMARYLOCK,
     * and that we don't support secondary buffers, this method will never be called */
    TRACE("(%p): stub\n",iface);
    return DSERR_UNSUPPORTED;
}

static HRESULT WINAPI IDsDriverBufferImpl_Unlock(PIDSDRIVERBUFFER iface,
						 LPVOID pvAudio1,DWORD dwLen1,
						 LPVOID pvAudio2,DWORD dwLen2)
{
    /* ICOM_THIS(IDsDriverBufferImpl,iface); */
    TRACE("(%p): stub\n",iface);
    return DSERR_UNSUPPORTED;
}

static HRESULT WINAPI IDsDriverBufferImpl_SetFormat(PIDSDRIVERBUFFER iface,
						    LPWAVEFORMATEX pwfx)
{
    /* ICOM_THIS(IDsDriverBufferImpl,iface); */

    TRACE("(%p,%p)\n",iface,pwfx);
    /* On our request (GetDriverDesc flags), DirectSound has by now used
     * waveOutClose/waveOutOpen to set the format...
     * unfortunately, this means our mmap() is now gone...
     * so we need to somehow signal to our DirectSound implementation
     * that it should completely recreate this HW buffer...
     * this unexpected error code should do the trick... */
    return DSERR_BUFFERLOST;
}

static HRESULT WINAPI IDsDriverBufferImpl_SetFrequency(PIDSDRIVERBUFFER iface, DWORD dwFreq)
{
    /* ICOM_THIS(IDsDriverBufferImpl,iface); */
    TRACE("(%p,%ld): stub\n",iface,dwFreq);
    return DSERR_UNSUPPORTED;
}

static HRESULT WINAPI IDsDriverBufferImpl_SetVolumePan(PIDSDRIVERBUFFER iface, PDSVOLUMEPAN pVolPan)
{
    /* ICOM_THIS(IDsDriverBufferImpl,iface); */
    FIXME("(%p,%p): stub!\n",iface,pVolPan);
    return DSERR_UNSUPPORTED;
}

static HRESULT WINAPI IDsDriverBufferImpl_SetPosition(PIDSDRIVERBUFFER iface, DWORD dwNewPos)
{
    /* ICOM_THIS(IDsDriverImpl,iface); */
    TRACE("(%p,%ld): stub\n",iface,dwNewPos);
    return DSERR_UNSUPPORTED;
}

static HRESULT WINAPI IDsDriverBufferImpl_GetPosition(PIDSDRIVERBUFFER iface,
						      LPDWORD lpdwPlay, LPDWORD lpdwWrite)
{
    ICOM_THIS(IDsDriverBufferImpl,iface);
    count_info info;
    DWORD ptr;

    TRACE("(%p)\n",iface);
    if (WOutDev[This->drv->wDevID].unixdev == -1) {
	ERR("device not open, but accessing?\n");
	return DSERR_UNINITIALIZED;
    }
    if (ioctl(WOutDev[This->drv->wDevID].unixdev, SNDCTL_DSP_GETOPTR, &info) < 0) {
	ERR("ioctl failed (%d)\n", errno);
	return DSERR_GENERIC;
    }
    ptr = info.ptr & ~3; /* align the pointer, just in case */
    if (lpdwPlay) *lpdwPlay = ptr;
    if (lpdwWrite) {
	/* add some safety margin (not strictly necessary, but...) */
	if (WOutDev[This->drv->wDevID].caps.dwSupport & WAVECAPS_SAMPLEACCURATE)
	    *lpdwWrite = ptr + 32;
	else
	    *lpdwWrite = ptr + WOutDev[This->drv->wDevID].dwFragmentSize;
	while (*lpdwWrite > This->buflen)
	    *lpdwWrite -= This->buflen;
    }
    TRACE("playpos=%ld, writepos=%ld\n", lpdwPlay?*lpdwPlay:0, lpdwWrite?*lpdwWrite:0);
    return DS_OK;
}

static HRESULT WINAPI IDsDriverBufferImpl_Play(PIDSDRIVERBUFFER iface, DWORD dwRes1, DWORD dwRes2, DWORD dwFlags)
{
    ICOM_THIS(IDsDriverBufferImpl,iface);
    int enable = PCM_ENABLE_OUTPUT;
    TRACE("(%p,%lx,%lx,%lx)\n",iface,dwRes1,dwRes2,dwFlags);
    if (ioctl(WOutDev[This->drv->wDevID].unixdev, SNDCTL_DSP_SETTRIGGER, &enable) < 0) {
	ERR("ioctl failed (%d)\n", errno);
	return DSERR_GENERIC;
    }
    return DS_OK;
}

static HRESULT WINAPI IDsDriverBufferImpl_Stop(PIDSDRIVERBUFFER iface)
{
    ICOM_THIS(IDsDriverBufferImpl,iface);
    int enable = 0;
    TRACE("(%p)\n",iface);
    /* no more playing */
    if (ioctl(WOutDev[This->drv->wDevID].unixdev, SNDCTL_DSP_SETTRIGGER, &enable) < 0) {
	ERR("ioctl failed (%d)\n", errno);
	return DSERR_GENERIC;
    }
#if 0
    /* the play position must be reset to the beginning of the buffer */
    if (ioctl(WOutDev[This->drv->wDevID].unixdev, SNDCTL_DSP_RESET, 0) < 0) {
	ERR("ioctl failed (%d)\n", errno);
	return DSERR_GENERIC;
    }
#endif
    /* Most OSS drivers just can't stop the playback without closing the device...
     * so we need to somehow signal to our DirectSound implementation
     * that it should completely recreate this HW buffer...
     * this unexpected error code should do the trick... */
    return DSERR_BUFFERLOST;
}

static ICOM_VTABLE(IDsDriverBuffer) dsdbvt =
{
    ICOM_MSVTABLE_COMPAT_DummyRTTIVALUE
    IDsDriverBufferImpl_QueryInterface,
    IDsDriverBufferImpl_AddRef,
    IDsDriverBufferImpl_Release,
    IDsDriverBufferImpl_Lock,
    IDsDriverBufferImpl_Unlock,
    IDsDriverBufferImpl_SetFormat,
    IDsDriverBufferImpl_SetFrequency,
    IDsDriverBufferImpl_SetVolumePan,
    IDsDriverBufferImpl_SetPosition,
    IDsDriverBufferImpl_GetPosition,
    IDsDriverBufferImpl_Play,
    IDsDriverBufferImpl_Stop
};

static HRESULT WINAPI IDsDriverImpl_QueryInterface(PIDSDRIVER iface, REFIID riid, LPVOID *ppobj)
{
    /* ICOM_THIS(IDsDriverImpl,iface); */
    FIXME("(%p): stub!\n",iface);
    return DSERR_UNSUPPORTED;
}

static ULONG WINAPI IDsDriverImpl_AddRef(PIDSDRIVER iface)
{
    ICOM_THIS(IDsDriverImpl,iface);
    This->ref++;
    return This->ref;
}

static ULONG WINAPI IDsDriverImpl_Release(PIDSDRIVER iface)
{
    ICOM_THIS(IDsDriverImpl,iface);
    if (--This->ref)
	return This->ref;
    HeapFree(GetProcessHeap(),0,This);
    return 0;
}

static HRESULT WINAPI IDsDriverImpl_GetDriverDesc(PIDSDRIVER iface, PDSDRIVERDESC pDesc)
{
    ICOM_THIS(IDsDriverImpl,iface);
    TRACE("(%p,%p)\n",iface,pDesc);
    pDesc->dwFlags = DSDDESC_DOMMSYSTEMOPEN | DSDDESC_DOMMSYSTEMSETFORMAT |
	DSDDESC_USESYSTEMMEMORY | DSDDESC_DONTNEEDPRIMARYLOCK;
    strcpy(pDesc->szDesc,"WineOSS DirectSound Driver");
    strcpy(pDesc->szDrvName,"wineoss.drv");
    pDesc->dnDevNode		= WOutDev[This->wDevID].waveDesc.dnDevNode;
    pDesc->wVxdId		= 0; 
    pDesc->wReserved		= 0;
    pDesc->ulDeviceNum		= This->wDevID;
    pDesc->dwHeapType		= DSDHEAP_NOHEAP;
    pDesc->pvDirectDrawHeap	= NULL;
    pDesc->dwMemStartAddress	= 0;
    pDesc->dwMemEndAddress	= 0;
    pDesc->dwMemAllocExtra	= 0;
    pDesc->pvReserved1		= NULL;
    pDesc->pvReserved2		= NULL;
    return DS_OK;
}

static HRESULT WINAPI IDsDriverImpl_Open(PIDSDRIVER iface)
{
    ICOM_THIS(IDsDriverImpl,iface);
    int enable = 0;

    TRACE("(%p)\n",iface);
    /* make sure the card doesn't start playing before we want it to */
    if (ioctl(WOutDev[This->wDevID].unixdev, SNDCTL_DSP_SETTRIGGER, &enable) < 0) {
	ERR("ioctl failed (%d)\n", errno);
	return DSERR_GENERIC;
    }
    return DS_OK;
}

static HRESULT WINAPI IDsDriverImpl_Close(PIDSDRIVER iface)
{
    ICOM_THIS(IDsDriverImpl,iface);
    TRACE("(%p)\n",iface);
    if (This->primary) {
	ERR("problem with DirectSound: primary not released\n");
	return DSERR_GENERIC;
    }
    return DS_OK;
}

static HRESULT WINAPI IDsDriverImpl_GetCaps(PIDSDRIVER iface, PDSDRIVERCAPS pCaps)
{
    /* ICOM_THIS(IDsDriverImpl,iface); */
    TRACE("(%p,%p)\n",iface,pCaps);
    memset(pCaps, 0, sizeof(*pCaps));
    /* FIXME: need to check actual capabilities */
    pCaps->dwFlags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO |
	DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT;
    pCaps->dwPrimaryBuffers = 1;
    /* the other fields only apply to secondary buffers, which we don't support
     * (unless we want to mess with wavetable synthesizers and MIDI) */
    return DS_OK;
}

static HRESULT WINAPI IDsDriverImpl_CreateSoundBuffer(PIDSDRIVER iface,
						      LPWAVEFORMATEX pwfx,
						      DWORD dwFlags, DWORD dwCardAddress,
						      LPDWORD pdwcbBufferSize,
						      LPBYTE *ppbBuffer,
						      LPVOID *ppvObj)
{
    ICOM_THIS(IDsDriverImpl,iface);
    IDsDriverBufferImpl** ippdsdb = (IDsDriverBufferImpl**)ppvObj;
    HRESULT err;
    audio_buf_info info;
    int enable = 0;

    TRACE("(%p,%p,%lx,%lx)\n",iface,pwfx,dwFlags,dwCardAddress);
    /* we only support primary buffers */
    if (!(dwFlags & DSBCAPS_PRIMARYBUFFER))
	return DSERR_UNSUPPORTED;
    if (This->primary)
	return DSERR_ALLOCATED;
    if (dwFlags & (DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPAN))
	return DSERR_CONTROLUNAVAIL;

    *ippdsdb = (IDsDriverBufferImpl*)HeapAlloc(GetProcessHeap(),0,sizeof(IDsDriverBufferImpl));
    if (*ippdsdb == NULL)
	return DSERR_OUTOFMEMORY;
    ICOM_VTBL(*ippdsdb) = &dsdbvt;
    (*ippdsdb)->ref	= 1;
    (*ippdsdb)->drv	= This;

    /* check how big the DMA buffer is now */
    if (ioctl(WOutDev[This->wDevID].unixdev, SNDCTL_DSP_GETOSPACE, &info) < 0) {
	ERR("ioctl failed (%d)\n", errno);
	HeapFree(GetProcessHeap(),0,*ippdsdb);
	*ippdsdb = NULL;
	return DSERR_GENERIC;
    }
    WOutDev[This->wDevID].maplen = (*ippdsdb)->buflen = info.fragstotal * info.fragsize;

    /* map the DMA buffer */
    err = DSDB_MapPrimary(*ippdsdb);
    if (err != DS_OK) {
	HeapFree(GetProcessHeap(),0,*ippdsdb);
	*ippdsdb = NULL;
	return err;
    }

    /* primary buffer is ready to go */
    *pdwcbBufferSize	= WOutDev[This->wDevID].maplen;
    *ppbBuffer		= WOutDev[This->wDevID].mapping;

    /* some drivers need some extra nudging after mapping */
    if (ioctl(WOutDev[This->wDevID].unixdev, SNDCTL_DSP_SETTRIGGER, &enable) < 0) {
	ERR("ioctl failed (%d)\n", errno);
	return DSERR_GENERIC;
    }

    This->primary = *ippdsdb;

    return DS_OK;
}

static HRESULT WINAPI IDsDriverImpl_DuplicateSoundBuffer(PIDSDRIVER iface,
							 PIDSDRIVERBUFFER pBuffer,
							 LPVOID *ppvObj)
{
    /* ICOM_THIS(IDsDriverImpl,iface); */
    TRACE("(%p,%p): stub\n",iface,pBuffer);
    return DSERR_INVALIDCALL;
}

static ICOM_VTABLE(IDsDriver) dsdvt =
{
    ICOM_MSVTABLE_COMPAT_DummyRTTIVALUE
    IDsDriverImpl_QueryInterface,
    IDsDriverImpl_AddRef,
    IDsDriverImpl_Release,
    IDsDriverImpl_GetDriverDesc,
    IDsDriverImpl_Open,
    IDsDriverImpl_Close,
    IDsDriverImpl_GetCaps,
    IDsDriverImpl_CreateSoundBuffer,
    IDsDriverImpl_DuplicateSoundBuffer
};

static DWORD wodDsCreate(UINT wDevID, PIDSDRIVER* drv)
{
    IDsDriverImpl** idrv = (IDsDriverImpl**)drv;

    /* the HAL isn't much better than the HEL if we can't do mmap() */
    if (!(WOutDev[wDevID].caps.dwSupport & WAVECAPS_DIRECTSOUND)) {
	ERR("DirectSound flag not set\n");
	MESSAGE("This sound card's driver does not support direct access\n");
	MESSAGE("The (slower) DirectSound HEL mode will be used instead.\n");
	return MMSYSERR_NOTSUPPORTED;
    }

    *idrv = (IDsDriverImpl*)HeapAlloc(GetProcessHeap(),0,sizeof(IDsDriverImpl));
    if (!*idrv)
	return MMSYSERR_NOMEM;
    ICOM_VTBL(*idrv)	= &dsdvt;
    (*idrv)->ref	= 1;

    (*idrv)->wDevID	= wDevID;
    (*idrv)->primary	= NULL;
    return MMSYSERR_NOERROR;
}

/*======================================================================*
 *                  Low level WAVE IN implementation			*
 *======================================================================*/

/**************************************************************************
 * 			widNotifyClient			[internal]
 */
static DWORD widNotifyClient(WINE_WAVEIN* wwi, WORD wMsg, DWORD dwParam1, DWORD dwParam2)
{
    TRACE("wMsg = 0x%04x dwParm1 = %04lX dwParam2 = %04lX\n", wMsg, dwParam1, dwParam2);
    
    switch (wMsg) {
    case WIM_OPEN:
    case WIM_CLOSE:
    case WIM_DATA:
	if (wwi->wFlags != DCB_NULL && 
	    !DriverCallback(wwi->waveDesc.dwCallback, wwi->wFlags, wwi->waveDesc.hWave, 
			    wMsg, wwi->waveDesc.dwInstance, dwParam1, dwParam2)) {
	    WARN("can't notify client !\n");
	    return MMSYSERR_ERROR;
	}
	break;
    default:
	FIXME("Unknown CB message %u\n", wMsg);
	return MMSYSERR_INVALPARAM;
    }
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 			widGetDevCaps				[internal]
 */
static DWORD widGetDevCaps(WORD wDevID, LPWAVEINCAPSA lpCaps, DWORD dwSize)
{
    TRACE("(%u, %p, %lu);\n", wDevID, lpCaps, dwSize);
    
    if (lpCaps == NULL) return MMSYSERR_NOTENABLED;
    
    if (wDevID >= MAX_WAVEINDRV) {
	TRACE("MAX_WAVINDRV reached !\n");
	return MMSYSERR_BADDEVICEID;
    }

    memcpy(lpCaps, &WInDev[wDevID].caps, min(dwSize, sizeof(*lpCaps)));
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				widRecorder			[internal]
 */
static	DWORD	CALLBACK	widRecorder(LPVOID pmt)
{
    WORD		uDevID = (DWORD)pmt;
    WINE_WAVEIN*	wwi = (WINE_WAVEIN*)&WInDev[uDevID];
    WAVEHDR*		lpWaveHdr;
    DWORD		dwSleepTime;
    DWORD		bytesRead;
    LPVOID		buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, wwi->dwFragmentSize);
    LPVOID		pOffset = buffer;
    audio_buf_info 	info;
    int 		xs;
    enum win_wm_message msg;
    DWORD		param;
    HANDLE		ev;

    wwi->state = WINE_WS_STOPPED;
    wwi->dwTotalRecorded = 0;

    SetEvent(wwi->hStartUpEvent);

    /* the soundblaster live needs a micro wake to get its recording started
     * (or GETISPACE will have 0 frags all the time)
     */
    read(wwi->unixdev,&xs,4);
    
	/* make sleep time to be # of ms to output a fragment */
    dwSleepTime = (wwi->dwFragmentSize * 1000) / wwi->format.wf.nAvgBytesPerSec;
    TRACE("sleeptime=%ld ms\n", dwSleepTime);

    for (;;) {
	/* wait for dwSleepTime or an event in thread's queue */
	/* FIXME: could improve wait time depending on queue state,
	 * ie, number of queued fragments
	 */

	if (wwi->lpQueuePtr != NULL && wwi->state == WINE_WS_PLAYING) 
        {
            lpWaveHdr = wwi->lpQueuePtr;

	    ioctl(wwi->unixdev, SNDCTL_DSP_GETISPACE, &info);
            TRACE("info={frag=%d fsize=%d ftotal=%d bytes=%d}\n", info.fragments, info.fragsize, info.fragstotal, info.bytes);

            /* read all the fragments accumulated so far */
            while ((info.fragments > 0) && (wwi->lpQueuePtr))
            {
                info.fragments --;

                if (lpWaveHdr->dwBufferLength - lpWaveHdr->dwBytesRecorded >= wwi->dwFragmentSize)
                {
                    /* directly read fragment in wavehdr */
                    bytesRead = read(wwi->unixdev, 
		      		     lpWaveHdr->lpData + lpWaveHdr->dwBytesRecorded, 
                                     wwi->dwFragmentSize);

                    TRACE("bytesRead=%ld (direct)\n", bytesRead);
		    if (bytesRead != (DWORD) -1)
		    {
			/* update number of bytes recorded in current buffer and by this device */
                        lpWaveHdr->dwBytesRecorded += bytesRead;
			wwi->dwTotalRecorded       += bytesRead;
				
			/* buffer is full. notify client */
			if (lpWaveHdr->dwBytesRecorded == lpWaveHdr->dwBufferLength) 
			{
			    /* must copy the value of next waveHdr, because we have no idea of what
			     * will be done with the content of lpWaveHdr in callback
			     */
			    LPWAVEHDR	lpNext = lpWaveHdr->lpNext;

			    lpWaveHdr->dwFlags &= ~WHDR_INQUEUE;
			    lpWaveHdr->dwFlags |=  WHDR_DONE;

			    widNotifyClient(wwi, WIM_DATA, (DWORD)lpWaveHdr, 0);
			    lpWaveHdr = wwi->lpQueuePtr = lpNext;
			}
                    }
                }
                else
		{
                    /* read the fragment in a local buffer */
                    bytesRead = read(wwi->unixdev, buffer, wwi->dwFragmentSize);
                    pOffset = buffer;

                    TRACE("bytesRead=%ld (local)\n", bytesRead);

                    /* copy data in client buffers */	
                    while (bytesRead != (DWORD) -1 && bytesRead > 0)
                    {
                        DWORD dwToCopy = min (bytesRead, lpWaveHdr->dwBufferLength - lpWaveHdr->dwBytesRecorded);

                        memcpy(lpWaveHdr->lpData + lpWaveHdr->dwBytesRecorded,
                               pOffset,
                               dwToCopy);

                        /* update number of bytes recorded in current buffer and by this device */
                        lpWaveHdr->dwBytesRecorded += dwToCopy;
                        wwi->dwTotalRecorded += dwToCopy;
                        bytesRead -= dwToCopy;
                        pOffset   += dwToCopy;
				
                        /* client buffer is full. notify client */
                        if (lpWaveHdr->dwBytesRecorded == lpWaveHdr->dwBufferLength) 
                        {
			    /* must copy the value of next waveHdr, because we have no idea of what
			     * will be done with the content of lpWaveHdr in callback
			     */
			    LPWAVEHDR	lpNext = lpWaveHdr->lpNext;
			    TRACE("lpNext=%p\n", lpNext);

                            lpWaveHdr->dwFlags &= ~WHDR_INQUEUE;
                            lpWaveHdr->dwFlags |=  WHDR_DONE;

                            widNotifyClient(wwi, WIM_DATA, (DWORD)lpWaveHdr, 0);
				   
			    wwi->lpQueuePtr = lpWaveHdr = lpNext;
			    if (!lpNext && bytesRead) {
                                /* no more buffer to copy data to, but we did read more. 
                                 * what hasn't been copied will be dropped
                                 */ 
                                WARN("buffer under run! %lu bytes dropped.\n", bytesRead);
                                wwi->lpQueuePtr = NULL;
                                break;
                            }
                        }
                    }
                }
            }
	}
	
	WaitForSingleObject(wwi->msgRing.msg_event, dwSleepTime);

	while (OSS_RetrieveRingMessage(&wwi->msgRing, &msg, &param, &ev))
	{

            TRACE("msg=0x%x param=0x%lx\n", msg, param);
	    switch (msg) {
	    case WINE_WM_PAUSING:
		wwi->state = WINE_WS_PAUSED;
                /*FIXME("Device should stop recording\n");*/
		SetEvent(ev);
		break;
	    case WINE_WM_RESTARTING:
            {
                int enable = PCM_ENABLE_INPUT;
		wwi->state = WINE_WS_PLAYING;

                if (wwi->bTriggerSupport)
                {
                    /* start the recording */
                    if (ioctl(wwi->unixdev, SNDCTL_DSP_SETTRIGGER, &enable) < 0) 
                    {
                        ERR("ioctl(SNDCTL_DSP_SETTRIGGER) failed (%d)\n", errno);
                    }
                }
                else
                {
                    unsigned char data[4];
                    /* read 4 bytes to start the recording */
                    read(wwi->unixdev, data, 4);
                }
                
		SetEvent(ev);
		break;
            }
	    case WINE_WM_HEADER:
		lpWaveHdr = (LPWAVEHDR)param;
		lpWaveHdr->lpNext = 0;

		/* insert buffer at the end of queue */
		{
		    LPWAVEHDR*	wh;
		    for (wh = &(wwi->lpQueuePtr); *wh; wh = &((*wh)->lpNext));
		    *wh = lpWaveHdr;
		}
		break;
	    case WINE_WM_RESETTING:
		wwi->state = WINE_WS_STOPPED;
		/* return all buffers to the app */
		for (lpWaveHdr = wwi->lpQueuePtr; lpWaveHdr; lpWaveHdr = lpWaveHdr->lpNext) {
		    TRACE("reset %p %p\n", lpWaveHdr, lpWaveHdr->lpNext);
		    lpWaveHdr->dwFlags &= ~WHDR_INQUEUE;
		    lpWaveHdr->dwFlags |= WHDR_DONE;
	
		    widNotifyClient(wwi, WIM_DATA, (DWORD)lpWaveHdr, 0);
		}
		wwi->lpQueuePtr = NULL;
		SetEvent(ev);
		break;
	    case WINE_WM_CLOSING:
		wwi->hThread = 0;
		wwi->state = WINE_WS_CLOSED;
		SetEvent(ev);
		HeapFree(GetProcessHeap(), 0, buffer); 
		ExitThread(0);
		/* shouldn't go here */
	    default:
		FIXME("unknown message %d\n", msg);
		break;
	    }
	}
    }
    ExitThread(0);
    /* just for not generating compilation warnings... should never be executed */
    return 0; 
}


/**************************************************************************
 * 				widOpen				[internal]
 */
static DWORD widOpen(WORD wDevID, LPWAVEOPENDESC lpDesc, DWORD dwFlags)
{
    int 		audio;
    int			fragment_size;
    int			sample_rate;
    int			format;
    int			dsp_stereo;
    WINE_WAVEIN*	wwi;
    int			audio_fragment;

    TRACE("(%u, %p, %08lX);\n", wDevID, lpDesc, dwFlags);
    if (lpDesc == NULL) {
	WARN("Invalid Parameter !\n");
	return MMSYSERR_INVALPARAM;
    }
    if (wDevID >= MAX_WAVEINDRV) return MMSYSERR_BADDEVICEID;

    /* only PCM format is supported so far... */
    if (lpDesc->lpFormat->wFormatTag != WAVE_FORMAT_PCM ||
	lpDesc->lpFormat->nChannels == 0 ||
	lpDesc->lpFormat->nSamplesPerSec == 0) {
	WARN("Bad format: tag=%04X nChannels=%d nSamplesPerSec=%ld !\n", 
	     lpDesc->lpFormat->wFormatTag, lpDesc->lpFormat->nChannels,
	     lpDesc->lpFormat->nSamplesPerSec);
	return WAVERR_BADFORMAT;
    }

    if (dwFlags & WAVE_FORMAT_QUERY) {
	TRACE("Query format: tag=%04X nChannels=%d nSamplesPerSec=%ld !\n", 
	     lpDesc->lpFormat->wFormatTag, lpDesc->lpFormat->nChannels,
	     lpDesc->lpFormat->nSamplesPerSec);
	return MMSYSERR_NOERROR;
    }

    if (access(SOUND_DEV,0) != 0) return MMSYSERR_NOTENABLED;
    audio = open(SOUND_DEV, O_RDONLY|O_NDELAY, 0);
    if (audio == -1) {
	WARN("can't open sound device %s (%s)!\n", SOUND_DEV, strerror(errno));
	return MMSYSERR_ALLOCATED;
    }
    fcntl(audio, F_SETFD, 1); /* set close on exec flag */

    wwi = &WInDev[wDevID];
    if (wwi->lpQueuePtr) {
	WARN("Should have an empty queue (%p)\n", wwi->lpQueuePtr);
	wwi->lpQueuePtr = NULL;
    }
    wwi->unixdev = audio;
    wwi->dwTotalRecorded = 0;
    wwi->wFlags = HIWORD(dwFlags & CALLBACK_TYPEMASK);

    memcpy(&wwi->waveDesc, lpDesc,           sizeof(WAVEOPENDESC));
    memcpy(&wwi->format,   lpDesc->lpFormat, sizeof(PCMWAVEFORMAT));

    if (wwi->format.wBitsPerSample == 0) {
	WARN("Resetting zeroed wBitsPerSample\n");
	wwi->format.wBitsPerSample = 8 *
	    (wwi->format.wf.nAvgBytesPerSec /
	     wwi->format.wf.nSamplesPerSec) /
	    wwi->format.wf.nChannels;
    }

    sample_rate = wwi->format.wf.nSamplesPerSec;
    dsp_stereo = (wwi->format.wf.nChannels > 1) ? TRUE : FALSE;
    format = (wwi->format.wBitsPerSample == 16) ? AFMT_S16_LE : AFMT_U8;


    IOCTL(audio, SNDCTL_DSP_SETFMT, format);
    IOCTL(audio, SNDCTL_DSP_STEREO, dsp_stereo);
    IOCTL(audio, SNDCTL_DSP_SPEED,  sample_rate);


    /* This is actually hand tuned to work so that my SB Live:
     * - does not skip
     * - does not buffer too much
     * when sending with the Shoutcast winamp plugin
     */
    /* 7 fragments max, 2^10 = 1024 bytes per fragment */
    audio_fragment = 0x0007000A;
    IOCTL(audio, SNDCTL_DSP_SETFRAGMENT, audio_fragment);

    /* paranoid checks */
    if (format != ((wwi->format.wBitsPerSample == 16) ? AFMT_S16_LE : AFMT_U8))
	ERR("Can't set format to %d (%d)\n", 
	    (wwi->format.wBitsPerSample == 16) ? AFMT_S16_LE : AFMT_U8, format);
    if (dsp_stereo != (wwi->format.wf.nChannels > 1) ? 1 : 0) 
	ERR("Can't set stereo to %u (%d)\n", 
	    (wwi->format.wf.nChannels > 1) ? 1 : 0, dsp_stereo);
    if (!NEAR_MATCH(sample_rate, wwi->format.wf.nSamplesPerSec))
	ERR("Can't set sample_rate to %lu (%d)\n", 
	    wwi->format.wf.nSamplesPerSec, sample_rate);

    IOCTL(audio, SNDCTL_DSP_GETBLKSIZE, fragment_size);
    if (fragment_size == -1) {
	WARN("IOCTL can't 'SNDCTL_DSP_GETBLKSIZE' !\n");
	close(audio);
	wwi->unixdev = -1;
	return MMSYSERR_NOTENABLED;
    }
    wwi->dwFragmentSize = fragment_size;

    TRACE("wBitsPerSample=%u, nAvgBytesPerSec=%lu, nSamplesPerSec=%lu, nChannels=%u nBlockAlign=%u!\n", 
	  wwi->format.wBitsPerSample, wwi->format.wf.nAvgBytesPerSec, 
	  wwi->format.wf.nSamplesPerSec, wwi->format.wf.nChannels,
	  wwi->format.wf.nBlockAlign);

    wwi->hStartUpEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    wwi->hThread = CreateThread(NULL, 0, widRecorder, (LPVOID)(DWORD)wDevID, 0, &(wwi->dwThreadID));
    WaitForSingleObject(wwi->hStartUpEvent, INFINITE);
    CloseHandle(wwi->hStartUpEvent);
    wwi->hStartUpEvent = INVALID_HANDLE_VALUE;

    return widNotifyClient(wwi, WIM_OPEN, 0L, 0L);
}

/**************************************************************************
 * 				widClose			[internal]
 */
static DWORD widClose(WORD wDevID)
{
    WINE_WAVEIN*	wwi;

    TRACE("(%u);\n", wDevID);
    if (wDevID >= MAX_WAVEINDRV || WInDev[wDevID].unixdev == -1) {
	WARN("can't close !\n");
	return MMSYSERR_INVALHANDLE;
    }

    wwi = &WInDev[wDevID];

    if (wwi->lpQueuePtr != NULL) {
	WARN("still buffers open !\n");
	return WAVERR_STILLPLAYING;
    }

    OSS_AddRingMessage(&wwi->msgRing, WINE_WM_CLOSING, 0, TRUE);
    close(wwi->unixdev);
    wwi->unixdev = -1;
    wwi->dwFragmentSize = 0;
    return widNotifyClient(wwi, WIM_CLOSE, 0L, 0L);
}

/**************************************************************************
 * 				widAddBuffer		[internal]
 */
static DWORD widAddBuffer(WORD wDevID, LPWAVEHDR lpWaveHdr, DWORD dwSize)
{
    TRACE("(%u, %p, %08lX);\n", wDevID, lpWaveHdr, dwSize);

    if (wDevID >= MAX_WAVEINDRV || WInDev[wDevID].unixdev == -1) {
	WARN("can't do it !\n");
	return MMSYSERR_INVALHANDLE;
    }
    if (!(lpWaveHdr->dwFlags & WHDR_PREPARED)) {
	TRACE("never been prepared !\n");
	return WAVERR_UNPREPARED;
    }
    if (lpWaveHdr->dwFlags & WHDR_INQUEUE) {
	TRACE("header already in use !\n");
	return WAVERR_STILLPLAYING;
    }

    lpWaveHdr->dwFlags |= WHDR_INQUEUE;
    lpWaveHdr->dwFlags &= ~WHDR_DONE;
    lpWaveHdr->dwBytesRecorded = 0;
    lpWaveHdr->lpNext = NULL;
	
    OSS_AddRingMessage(&WInDev[wDevID].msgRing, WINE_WM_HEADER, (DWORD)lpWaveHdr, FALSE);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				widPrepare			[internal]
 */
static DWORD widPrepare(WORD wDevID, LPWAVEHDR lpWaveHdr, DWORD dwSize)
{
    TRACE("(%u, %p, %08lX);\n", wDevID, lpWaveHdr, dwSize);

    if (wDevID >= MAX_WAVEINDRV) return MMSYSERR_INVALHANDLE;

    if (lpWaveHdr->dwFlags & WHDR_INQUEUE)
	return WAVERR_STILLPLAYING;

    lpWaveHdr->dwFlags |= WHDR_PREPARED;
    lpWaveHdr->dwFlags &= ~(WHDR_INQUEUE|WHDR_DONE);
    lpWaveHdr->dwBytesRecorded = 0;
    TRACE("header prepared !\n");
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				widUnprepare			[internal]
 */
static DWORD widUnprepare(WORD wDevID, LPWAVEHDR lpWaveHdr, DWORD dwSize)
{
    TRACE("(%u, %p, %08lX);\n", wDevID, lpWaveHdr, dwSize);
    if (wDevID >= MAX_WAVEINDRV) return MMSYSERR_INVALHANDLE;

    if (lpWaveHdr->dwFlags & WHDR_INQUEUE)
	return WAVERR_STILLPLAYING;

    lpWaveHdr->dwFlags &= ~(WHDR_PREPARED|WHDR_INQUEUE);
    lpWaveHdr->dwFlags |= WHDR_DONE;
    
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 			widStart				[internal]
 */
static DWORD widStart(WORD wDevID)
{
    TRACE("(%u);\n", wDevID);
    if (wDevID >= MAX_WAVEINDRV || WInDev[wDevID].unixdev == -1) {
	WARN("can't start recording !\n");
	return MMSYSERR_INVALHANDLE;
    }

    OSS_AddRingMessage(&WInDev[wDevID].msgRing, WINE_WM_RESTARTING, 0, TRUE);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 			widStop					[internal]
 */
static DWORD widStop(WORD wDevID)
{
    TRACE("(%u);\n", wDevID);
    if (wDevID >= MAX_WAVEINDRV || WInDev[wDevID].unixdev == -1) {
	WARN("can't stop !\n");
	return MMSYSERR_INVALHANDLE;
    }
    /* FIXME: reset aint stop */
    OSS_AddRingMessage(&WInDev[wDevID].msgRing, WINE_WM_RESETTING, 0, TRUE);
    
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 			widReset				[internal]
 */
static DWORD widReset(WORD wDevID)
{
    TRACE("(%u);\n", wDevID);
    if (wDevID >= MAX_WAVEINDRV || WInDev[wDevID].unixdev == -1) {
	WARN("can't reset !\n");
	return MMSYSERR_INVALHANDLE;
    }
    OSS_AddRingMessage(&WInDev[wDevID].msgRing, WINE_WM_RESETTING, 0, TRUE);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				widGetPosition			[internal]
 */
static DWORD widGetPosition(WORD wDevID, LPMMTIME lpTime, DWORD uSize)
{
    int			time;
    WINE_WAVEIN*	wwi;
    
    TRACE("(%u, %p, %lu);\n", wDevID, lpTime, uSize);

    if (wDevID >= MAX_WAVEINDRV || WInDev[wDevID].unixdev == -1) {
	WARN("can't get pos !\n");
	return MMSYSERR_INVALHANDLE;
    }
    if (lpTime == NULL)	return MMSYSERR_INVALPARAM;

    wwi = &WInDev[wDevID];

    TRACE("wType=%04X !\n", lpTime->wType);
    TRACE("wBitsPerSample=%u\n", wwi->format.wBitsPerSample); 
    TRACE("nSamplesPerSec=%lu\n", wwi->format.wf.nSamplesPerSec); 
    TRACE("nChannels=%u\n", wwi->format.wf.nChannels); 
    TRACE("nAvgBytesPerSec=%lu\n", wwi->format.wf.nAvgBytesPerSec); 
    switch (lpTime->wType) {
    case TIME_BYTES:
	lpTime->u.cb = wwi->dwTotalRecorded;
	TRACE("TIME_BYTES=%lu\n", lpTime->u.cb);
	break;
    case TIME_SAMPLES:
	lpTime->u.sample = wwi->dwTotalRecorded * 8 /
	    wwi->format.wBitsPerSample;
	TRACE("TIME_SAMPLES=%lu\n", lpTime->u.sample);
	break;
    case TIME_SMPTE:
	time = wwi->dwTotalRecorded /
	    (wwi->format.wf.nAvgBytesPerSec / 1000);
	lpTime->u.smpte.hour = time / 108000;
	time -= lpTime->u.smpte.hour * 108000;
	lpTime->u.smpte.min = time / 1800;
	time -= lpTime->u.smpte.min * 1800;
	lpTime->u.smpte.sec = time / 30;
	time -= lpTime->u.smpte.sec * 30;
	lpTime->u.smpte.frame = time;
	lpTime->u.smpte.fps = 30;
	TRACE("TIME_SMPTE=%02u:%02u:%02u:%02u\n",
	      lpTime->u.smpte.hour, lpTime->u.smpte.min,
	      lpTime->u.smpte.sec, lpTime->u.smpte.frame);
	break;
    case TIME_MS:
	lpTime->u.ms = wwi->dwTotalRecorded /
	    (wwi->format.wf.nAvgBytesPerSec / 1000);
	TRACE("TIME_MS=%lu\n", lpTime->u.ms);
	break;
    default:
	FIXME("format not supported (%u) ! use TIME_MS !\n", lpTime->wType);
	lpTime->wType = TIME_MS;
    }
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				widMessage (WINEOSS.6)
 */
DWORD WINAPI OSS_widMessage(WORD wDevID, WORD wMsg, DWORD dwUser, 
			    DWORD dwParam1, DWORD dwParam2)
{
    TRACE("(%u, %04X, %08lX, %08lX, %08lX);\n",
	  wDevID, wMsg, dwUser, dwParam1, dwParam2);

    switch (wMsg) {
    case DRVM_INIT:
    case DRVM_EXIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
	/* FIXME: Pretend this is supported */
	return 0;
    case WIDM_OPEN:		return widOpen       (wDevID, (LPWAVEOPENDESC)dwParam1, dwParam2);
    case WIDM_CLOSE:		return widClose      (wDevID);
    case WIDM_ADDBUFFER:	return widAddBuffer  (wDevID, (LPWAVEHDR)dwParam1, dwParam2);
    case WIDM_PREPARE:		return widPrepare    (wDevID, (LPWAVEHDR)dwParam1, dwParam2);
    case WIDM_UNPREPARE:	return widUnprepare  (wDevID, (LPWAVEHDR)dwParam1, dwParam2);
    case WIDM_GETDEVCAPS:	return widGetDevCaps (wDevID, (LPWAVEINCAPSA)dwParam1, dwParam2);
    case WIDM_GETNUMDEVS:	return wodGetNumDevs ();	/* same number of devices in output as in input */
    case WIDM_GETPOS:		return widGetPosition(wDevID, (LPMMTIME)dwParam1, dwParam2);
    case WIDM_RESET:		return widReset      (wDevID);
    case WIDM_START:		return widStart      (wDevID);
    case WIDM_STOP:		return widStop       (wDevID);
    default:
	FIXME("unknown message %u!\n", wMsg);
    }
    return MMSYSERR_NOTSUPPORTED;
}

#else /* !HAVE_OSS */

/**************************************************************************
 * 				wodMessage (WINEOSS.7)
 */
DWORD WINAPI OSS_wodMessage(WORD wDevID, WORD wMsg, DWORD dwUser, 
			    DWORD dwParam1, DWORD dwParam2)
{
    FIXME("(%u, %04X, %08lX, %08lX, %08lX):stub\n", wDevID, wMsg, dwUser, dwParam1, dwParam2);
    return MMSYSERR_NOTENABLED;
}

/**************************************************************************
 * 				widMessage (WINEOSS.6)
 */
DWORD WINAPI OSS_widMessage(WORD wDevID, WORD wMsg, DWORD dwUser, 
			    DWORD dwParam1, DWORD dwParam2)
{
    FIXME("(%u, %04X, %08lX, %08lX, %08lX):stub\n", wDevID, wMsg, dwUser, dwParam1, dwParam2);
    return MMSYSERR_NOTENABLED;
}

#endif /* HAVE_OSS */
