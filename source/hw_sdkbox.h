/* SDKBOX neutralisation. */

#ifndef HW_SDKBOX_H
#define HW_SDKBOX_H

int   hw_sdkbox_owns_class(const char *cls);
void *hw_sdkbox_class(const char *cls);
int   hw_sdkbox_is_object(const void *obj);
int   hw_sdkbox_query_bool(const char *method);

#endif
