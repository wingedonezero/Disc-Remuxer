/* bindgen entry point for libdvdread-sys. The public surface of libdvdread
 * lives across several headers under <dvdread/...> — pull them all in so the
 * generated bindings cover the full IFO + sector + UDF surface. */
#include <dvdread/dvd_reader.h>
#include <dvdread/dvd_udf.h>
#include <dvdread/ifo_types.h>
#include <dvdread/ifo_read.h>
#include <dvdread/ifo_print.h>
#include <dvdread/nav_types.h>
#include <dvdread/nav_read.h>
#include <dvdread/nav_print.h>
#include <dvdread/bitreader.h>
