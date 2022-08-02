/* gpssnmp - poll local gpsd for SNMP variables
 *
 * To build this:
 *     gcc -o gpssnmp gpssnmp.c -lgps
 *
 * Copyright 2016 David Taylor <gpsd@david.taylor.name>
 *
 * Copyright 2018 by the GPSD project
 * SPDX-License-Identifier: BSD-2-clause
 *
 */

#include "../include/gpsd_config.h"  // must be before all includes

#ifdef HAVE_GETOPT_LONG
   #include <getopt.h>
#endif
#include <math.h>                    // for isfinite()
#include <stdio.h>
#include <stdlib.h>
#include <string.h>                  // for strlcpy()

#include "../include/compiler.h"     // for FALLTHROUGH
#include "../include/gps.h"
#include "../include/gpsdclient.h"   // for gpsd_source_spec()
#include "../include/os_compat.h"    // for strlcpy() if needed

int debug = 0;                       // debug level
struct gps_data_t gpsdata_now, gpsdata;
int one = 1;                         // the one!
double snr_avg = 0;

typedef enum {
    t_double,
    t_dummy,      // used for non-terminal OIDs.
    t_sbyte,
    t_schort,
    t_sinteger,
    t_slongint,
    t_string,
    t_ubyte,
    t_uinteger,
    t_ulongint,
    t_ushort}
gpsdata_type;

struct oid_mib_xlate {
    const char *oid;            // this OID
    const char *short_mib;      // short MIB for this
    gpsdata_type type;          // the type of the value
    void *pval;                 // pointer the value
    int64_t scale;              // scale factor to convert to int32_t
    int64_t min;                // minimum value of scaled value
    const char *desc;           // description, for usage()
};

/* Keep this list sorted, so it can be "walked".
 * Sorted "Numerically", not "alphabetically".
 * For now we only handle the first device, so table OIDs, end in .1
 */
const struct oid_mib_xlate xlate[] = {
    // next three are "pirate" OIDs, deprecated
    {".1.3.6.1.2.1.25.1.31", NULL, t_sinteger, &gpsdata.satellites_visible,
     1, -9, ""},
    {".1.3.6.1.2.1.25.1.32", NULL, t_sinteger, &gpsdata.satellites_used,
     1, 0, ""},
    {".1.3.6.1.2.1.25.1.33", NULL, t_sinteger, &snr_avg, 1, 0, ""},
    // previous three are "pirate" OIDs, deprecated
    {".1.3.6.1.4.1.59054", "gpsd", t_dummy, NULL, 0, 0,
     "Anchor for GPSD-MIB"},
    // start sky
    {".1.3.6.1.4.1.59054.11", "sky", t_dummy, NULL, 0, 0,
     "Anchor for SKY"},
    // only handle one device, for now
    {".1.3.6.1.4.1.59054.11.1", "skyNumber", t_sinteger, &one, 1, -1,
     "The number of devices in the skyTable"},
    {".1.3.6.1.4.1.59054.11.2.1.1.1", "skyIndex", t_sinteger, &one, 1, 0,
     "skyTable Index"},
    {".1.3.6.1.4.1.59054.11.2.1.2.1", "skyPath", t_string, &gpsdata.dev.path,
     1, 0, "path for this device"},
    {".1.3.6.1.4.1.59054.11.2.1.3.1", "skynSat.1", t_sinteger,
     &gpsdata.satellites_visible, 1, -1,
     "Number of satellties seen"},
    {".1.3.6.1.4.1.59054.11.2.1.4.1", "skyuSat.1", t_sinteger,
     &gpsdata.satellites_used, 1, -1,
     "Number of satellties in use"},
    {".1.3.6.1.4.1.59054.11.2.1.5.1", "skySNRavg.1", t_double,
     &snr_avg, 100, 0,
     "Average SNR of all satellites in use."},
    // end sky
    // start tpv
    {".1.3.6.1.4.1.59054.13", "tpv", t_dummy, NULL, 0, 0,
     "Anchor for TPV"},
    {".1.3.6.1.4.1.59054.13.1", "tpvLeapSeconds", t_sinteger,
     &gpsdata.leap_seconds, 1, 1,
     ""},
    // only handle one device, for now
    {".1.3.6.1.4.1.59054.13.2", "tpvNumber", t_sinteger,
     &one, 1, 0,
     "The number of devices in the tpvTable"},
    {".1.3.6.1.4.1.59054.13.3.1.1.1", "tpvIndex", t_sinteger,
     &one, 1, 1, "tpvTable Index"},
    {".1.3.6.1.4.1.59054.13.3.1.2.1", "tpvPath", t_string,
     &gpsdata.dev.path, 1, 1, "path for this device"},
    {".1.3.6.1.4.1.59054.13.3.1.3.1", "tpvMode.1", t_sinteger,
     &gpsdata.fix.mode, 1, 0, "Fix Mode"},
    {".1.3.6.1.4.1.59054.13.3.1.4.1", "tpvStatus.1", t_sinteger,
     &gpsdata.fix.status, 1, 0, "Fix Status"},
    // why 1e7?  Because SNMP chokes on INTEGERS > 32 bits.
    {".1.3.6.1.4.1.59054.13.3.1.5.1", "tpvLatitude.1", t_double,
     &gpsdata.fix.latitude, 10000000LL, -900000000LL,
     "Latitude in degrees."},
    {".1.3.6.1.4.1.59054.13.3.1.6.1", "tpvLongitude.1", t_double,
     &gpsdata.fix.longitude, 10000000LL, -18010000000LL,
     "Longitude in degrees."},
    {".1.3.6.1.4.1.59054.13.3.1.7.1", "tpvAltHAE.1", t_double,
     &gpsdata.fix.altHAE, 10000, LONG_MIN,
     "Height above Ellipsoid, in meters."},
    {".1.3.6.1.4.1.59054.13.3.1.8.1", "tpvAltMSL.1", t_double,
     &gpsdata.fix.altMSL, 10000, LONG_MIN,
     "Height above MSL, in meters."},
    {".1.3.6.1.4.1.59054.13.3.1.9.1", "tpvClimb.1", t_double,
     &gpsdata.fix.climb, 10000, LONG_MIN,
     "CLimb rate in meters/second"},
    {".1.3.6.1.4.1.59054.13.3.1.10.1", "tpvTrack.1", t_double,
     &gpsdata.fix.track, 100000, -1,
     "True Track in degrees."},
    {".1.3.6.1.4.1.59054.13.3.1.11.1", "tpvSpeed.1", t_double,
     &gpsdata.fix.speed, 10000, -1,
     "Ground speed (2D) in meters/second."},
    {".1.3.6.1.4.1.59054.13.3.1.12.1", "tpvEpc.1", t_double,
     &gpsdata.fix.epc, 100000, -1,
     "Estimated climb error in meters / second."},
    {".1.3.6.1.4.1.59054.13.3.1.13.1", "tpvEpd.1", t_double,
     &gpsdata.fix.epd, 100000, -1,
     "Estimated track (direction) error in degrees."},
    {".1.3.6.1.4.1.59054.13.3.1.14.1", "tpvEph.1", t_double,
     &gpsdata.fix.eph, 100000, -1,
     "Estimated horizontal (2D) error in meters."},
    {".1.3.6.1.4.1.59054.13.3.1.15.1", "tpvEps.1", t_double,
     &gpsdata.fix.eps, 100000, -1,
     "Estimated speed (2d) error in meters / second."},
    {".1.3.6.1.4.1.59054.13.3.1.16.1", "tpvEpt.1", t_double,
     &gpsdata.fix.ept, 10000000, -1,
     "Estimated time in seconds."},
    {".1.3.6.1.4.1.59054.13.3.1.18.1", "tpvEpv.1", t_double,
     &gpsdata.fix.epv, 100000, -1,
     "Estimated vertical (altitude) error in meters."},
    {".1.3.6.1.4.1.59054.13.3.1.19.1", "tpvEpx.1", t_double,
     &gpsdata.fix.epx, 100000, -1,
     "Estimated longitude error in meters."},
    {".1.3.6.1.4.1.59054.13.3.1.20.1", "tpvEpy.1", t_double,
     &gpsdata.fix.epy, 100000, -1,
     "Estimated latitude error in meters."},
    // end tpv
    {NULL, NULL, t_sinteger, NULL, 0, 0, ""},
};

/* compare_oid() -- compare 2 oids, "numerically".
 *
 * even though they look alphanumeric, oids need to be compared
 * "numerically".
 *
 * so ".1.3.6.1.4.1.59054.13.3.1.9.1"
 * comes before ".1.3.6.1.4.1.59054.13.3.1.10.1"
 *
 * Return: 0 if oid1 == oid2
 *         negative  if oid1 < oid2
 *         positive  if oid1 > oid2
 *
 */
static long compare_oid(const char *oid1, const char *oid2)
{
    /* One could assume that in1 == in2, until the mismatch, but
     * someone add a leading zero and mess things up. */
    int in1 = 0;
    int in2 = 0;
    long part1, part2, ret = 0;

    while (1) {
        if ('.' != oid1[in1] ||
            '.' != oid2[in2]) {
            // legally, the only option is nul.
            ret = (long)oid1[in1] - (long)oid2[in2];
            break;
        }
        in1++;
        in2++;

        part1 = atol(&oid1[in1]);
        part2 = atol(&oid2[in2]);
        if (part1 != part2) {
            ret = part1 - part2;
            break;
        }
        // same, so far
        // scan to next period or nul
        while (1) {
            in1++;
            if ('\0' == oid1[in1] ||
                '.' == oid1[in1]) {
                break;
            }
        }
        while (1) {
            in2++;
            if ('\0' == oid2[in2] ||
                '.' == oid2[in2]) {
                break;
            }
        }
    }
    // debug:
    // (void)fprintf(stderr, "%s (%d) %s (%d) %ld\n\n",
    //               oid1, in1, oid2, in2, ret);
    return ret;
}

/* print usage info, then exit.
 *
 * Never returns
 */
static void usage(char *prog_name) {
    const struct oid_mib_xlate *pxlate;

    (void)printf("usage: %s [OPTIONS] [server[:port[:device]]]\n\n\
Options include: \n\
  -?, -h, --help            = help message\n\
                              Use with -D 1 to show possible OIDs\n\
                              Use with -D 2 to show scale factors\n\
  -D, --debug LVL           = set debug level to LVL, default 0 \n\
  -g, --get OID             = get value for OID\n\
  -n, --next OID            = next OID value\n\
  -V, --version             = emit version and exit.\n\n\
Examples:\n\n\
to get the number of saltellites seen with the OID\n\
   $ gpssnmp -g .1.3.6.1.4.1.59054.11.2.1.3.1\n\
   .1.3.6.1.4.1.59054.11.2.1.3.1\n\
   INTEGER\n\
   15\n\n\
to get the number of saltellites seen with the MIB name\n\
   $ gpssnmp -g skynSat.1\n\
   .1.3.6.1.4.1.59054.11.2.1.3.1\n\
   INTEGER\n\
   15\n\n",
        prog_name);

    if (0 >= debug) {
        // done, exit
        exit(0);
    }
    puts("Supported OIDs and their short names:\n");
    for (pxlate = xlate; NULL != pxlate->oid; pxlate++) {
        if (NULL == pxlate->short_mib) {
            // skip deprecated OIDs
            continue;
        }
        printf("   %-15s %-50s\n", pxlate->short_mib, pxlate->oid);
        if (2 > debug) {
            continue;
        }
        if ('\0' != pxlate->desc[0]) {
            printf("     Desc: %s\n", pxlate->desc);
        }
        if (t_sinteger == pxlate->type ||
            t_double == pxlate->type) {
            printf("     Scale: %ld\n", pxlate->scale);
        }
    }
    puts("");
    exit(0);
}

int main (int argc, char **argv)
{
    bool do_usage = false;
    bool get_next = false;
    int i;
    double snr_total = 0;
    int status;
    char oid[40] = "";       // requested get OID
    char noid[40] = "";      // requested next OID
    struct fixsource_t source;
    struct timespec ts_start, ts_now;
    const struct oid_mib_xlate *pxlate;
    long long value;         // (long long) so we get 64 bits on 32-bit CPUs.

    const char *optstring = "?D:g:hn:V";
#ifdef HAVE_GETOPT_LONG
    int option_index = 0;
    static struct option long_options[] = {
        {"debug", required_argument, NULL, 'D'},
        {"get", required_argument, NULL, 'g'},
        {"help", no_argument, NULL, 'h'},
        {"next", required_argument, NULL, 'n'},
        {"version", no_argument, NULL, 'V' },
        {NULL, 0, NULL, 0},
    };
#endif  // HAVE_GETOPT_LONG

    /* Process the options.  Print help if requested. */
    while (1) {
        int ch;
#ifdef HAVE_GETOPT_LONG
        ch = getopt_long(argc, argv, optstring, long_options, &option_index);
#else
        ch = getopt(argc, argv, optstring);
#endif

        if (-1 == ch) {
            break;
        }

        switch (ch) {
        case '?':
            FALLTHROUGH
        case 'h':
            do_usage = true;
            break;
        case 'D':
            debug = atoi(optarg);
            gps_enable_debug(debug, stderr);
            break;
        case 'g':
            strlcpy(oid, optarg, sizeof(oid));
            break;
        case 'n':
            strlcpy(noid, optarg, sizeof(noid));
            break;
        case 'V':
            (void)fprintf(stderr, "%s: %s (revision %s)\n",
                          argv[0], VERSION, REVISION);
            exit(EXIT_SUCCESS);
        default:
            (void)fprintf(stderr, "ERROR: Unknown option %c\n\n", ch);
            do_usage = true;
            break;
        }
    }

    if (do_usage) {
        usage(argv[0]);    // never returns
    }
    if ('\0' == oid[0] &&
        '\0' == noid[0]) {
        (void)fprintf(stderr, "%s: ERROR: Missing option -g or -n\n\n",
                      argv[0]);
        usage(argv[0]);
        exit(1);
    }

    if ('\0' != oid[0] &&
        '\0' != noid[0]) {
        (void)fprintf(stderr, "%s: ERROR: Use either -g or -n, not both\n\n",
                      argv[0]);
        usage(argv[0]);
        exit(1);
    }

    // Grok the server, port, and device
    if (optind < argc) {
        gpsd_source_spec(argv[optind], &source);
    } else {
        gpsd_source_spec(NULL, &source);
    }

    // Open the stream to gpsd
    status = gps_open(source.server, source.port, &gpsdata_now);
    if (0 != status) {
        (void)fprintf(stderr, "gpssnmp: ERROR: connection failed: %d\n",
                      status);
        exit(1);
    }
    // we want JSON
    (void)gps_stream(&gpsdata_now, WATCH_ENABLE | WATCH_JSON, NULL);

    clock_gettime(CLOCK_REALTIME, &ts_start);

    while (gps_waiting(&gpsdata_now, 5000000)) {
        gps_mask_t want = MODE_SET | SATELLITE_SET;

        // wait 10 seconds, tops.
        clock_gettime(CLOCK_REALTIME, &ts_now);
        // use llabs(), in case time went backwards...
        if (10 < llabs(ts_now.tv_sec - ts_start.tv_sec)) {
            // FIXME:  Make this configurable.
            // timeout
            (void)fputs("gpssnmp: ERROR: timeout", stderr);
            exit(1);
        }

        status = gps_read(&gpsdata_now, NULL, 0);
        if (-1 == status) {
            (void)fprintf(stderr, "gpssnmp: ERROR: read failed %d\n", status);
            exit(1);
        }
        if (!(TIME_SET & gpsdata_now.set)) {
            // not useful, save it
            gpsdata = gpsdata_now;
            continue;
        }
        if (3 <= debug) {
            (void)fprintf(stderr, "gpssnmp: time %lld %lld mask %s\n",
                          (long long)gpsdata_now.online.tv_sec,
                          (long long)gpsdata.online.tv_sec,
                          gps_maskdump(gpsdata_now.set));
        }
        if (want == (want & gpsdata_now.set)) {
            // got what we need
            // FIXME: wait for time change too.
            gpsdata = gpsdata_now;
            break;
        }
        gpsdata = gpsdata_now;
    }
    for(i = 0; i <= gpsdata.satellites_used; i++) {
        if (0 < gpsdata.skyview[i].used &&
            1 <  gpsdata.skyview[i].ss) {
            // printf("i: %d, P:%d, ss: %f\n", i, gpsdata.skyview[i].PRN,
            //         gpsdata.skyview[i].ss);
            snr_total+=gpsdata.skyview[i].ss;
        }
    }
    if (0 < gpsdata.satellites_used) {
        snr_avg = snr_total / gpsdata.satellites_used;
    }
    for (pxlate = xlate; NULL != pxlate->oid; pxlate++) {

        // Debug (void)fprintf(stderr, "gpssnmp: Trying %s\n", pxlate->oid);
        if ('\0' != oid[0]) {
            if (0 == compare_oid(pxlate->oid, oid) ||
                (NULL != pxlate->short_mib &&
                 0 == strncmp(pxlate->short_mib, oid, sizeof(oid)))) {
                // get match
            } else {
                continue;
            }
        } else if (get_next) {
            // this is the next one
        } else if ('\0' != noid[0]) {
             long cmp = compare_oid(pxlate->oid, noid);

             if (0 >= cmp) {
                // no match, yet.
                continue;
             }
             // got next match, numeric OID only
        }
        /* got match
         * The output here conforms to the requierments of the
         * "pass [-p priority] MIBOID PROG" option to snmpd.conf
         */

        switch (pxlate->type) {
        case t_dummy:
            // skip, go to next one
            get_next = true;
            continue;
        case t_double:
            // SNMP is too stupid to understand IEEE754, use scaled integers
            // SNMP chokes on INTEGER > 32 bits.
            if (isfinite(*(double *)pxlate->pval)) {
                value = (long)(*(double *)pxlate->pval * pxlate->scale);
                if (pxlate->min <= value) {
                    printf("%s\nINTEGER\n%lld\n", pxlate->oid, value);
                }
            } else {
                // skip, go to next one
                get_next = true;
            }
            break;
        case t_sinteger:
            // not scaled
            value = *(int *)pxlate->pval;
            if (pxlate->min <= value) {
                printf("%s\nINTEGER\n%lld\n", pxlate->oid, value);
            }
            break;
        case t_string:
            // 255 seems to be max STRING length.
            printf("%s\nSTRING\n%.255s\n", pxlate->oid,
                   (char *)pxlate->pval);
            break;
        default:
            (void)fprintf(stderr, "%s: ERROR: internal error, OID %s\n\n",
                          argv[0], oid);
            break;
        }
        break;
    }
    if (NULL == pxlate->oid) {
        // fell of the end of the list...
        (void)fprintf(stderr, "%s: ERROR: Unknown OID %s\n\n",
                      argv[0], oid);
        usage(argv[0]);
        exit(1);
    }
    gps_close (&gpsdata);

    exit(0);
}
