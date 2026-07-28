/* wmw_entrypoints.h -- the exported JNI entry points of libwmw.so
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * libwmw.so exports 48 Java_* symbols plus JNI_OnLoad. They are *statically*
 * named (Java_<class>_<method>), so nothing goes through RegisterNatives --
 * every one is resolvable by name straight out of the dynamic symbol table.
 *
 * Two Java classes are involved:
 *
 *   com.disney.common.WMWRenderer  -- the GLSurfaceView.Renderer: init, resize,
 *                                     draw, context reload and the three touch
 *                                     callbacks. This is the whole game loop.
 *   com.disney.common.BaseActivity -- Android lifecycle plus a long tail of
 *                                     IAP / AMPS / cloud / social notifications
 *                                     that this port never needs to call.
 *
 * The signatures below are taken from the APK's classes.dex, which declares
 * every native method explicitly, and were cross-checked against the
 * disassembly of each entry point. Two of them are not what the disassembly
 * alone suggested, so the dex mattered:
 *
 *   - rendererInit takes FIVE Java arguments, not zero.
 *   - the touch id array is int[], not float[]  (confirmed: the third
 *     GetArrayElements call in rendererTouchBegan uses JNIEnv slot
 *     0x5d8/8 = 187 = GetIntArrayElements, while the first two use
 *     0x5e8/8 = 189 = GetFloatArrayElements).
 *   - rendererTouchMoved takes SIX arguments: it also wants the previous
 *     position of each contact.
 */

#ifndef __WMW_ENTRYPOINTS_H__
#define __WMW_ENTRYPOINTS_H__

#include <stdint.h>

// Every entry point takes the fake JNIEnv* and the fake activity/renderer
// object as its first two arguments, exactly as the Dalvik ABI would.
typedef void    (*fn_v)  (void *env, void *thiz);
typedef void    (*fn_ii) (void *env, void *thiz, int32_t a, int32_t b);
typedef void    (*fn_i)  (void *env, void *thiz, int32_t a);
typedef void    (*fn_z)  (void *env, void *thiz, int32_t b); // jboolean widens to int
typedef void    (*fn_fff)(void *env, void *thiz, float x, float y, float z);
typedef void   *(*fn_str)(void *env, void *thiz, void *jstr);
typedef void    (*fn_str_z)(void *env, void *thiz, void *jstr, int32_t b);
typedef int32_t (*fn_onload)(void *vm, void *reserved);

// rendererInit(String apkPath, String dataPath, Context ctx, String sku, float density)
//
// From WMWRenderer.onSurfaceCreated(), the Java caller passed:
//   apkPath  = ApplicationInfo.sourceDir      -- the APK itself
//   dataPath = getAppDataPath(packageName)    -- the writable data directory
//   ctx      = the Activity (a Context)
//   sku      = the "SKUMarket" manifest metadata, defaulting to "google"
//   density  = DisplayMetrics density
// The engine logs these as "PackagePath: %s" and "DataPath:    %s".
typedef void (*fn_init)(void *env, void *thiz, void *apkPath, void *dataPath,
                        void *context, void *skuMarket, float density);

// rendererTouchBegan / rendererTouchEnded (int count, float[] x, float[] y, int[] ids)
typedef void (*fn_touch)(void *env, void *thiz, int32_t count,
                         void *xs, void *ys, void *ids);

// rendererTouchMoved (int count, float[] x, float[] y, float[] prevX, float[] prevY, int[] ids)
typedef void (*fn_touch_moved)(void *env, void *thiz, int32_t count,
                               void *xs, void *ys, void *prev_xs, void *prev_ys,
                               void *ids);

typedef struct {
  fn_onload JNI_OnLoad;

  // --- WMWRenderer: the game loop ------------------------------------------
  fn_init        rendererInit;
  fn_ii          rendererResized;           // (width, height) -- first surface
  fn_ii          notifyScreenResized;       // (width, height) -- later changes
  fn_v           rendererDrawFrame;         // one frame; opens with glClear(0x4100)
  fn_v           rendererReloadContextData; // re-upload GL objects after context loss
  fn_touch       rendererTouchBegan;
  fn_touch_moved rendererTouchMoved;
  fn_touch       rendererTouchEnded;

  // --- BaseActivity: lifecycle ---------------------------------------------
  fn_v   onGamePause;
  fn_v   onGameResume;
  fn_v   onLostFocus;
  fn_v   onRegainedFocus;
  fn_v   backKeyPressed;
  fn_fff accelerometerChanged;   // no Switch accelerometer; optional gyro
  fn_str getLocalizedText;

  // --- BaseActivity: the ANSWER half of the request/notify pairs -----------
  // classes.dex declares these as public native on BaseActivity. Java called
  // them when an async platform operation completed; wmw_callbacks.c does the
  // same. Without them the engine waits forever on screens that ask a question.
  fn_z   notifyIAPAvailability;      // (Z)V
  fn_z   notifyReachability;         // (Z)V
  fn_z   notifyAMPSAvailability;     // (Z)V
  fn_str notifyProductInfoFailed;    // (Ljava/lang/String;)V
  fn_str notifyPurchaseFailed;       // (Ljava/lang/String;)V
  fn_str notifyPurchaseCancelled;    // (Ljava/lang/String;)V
  fn_i   notifyLOTWCountDown;        // (I)V
  fn_v     notifyEnableBackKey;      // ()V
  fn_str_z notifyPurchaseSuccess;    // (Ljava/lang/String;Z)V -- sku, isRestore

  // Optional: hands the engine's own FileManager the path to a main.obb.
  // Confirmed (JNIEnv*, jobject, jstring) -- calls JNIEnv slot 169
  // (GetStringUTFChars) on its third argument.
  fn_str notifyAddObbFilePathToFileManager;
} wmw_entry_points;

extern wmw_entry_points wmw;

#endif
