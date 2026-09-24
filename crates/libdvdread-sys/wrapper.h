/* bindgen entry point for libdvdread-sys. The public surface of libdvdread
 * lives across several headers under <dvdread/...> — pull them all in so the
 * generated bindings cover the full IFO + sector + UDF surface.
 *
 * Deliberately NOT included: <dvdread/ifo_print.h> and
 * <dvdread/nav_print.h>. Those are libdvdread's debug pretty-printers
 * (used by the upstream `dvdinfo` example program). We don't call them —
 * our own `disc-remuxer info` subcommand prints the same data by reading
 * the parsed structs directly. Skipping these headers also keeps the
 * known cosmetic bug in `ifo_print.c`'s ifoPrint_C_ADT (uses
 * `sizeof(c_adt_t)` as the divisor for the cell-address-table entry
 * count) outside our binding surface entirely. */
#include <dvdread/dvd_reader.h>
#include <dvdread/dvd_udf.h>
#include <dvdread/ifo_types.h>
#include <dvdread/ifo_read.h>
#include <dvdread/nav_types.h>
#include <dvdread/nav_read.h>
#include <dvdread/bitreader.h>
