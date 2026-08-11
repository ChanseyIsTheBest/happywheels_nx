/* Minimal JNI environment for libcocos2dcpp. */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stdint.h>

extern void *fake_vm;
extern void *fake_env;

extern volatile int jni_quit_requested;

void jni_init(void);

/* Writes jni_unimplemented.txt next to the NRO if the game called any JNIEnv
 * slot this port does not implement. Empty run = nothing written. */
void jni_dump_unimplemented(const char *data_root);

void *jni_make_activity_object(void);

void *jni_make_string(const char *utf);
void *jni_make_object(const char *label);

void *jni_make_int_array(const int *src, int n);
void *jni_make_float_array(const float *src, int n);
void *jni_make_byte_array(const void *src, int n);

const char *jni_string_utf(void *jstr);

#endif
