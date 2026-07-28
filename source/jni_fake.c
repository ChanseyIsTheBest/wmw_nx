/* jni_fake.c -- fake JNI environment for the Where's My Water? engine
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

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "wmw_jni.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'  heap object (freeable)
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'  pooled, never freed
  TAG_CLASS  = 0x434c5331, // 'CLS1'  singleton, never freed
};

typedef struct { uint32_t tag; char label[64]; } FakeObject;
typedef struct { uint32_t tag; char *utf; } FakeString;
typedef struct { uint32_t tag; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag; int len; int elem_size; void *data; } FakePriArray;
typedef struct { uint32_t tag; char name[64]; char sig[96]; } FakeID;

volatile int jni_quit_requested = 0;
volatile int g_kbd_requested = 0;

// ---------------------------------------------------------------------------
// local reference registry: native code that never returns to Java must free
// the refs it creates (or leak them). the engine brackets its JNI use with
// DeleteLocalRef / Push+PopLocalFrame, which we honour here.
// ---------------------------------------------------------------------------

#define MAX_LOCALS 16384
#define MAX_FRAMES 64
static void *locals[MAX_LOCALS];
static int locals_top = 0;
static int frames[MAX_FRAMES];
static int frame_top = 0;
static Mutex locals_lock;

static void *reg_local(void *ref) {
  if (ref) {
    mutexLock(&locals_lock);
    if (locals_top < MAX_LOCALS)
      locals[locals_top++] = ref;
    else
      debugPrintf("JNI: local ref table full, leaking %p\n", ref);
    mutexUnlock(&locals_lock);
  }
  return ref;
}

static void free_ref(void *ref) {
  if (!ref)
    return;
  switch (*(uint32_t *)ref) {
    case TAG_STRING: { FakeString *s = ref; free(s->utf); free(s); break; }
    case TAG_PRIARR: { FakePriArray *a = ref; free(a->data); free(a); break; }
    case TAG_OBJARR: { FakeObjArray *a = ref; free(a->items); free(a); break; }
    case TAG_OBJECT: free(ref); break;
    default: break; // TAG_ID / TAG_CLASS are never freed
  }
}

static void delete_local(void *ref) {
  if (!ref)
    return;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--) {
    if (locals[i] == ref) {
      locals[i] = locals[--locals_top];
      free_ref(ref);
      break;
    }
  }
  mutexUnlock(&locals_lock);
}

// ---------------------------------------------------------------------------
// fake object constructors (register as local refs)
// ---------------------------------------------------------------------------

void *jni_make_object(const char *label) {
  FakeObject *o = calloc(1, sizeof(*o));
  o->tag = TAG_OBJECT;
  strncpy(o->label, label ? label : "", sizeof(o->label) - 1);
  return reg_local(o);
}

void *jni_make_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(*s));
  s->tag = TAG_STRING;
  s->utf = strdup(utf ? utf : "");
  return reg_local(s);
}

static void *make_pri_array_adopt(void *data, int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = data;
  return reg_local(a);
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING)
    return s->utf;
  return "";
}

// singletons (never freed)
static FakeObject *g_class = NULL;
static FakeObject *g_thiz = NULL;

void *jni_make_thiz(void) {
  if (!g_thiz) {
    g_thiz = calloc(1, sizeof(*g_thiz));
    g_thiz->tag = TAG_CLASS;
    strcpy(g_thiz->label, "com/disney/common/BaseActivity");
  }
  return g_thiz;
}

static void *get_class(void) {
  if (!g_class) {
    g_class = calloc(1, sizeof(*g_class));
    g_class->tag = TAG_CLASS;
    strcpy(g_class->label, "com/disney/common/BaseActivity$Class");
  }
  return g_class;
}

// ---------------------------------------------------------------------------
// method/field ID pool
// ---------------------------------------------------------------------------

#define MAX_IDS 512
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig))
      return &id_pool[i];
  if (id_count >= MAX_IDS) {
    debugPrintf("JNI: id pool exhausted (%s)\n", name);
    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->name, name ? name : "", sizeof(id->name) - 1);
  strncpy(id->sig, sig ? sig : "", sizeof(id->sig) - 1);
  return id;
}

// ---------------------------------------------------------------------------
// bring-up helper: force a numeric/boolean return for a named method
// ---------------------------------------------------------------------------

#define MAX_FORCED 64
static struct { char name[64]; long value; } forced[MAX_FORCED];
static int forced_count = 0;

int jni_force_int_return(const char *method_name, long value) {
  if (!method_name || forced_count >= MAX_FORCED)
    return -1;
  strncpy(forced[forced_count].name, method_name, sizeof(forced[0].name) - 1);
  forced[forced_count].value = value;
  forced_count++;
  return 0;
}

static int forced_lookup(const char *name, long *out) {
  for (int i = 0; i < forced_count; i++)
    if (!strcmp(forced[i].name, name)) { *out = forced[i].value; return 1; }
  return 0;
}

// ---------------------------------------------------------------------------
// method dispatch: signature-aware safe defaults + logging
//
// We only ever see the engine -> Java direction here, and only for the thin
// platform-service layer (graphics/audio/file I/O are all native). Returning
// defaults keeps the engine running; the logs identify which methods, if any,
// actually need a real implementation for a given title.
// ---------------------------------------------------------------------------

static int sig_returns_string(const char *sig) {
  const char *p = strrchr(sig, ')');
  return p && strcmp(p + 1, "Ljava/lang/String;") == 0;
}

static void log_call(const char *kind, const FakeID *id) {
  static int n = 0;
  if (n < 4000) { // avoid unbounded spam, but cover the whole boot
    debugPrintf("JNI: %s %s%s -> default\n", kind, id->name, id->sig);
    n++;
  }
}

static juint call_numeric(const FakeID *id) {
  long v;
  if (forced_lookup(id->name, &v))
    return (juint)v;
  if (wmw_jni_numeric(id->name, id->sig, &v)) // WMW BaseActivity answers
    return (juint)v;
  log_call("num", id);
  return 0;
}

static float call_float(const FakeID *id) {
  float v = 0.0f;
  if (wmw_jni_float(id->name, id->sig, &v))
    return v;
  log_call("flt", id);
  return 0.0f;
}

static void *call_object(const FakeID *id) {
  const char *s = wmw_jni_string(id->name, id->sig); // WMW BaseActivity answers
  if (s)
    return jni_make_string(s);
  if (sig_returns_string(id->sig))
    return jni_make_string(""); // empty string is safer than null for the engine
  log_call("obj", id);
  return NULL;
}

static void call_void(const FakeID *id) {
  // Ads / cloud / analytics / social / IAP are all no-ops here; a handful of
  // WMW methods (quit, loading progress) are acted on in wmw_jni.c.
  if (wmw_jni_void(id->name, id->sig))
    return;
  log_call("void", id);
}

// ---------------------------------------------------------------------------
// JNIEnv function implementations
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }
static void *j_FindClass(void *env, const char *name) { (void)env; (void)name; return get_class(); }
static void *j_GetObjectClass(void *env, void *obj) { (void)env; (void)obj; return get_class(); }
static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls; return get_id(name, sig);
}

static void *j_NewGlobalRef(void *env, void *obj) {
  (void)env;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--)
    if (locals[i] == obj) { locals[i] = locals[--locals_top]; break; }
  mutexUnlock(&locals_lock);
  return obj;
}
static void j_DeleteGlobalRef(void *env, void *obj) { (void)env; free_ref(obj); }
static void j_DeleteLocalRef(void *env, void *obj) { (void)env; delete_local(obj); }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }
static juint j_EnsureLocalCapacity(void *env, int cap) { (void)env; (void)cap; return 0; }

static juint j_PushLocalFrame(void *env, int cap) {
  (void)env; (void)cap;
  mutexLock(&locals_lock);
  if (frame_top < MAX_FRAMES)
    frames[frame_top++] = locals_top;
  mutexUnlock(&locals_lock);
  return 0;
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  mutexLock(&locals_lock);
  const int mark = frame_top > 0 ? frames[--frame_top] : 0;
  for (int i = mark; i < locals_top; i++)
    if (locals[i] != result)
      free_ref(locals[i]);
  locals_top = mark;
  if (result && locals_top < MAX_LOCALS)
    locals[locals_top++] = result;
  mutexUnlock(&locals_lock);
  return result;
}

// --- Call<type>Method (instance + static share signature-aware dispatch) ----
// The variadic argument list is intentionally ignored: we return defaults, so
// there is nothing to parse, and an unread va_list needs no cleanup.

#define CALL_NUM(fn) \
  static juint fn(void *env, void *recv, FakeID *id, ...) { (void)env; (void)recv; return call_numeric(id); } \
  static juint fn##V(void *env, void *recv, FakeID *id, va_list va) { (void)env; (void)recv; (void)va; return call_numeric(id); }

#define CALL_OBJ(fn) \
  static void *fn(void *env, void *recv, FakeID *id, ...) { (void)env; (void)recv; return call_object(id); } \
  static void *fn##V(void *env, void *recv, FakeID *id, va_list va) { (void)env; (void)recv; (void)va; return call_object(id); }

// Float and double returns come back in s0/d0, NOT in x0 -- so they cannot go
// through call_numeric(). Omitting them entirely is worse than it looks: the
// engine asks BaseActivity for getDisplayWidthInMM()F / getDisplayHeightInMM()F
// during rendererInit and divides the pixel size by the answer to get a DPI.
// A garbage answer there produces a garbage scale factor and the engine then
// tries to allocate a buffer sized from it -- which surfaces, confusingly, as
// "RendererResize failed with exception: std::bad_alloc" rather than as
// anything to do with JNI.
#define CALL_FLT(fn) \
  static float fn(void *env, void *recv, FakeID *id, ...) { (void)env; (void)recv; return call_float(id); } \
  static float fn##V(void *env, void *recv, FakeID *id, va_list va) { (void)env; (void)recv; (void)va; return call_float(id); }

#define CALL_DBL(fn) \
  static double fn(void *env, void *recv, FakeID *id, ...) { (void)env; (void)recv; return call_float(id); } \
  static double fn##V(void *env, void *recv, FakeID *id, va_list va) { (void)env; (void)recv; (void)va; return call_float(id); }

CALL_OBJ(j_CallObjectMethod)
CALL_NUM(j_CallIntMethod)
CALL_NUM(j_CallBooleanMethod)
CALL_NUM(j_CallLongMethod)
CALL_FLT(j_CallFloatMethod)
CALL_DBL(j_CallDoubleMethod)

// The engine routes its internal debug logging through LogFromCPP(String); it
// carries useful info like which asset-resolution tier it selected ("Using
// HighRes graphics." etc). Surface it instead of swallowing it as a default.
static int try_log_from_cpp(const FakeID *id, void *jstr) {
  if (strcmp(id->name, "LogFromCPP") != 0)
    return 0;
  debugPrintf("CPP: %s\n", obj_str(jstr));
  return 1;
}

static void j_CallVoidMethod(void *env, void *recv, FakeID *id, ...) {
  (void)env; (void)recv;
  if (strcmp(id->name, "LogFromCPP") == 0) {
    va_list va; va_start(va, id);
    void *s = va_arg(va, void *); va_end(va);
    try_log_from_cpp(id, s);
    return;
  }
  if (strcmp(id->name, "ShowKeyboard") == 0) {
    va_list va; va_start(va, id);
    int show = va_arg(va, int); // first arg: show flag
    va_end(va);
    g_kbd_requested = show ? 1 : 0;
    debugPrintf("ShowKeyboard(show=%d)\n", show);
    return;
  }
  call_void(id);
}
static void j_CallVoidMethodV(void *env, void *recv, FakeID *id, va_list va) {
  (void)env; (void)recv;
  if (strcmp(id->name, "LogFromCPP") == 0) {
    void *s = va_arg(va, void *);
    try_log_from_cpp(id, s);
    return;
  }
  if (strcmp(id->name, "ShowKeyboard") == 0) {
    int show = va_arg(va, int);
    g_kbd_requested = show ? 1 : 0;
    debugPrintf("ShowKeyboard(show=%d)\n", show);
    return;
  }
  call_void(id);
}

// static variants reuse the same dispatchers
#define j_CallStaticObjectMethod   j_CallObjectMethod
#define j_CallStaticObjectMethodV  j_CallObjectMethodV
#define j_CallStaticIntMethod      j_CallIntMethod
#define j_CallStaticIntMethodV     j_CallIntMethodV
#define j_CallStaticBooleanMethod  j_CallBooleanMethod
#define j_CallStaticBooleanMethodV j_CallBooleanMethodV
#define j_CallStaticLongMethod     j_CallLongMethod
#define j_CallStaticLongMethodV    j_CallLongMethodV
#define j_CallStaticVoidMethod     j_CallVoidMethod
#define j_CallStaticVoidMethodV    j_CallVoidMethodV

// --- strings ----------------------------------------------------------------

static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }
static juint j_GetStringLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }

// --- arrays -----------------------------------------------------------------

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr; // len at the same offset in FakeObjArray/FakePriArray
  if (a && (a->tag == TAG_PRIARR || a->tag == TAG_OBJARR))
    return a->len;
  return 0;
}

static void *new_pri_array(int len, int elem_size) {
  void *data = calloc(len ? len : 1, elem_size);
  return make_pri_array_adopt(data, len, elem_size);
}
static void *j_NewByteArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewIntArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }

// --- direct byte buffers ----------------------------------------------------
// libfmodex.so's Java_org_fmod_FMODAudioDevice_fmodProcess() calls
// env->GetDirectBufferAddress(buffer) (JNIEnv slot 230) to reach the PCM
// destination. fmod_audio.c hands it a fake direct buffer wrapping our own
// mixing buffer, so no Java ByteBuffer ever exists.

void *jni_make_float_array(int len, float **out) {
  float *data = calloc(len ? len : 1, sizeof(float));
  if (out) *out = data;
  return make_pri_array_adopt(data, len, sizeof(float));
}

void jni_pin(void *ref) {
  if (!ref) return;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--) {
    if (locals[i] == ref) {
      locals[i] = locals[--locals_top];   // remove WITHOUT freeing
      break;
    }
  }
  mutexUnlock(&locals_lock);
}

void *jni_make_int_array(int len, int32_t **out) {
  int32_t *data = calloc(len ? len : 1, sizeof(int32_t));
  if (out) *out = data;
  return make_pri_array_adopt(data, len, sizeof(int32_t));
}

void *jni_make_direct_buffer(void *data, size_t len) {
  return make_pri_array_adopt(data, (int)len, 1);
}

static void *j_NewDirectByteBuffer(void *env, void *addr, int64_t cap) {
  (void)env;
  return make_pri_array_adopt(addr, (int)cap, 1);
}
static void *j_GetDirectBufferAddress(void *env, void *buf) {
  (void)env;
  FakePriArray *a = buf;
  return (a && a->tag == TAG_PRIARR) ? a->data : NULL;
}
static int64_t j_GetDirectBufferCapacity(void *env, void *buf) {
  (void)env;
  FakePriArray *a = buf;
  return (a && a->tag == TAG_PRIARR) ? a->len : -1;
}

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  return (a && a->tag == TAG_PRIARR) ? a->data : NULL;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + (size_t)start * a->elem_size, (size_t)len * a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + (size_t)start * a->elem_size, buf, (size_t)len * a->elem_size);
}

// --- misc -------------------------------------------------------------------

static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env; (void)cls; (void)methods;
  debugPrintf("JNI: RegisterNatives(%d) ignored\n", n);
  return 0;
}
extern void *fake_vm;
static juint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_void1(void *env) { (void)env; }
static juint j_unimplemented(void) {
  debugPrintf("JNI: unimplemented slot called\n");
  return 0;
}

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  mutexInit(&locals_lock);

  for (int i = 0; i < 233; i++)
    env_table[i] = (void *)j_unimplemented;

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteGlobalRef;
  env_table[23]  = (void *)j_DeleteLocalRef;
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[53]  = (void *)j_CallLongMethod;
  env_table[54]  = (void *)j_CallLongMethodV;
  env_table[55]  = (void *)j_CallFloatMethod;
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[58]  = (void *)j_CallDoubleMethod;
  env_table[59]  = (void *)j_CallDoubleMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  env_table[113] = (void *)j_GetMethodID;            // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[135] = (void *)j_CallFloatMethod;   // CallStaticFloatMethod
  env_table[136] = (void *)j_CallFloatMethodV;
  env_table[138] = (void *)j_CallDoubleMethod;  // CallStaticDoubleMethod
  env_table[139] = (void *)j_CallDoubleMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  env_table[144] = (void *)j_GetMethodID;            // GetStaticFieldID
  env_table[164] = (void *)j_GetStringLength;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[176] = (void *)j_NewByteArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[181] = (void *)j_NewFloatArray;
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[226] = (void *)j_NewGlobalRef;            // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteGlobalRef;         // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;
  env_table[229] = (void *)j_NewDirectByteBuffer;
  env_table[230] = (void *)j_GetDirectBufferAddress;
  env_table[231] = (void *)j_GetDirectBufferCapacity;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread;

  debugPrintf("JNI: fake environment initialized (env=%p vm=%p)\n", fake_env, fake_vm);
}
