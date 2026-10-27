/*
 * xdk_dsound.h -- the XDK's DirectSound at API level, mixed with SDL3.
 *
 * The title's statically linked DSOUND library drives the MCPX APU; the LLE
 * path (src/apu) emulates that hardware. This layer replaces the library's
 * entry points instead: a title binds them by address (an override per
 * function in its recomp_manual.c) and they mix in host code -- no APU, no
 * voice processor, no DSP.
 *
 * Covered (what THPS 2X, DSOUND 3925, uses): DirectSoundCreate,
 * DirectSoundCreateStream, DirectSoundDoWork; IDirectSound AddRef/Release/
 * CreateSoundBuffer/CreateSoundStream; IDirectSoundBuffer Release, Play,
 * Stop, GetStatus, Get/SetCurrentPosition, Lock, SetBufferData,
 * SetLoopRegion, SetFrequency, SetVolume; IDirectSoundStream (XMediaObject)
 * AddRef, Release, GetInfo, GetStatus, Process, Discontinuity, Flush, plus
 * SetVolume and Pause. Formats: PCM 8/16-bit and Xbox ADPCM, mono or stereo.
 * Not covered: 3D, mix bins, effects (DSP image), headroom -- accepted and
 * ignored by whatever entry point a title binds for them.
 *
 * Calling convention like xdk_xinput.h: arguments on the guest stack at
 * g_esp + 4, result in g_eax, stdcall arguments popped here. Stream packet
 * completions (status, size, event, callback) run on the title's thread, in
 * the next DirectSound call it makes, never on the audio thread.
 *
 * Reference design: XboxXBE-Recomp runtime/xdk/dsound.cpp (TGP482, MIT,
 * commit 65e959d4a5fd0c171dc7b17f3f9b44bab00928e6). Rewritten in C.
 */
#ifndef XDK_DSOUND_H
#define XDK_DSOUND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The title's CDirectSoundStream vtables (the object a stream pointer points
 * at starts with them; the title calls Process/GetStatus through them). */
void xdk_dsound_bind(uint32_t stream_vtable, uint32_t stream_vtable2);

/* RECOMP_DSOUND_HLE (default on once bound): 0 runs the title's own DSOUND
 * code on the emulated APU instead. */
int  xdk_dsound_active(void);

/* RECOMP_DSOUND_TRACE=1: log a call and its first nargs stack arguments.
 * Called by the title's bindings before either path. */
void xdk_dsound_trace(const char *name, int nargs);

void xdk_DirectSoundCreate(void);            /* (guid, ppDS, unk)         8? */
void xdk_DirectSoundCreateStream(void);      /* (desc, ppStream)          8  */
void xdk_DirectSoundDoWork(void);            /* ()                        0  */
void xdk_IDirectSound_AddRef(void);          /* (ds)                      4  */
void xdk_IDirectSound_Release(void);         /* (ds)                      4  */
void xdk_IDirectSound_CreateSoundBuffer(void); /* (ds, desc, ppBuf, unk) 16 */
void xdk_IDirectSound_CreateSoundStream(void); /* (ds, desc, ppStr, unk) 16 */

void xdk_DSB_Release(void);                  /* (buf)                     4  */
void xdk_DSB_Play(void);                     /* (buf, r1, r2, flags)     16  */
void xdk_DSB_Stop(void);                     /* (buf)                     4  */
void xdk_DSB_GetStatus(void);                /* (buf, pdwStatus)          8  */
void xdk_DSB_GetCurrentPosition(void);       /* (buf, pPlay, pWrite)     12  */
void xdk_DSB_SetCurrentPosition(void);       /* (buf, dwPlay)             8  */
void xdk_DSB_Lock(void);                     /* (buf, off, n, pp1, pn1, pp2, pn2, flags) 32 */
void xdk_DSB_SetBufferData(void);            /* (buf, pv, bytes)         12  */
void xdk_DSB_SetLoopRegion(void);            /* (buf, start, length)     12  */
void xdk_DSB_SetFrequency(void);             /* (buf, hz)                 8  */
void xdk_DSB_SetVolume(void);                /* (buf, mB)                 8  */

void xdk_DSS_AddRef(void);                   /* (this)                    4  */
void xdk_DSS_Release(void);                  /* (this)                    4  */
void xdk_DSS_GetInfo(void);                  /* (this, pInfo)             8  */
void xdk_DSS_GetStatus(void);                /* (this, pdwStatus)         8  */
void xdk_DSS_Process(void);                  /* (this, pIn, pOut)        12  */
void xdk_DSS_Discontinuity(void);            /* (this)                    4  */
void xdk_DSS_Flush(void);                    /* (this)                    4  */
void xdk_DSS_SetVolume(void);                /* (this, mB)                8  */
void xdk_DSS_Pause(void);                    /* (this, pause)             8  */

#ifdef __cplusplus
}
#endif

#endif /* XDK_DSOUND_H */
