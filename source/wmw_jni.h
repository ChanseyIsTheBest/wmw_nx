/* wmw_jni.h -- answers for the Java methods libwmw.so calls back into
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __WMW_JNI_H__
#define __WMW_JNI_H__

// Consulted by jni_fake.c before it falls back to a logged default.
// Each returns non-zero (or non-NULL) if it handled the call.
int         wmw_jni_numeric(const char *name, const char *sig, long *out);
int         wmw_jni_float  (const char *name, const char *sig, float *out);
const char *wmw_jni_string (const char *name, const char *sig);
int         wmw_jni_void   (const char *name, const char *sig);

// Loading progress the engine reports through setDisplayPercent(F)V.
extern volatile float wmw_load_percent;

// Set when the engine calls closeActivity() -- main.c exits the loop.
extern volatile int wmw_quit_requested;

#endif
