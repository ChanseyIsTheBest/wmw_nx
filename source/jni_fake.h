/* jni_fake.h -- fake JNI environment for the Where's My Water? engine
 *               (libwmw.so, com.disney.WMW)
 *
 * The engine does the heavy lifting natively: it renders through GLES 1.1
 * fixed-function, parses its level data with a statically-linked libxml2, reads
 * its assets straight off disk with fopen(), and plays audio through FMOD Ex.
 * The Java side it calls back into is therefore thin -- com.disney.common.BaseActivity,
 * which on Android brokered display metrics, locale, achievements, in-app
 * purchases, Disney's AMPS content downloads and cloud saves.
 *
 * This file provides the generic JNIEnv/JavaVM machinery; the WMW-specific
 * answers live in wmw_jni.c, which is consulted before the logged default.
 * Slots 229..231 (NewDirectByteBuffer / GetDirectBufferAddress /
 * GetDirectBufferCapacity) exist for fmod_audio.c, which drives FMOD's PCM
 * callback the same way org.fmod.FMODAudioDevice did.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stdint.h>
#include <stddef.h>

extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *
// Set when the engine asks Android to show the soft keyboard (JNI "ShowKeyboard"
// with show=true); main.c services it with the Switch software keyboard.
extern volatile int g_kbd_requested;

// set if the engine ever asks the activity to finish
extern volatile int jni_quit_requested;

void jni_init(void);

// the fake activity instance / class handed to the JNI entry points
void *jni_make_thiz(void);

// constructors for fake Java objects
void *jni_make_string(const char *utf);
void *jni_make_object(const char *label);
// Fake direct ByteBuffer over an existing allocation -- GetDirectBufferAddress
// on it returns `data`. Used by fmod_audio.c to drive FMOD's PCM callback.
void *jni_make_direct_buffer(void *data, size_t len);
// Fake jfloatArray of `len` elements; *out points at the backing store so the
// caller can fill it in place each frame (used for the touch arrays).
void *jni_make_float_array(int len, float **out);
// Fake jintArray; *out points at the backing store (touch pointer ids).
void *jni_make_int_array(int len, int32_t **out);

/* Detach a reference from the local table so the engine can never free it.
 *
 * Everything jni_make_* produces is registered as a JNI LOCAL reference, and
 * the engine is entitled to discard those -- DeleteLocalRef, or PopLocalFrame
 * dropping everything above a mark. That is correct for the short-lived strings
 * handed to a single callback, and catastrophic for anything the port keeps
 * pointers into: free_ref() releases the backing store as well, so the port
 * then writes into freed memory every frame and the heap fails later, inside
 * free(), a long way from the cause.
 *
 * Pin anything created once and used for the lifetime of the process. Pinned
 * refs are never freed -- deliberately, since they live until exit anyway. */
void jni_pin(void *ref);

// Force a specific return value for an (obfuscated) boolean/int manager method
// by name -- handy during bring-up when a particular query must answer "true"
// for the engine to proceed. Returns 0 on success, -1 if the table is full.
// (The fake-JNI logs every unhandled call so you can discover the names.)
int jni_force_int_return(const char *method_name, long value);

#endif
