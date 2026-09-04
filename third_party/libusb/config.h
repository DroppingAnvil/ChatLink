/* Manual libusb config for MinGW-w64 / GCC (ChatLink vendored build). */
#ifndef LIBUSB_CHATLINK_CONFIG_H
#define LIBUSB_CHATLINK_CONFIG_H

/* Attribute for default symbol visibility (static lib: none needed). */
#define DEFAULT_VISIBILITY /**/

/* Enable message logging, but not verbose debug logging by default. */
#define ENABLE_LOGGING 1
/* #undef ENABLE_DEBUG_LOGGING */

/* Compiling for a Windows platform. */
#define PLATFORM_WINDOWS 1

/* GCC understands printf format checking. */
#define PRINTF_FORMAT(a, b) __attribute__ ((__format__ (__printf__, a, b)))

/* MinGW-w64 already declares struct timespec, and libusbi.h pulls it in via
   <sys/time.h>. Telling threads_windows.h so stops it defining a second one.
   Do NOT use _TIMESPEC_DEFINED here: that is the MSVC spelling and it
   suppresses libusb's definition without supplying the system one. */
#define HAVE_STRUCT_TIMESPEC 1

#endif /* LIBUSB_CHATLINK_CONFIG_H */
