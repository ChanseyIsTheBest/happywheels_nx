/* Cocos2d-x Java service handlers. */
#ifndef COCOS_JNI_H
#define COCOS_JNI_H

#include <stdarg.h>
#include <stdint.h>

void cocos_jni_init(const char *data_root);
int  cocos_owns_class(const char *cls);
int  cocos_owns_method(const char *name);

void    *cocos_dispatch_object(void *recv, const void *id, va_list va);
uint64_t cocos_dispatch_int   (void *recv, const void *id, va_list va);
float    cocos_dispatch_float (void *recv, const void *id, va_list va);
double   cocos_dispatch_double(void *recv, const void *id, va_list va);
void     cocos_dispatch_void  (void *recv, const void *id, va_list va);

extern void       *jni_make_string(const char *utf);
extern void       *jni_make_object(const char *label);
extern const char *jni_string_utf(void *jstr);
extern void       *jni_bytearray_data(void *arr, int *len_out);
extern void       *jni_make_byte_array(const void *src, int n);

#endif /* COCOS_JNI_H */
