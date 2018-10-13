#include "globals.h"
#include "sql.h"
#include <unistd.h>
#include <string.h>

#define CLOCK_LOCK_BOOST 6 /* arbitrary */
#define CLOCK_LOCK_DECAY 1 /* arbitrary */
#define CLOCK_DETECTOR_AMPLITUDE_THRESHOLD 60 /* arbi4rary */
#define CLOCK_ERROR_BOUNDS 0.25

#define IAM 0xFC   /* start-of-track record */
#define IAM_LEN    4
#define IDAM 0xFE  /* sector header */
#define IDAM_LEN   10
#define DAM1 0xF8  /* sector data (type 1) */
#define DAM2 0xFB  /* sector data (type 2) */
#define DAM_LEN    6 /* plus user data */
/* Length of a DAM record is determined by the previous sector header. */

static const char* inputfilename = NULL;
static const char* outputfilename = NULL;
static bool verbose = false;
static sqlite3* indb;
static sqlite3* outdb;

static const uint8_t* inputbuffer;
static int inputlen;
static int cursor;
static int elapsed_ticks;

static int clock_period; /* mfm cell width */
static int period0; /* lower limit for a short transition */
static int period1; /* between short and medium transitions */
static int period2; /* between medium and long transitions */
static int period3; /* upper limit for a long transition */

static uint8_t outputbuffer[5*1024];
static int outputbufferpos;
static uint8_t fifo = 0;
static int bitcount = 0;
static bool phaselocked = false;
static bool phase = false;
static bool queued = false;
static bool has_queued = false;

static int thislength = 0;
static int nextlength = 0;

static void syntax_error(void)
{
    fprintf(stderr,
        "syntax: fluxclient mfmdecode <options>:\n"
        "  -i <filename>       input filename (.flux)\n"
        "  -o <filename>       output filename (.rec)\n"
        "  -v                  verbose decoding\n"
    );
    exit(1);
}

static char* const* parse_options(char* const* argv)
{
	for (;;)
	{
		switch (getopt(countargs(argv), argv, "+i:o:v"))
		{
			case -1:
				return argv + optind - 1;

			case 'i':
                inputfilename = optarg;
                break;

            case 'o':
                outputfilename = optarg;
                break;

            case 'v':
                verbose = true;
                break;

			default:
				syntax_error();
		}
	}
}

static void close_files(void)
{
    if (indb)
        sqlite3_close(indb);
    if (outdb)
        sqlite3_close(outdb);
}

static void open_files(void)
{
    indb = sql_open(inputfilename, SQLITE_OPEN_READONLY);
    outdb = sql_open(outputfilename, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    sql_prepare_record(outdb);
    atexit(close_files);
}

static void queue_bit(bool bit)
{
    fifo <<= 1;
    fifo |= bit;
    bitcount++;
}

static bool read_bit(void)
{
    if (has_queued)
    {
        has_queued = false;
        return queued;
    }

    /*
     * MFM divides the signal into two-bit cells, which can be either x0
     * (representing 0) and 01 (representing 1). x can be any value; the rules
     * set it to 0 of the previous cell contained 01, and 1 otherwise.
     * 
     * However, all we have are the intervals and so we don't know where a cell
     * begins. Consider:
     * 
     * Data:    1  1  0  0  0  1  0  0  1  1  1  0  1  0  1  0  1  0
     * Signal: 01 01 00 10 10 01 00 10 01 01 01 00 01 00 01 00 01 00
     * 
     * If our signal is offset half a cell, we get this:
     * 
     * Data:      0  0  1  1  0  0  1  0  0  0  0  0  0  0  0  0  0
     * Signal: 0 10 10 01 01 00 10 01 00 10 10 10 00 10 00 10 00 10 0
     * 
     * However! This violates the rules. We have 10 00. Both of these
     * encode zeros, which means the second 00 should have been a 10.
     * So, after a long transition, we know that the cell sequence
     * must have been (assuming correct encoding) 00 01, encoding 0
     * and 1, and that the next interval will be at the start of a
     * cell.
     * 
     * Data:      0  0  1  1  0  0  1  0  0  0  0  0 |  1  0  1  0  1  0
     * Signal: 0 10 10 01 01 00 10 01 00 10 10 10 00 | x1 00 01 00 01 00
     *                                               \ resync
     */

    if (cursor >= inputlen)
        return false;
    uint8_t t = inputbuffer[cursor++];
    elapsed_ticks += t;

    if ((t < period0) || (t > period3))
    {
        /* Garbage data: our clock's wrong. */
        phaselocked = false;
        return false;
    }
    else if (t < period1)
    {
        /* Short transition: either (1),01, or ,(1)0,1. */
        return !phase;
    }
    else if (t > period2)
    {
        /* Long transition: either (1),00,01 or ,(1)0,00,1.
         * The latter is illegal but occurs inside marker bytes. */

        if (!phase)
        {
            queued = true;
            has_queued = true;
            return false;
        }
        else
        {
            queued = false;
            has_queued = true;
            return false;
        }
    }
    else
    {
        /* Medium transition: either (1),00,1 or ,(1)0,01. */
        if (!phase)
        {
            phase = true;
            return false;
        }
        else
        {
            queued = true;
            has_queued = true;
            phase = false;
            return false;
        }
    }
}

static uint8_t read_byte(void)
{
    while (phaselocked && (bitcount < 8))
        queue_bit(read_bit());
    bitcount = 0;
    return fifo;
}

static void log_record(char type)
{
    if (verbose)
        printf("\n    % 8.3fms [0x%05x]: ",
            (double)elapsed_ticks / (TICKS_PER_US*1000.0), cursor);
    putchar(type);
}

static bool process_byte(uint8_t b)
{
    outputbuffer[outputbufferpos++] = b;
    if (outputbufferpos == sizeof(outputbuffer))
        goto abandon_record;

#if 0
    if (outputbufferpos < 4)
    {
        if ((b != 0xA1) && (b != 0xC2))
            goto abandon_record;
    }
    #endif

    if (outputbufferpos == 4)
    {
        switch (outputbuffer[3])
        {
            case IAM:
                thislength = IAM_LEN;
                log_record('T');
                break;

            case IDAM:
                thislength = IDAM_LEN;
                log_record('H');
                break;

            case DAM1:
            case DAM2:
                thislength = nextlength;
                nextlength = 0;
                /* Sector with a header? */
                if (thislength == 0)
                    goto abandon_record;
                log_record('D');
                break;

            default:
                goto abandon_record;
        }
    }

    if (outputbufferpos == thislength)
    {
        /* We've read a complete record. */

        if (verbose)
            printf(" %d bytes ", thislength);

        switch (outputbuffer[3])
        {
            case IDAM:
                if (verbose)
                    printf(" C%02d H%01d S%02d", outputbuffer[4], outputbuffer[5], outputbuffer[6]);
                nextlength = (1<<(outputbuffer[7] + 7)) + DAM_LEN;
                break;

            case DAM1:
            case DAM2:
                if (verbose)
                    printf(" (%d bytes user data)", thislength - DAM_LEN);
                break;
        }

        phaselocked = false;
        return true;
    }

    return false;

abandon_record:
    if (verbose && (outputbufferpos > 4))
        printf(" misread");
    phaselocked = false;
    return false;
}

static void decode_track_cb(int track, int side, const struct fluxmap* fluxmap)
{
    printf("Track %02d side %d: ", track, side);

    inputbuffer = fluxmap->intervals;
    inputlen = fluxmap->bytes;
    cursor = 0;
    elapsed_ticks = 0;
    int record = 0;

    while (cursor < inputlen)
    {
        if (!phaselocked)
        {
            while (cursor < inputlen)
            {
                /* Somewhere in the bitstream there'll be a sync sequence of 16 0 bytes
                 * followed by a special index byte, beginning with a 1 bit.
                 * 
                 * These zeroes will be encoded as 10 10 10 10..., so forming a nice
                 * simple signal that should be easy to detect. This routine scans the
                 * bitstream until it finds one of these windows, and sets the clock
                 * accordingly. Remember that the routine can be easily spoofed by bad
                 * data, so you need to check for the marker byte afterwards.
                 * 
                 * ...btw, the standard fill byte is 0x4e:
                 * 
                 *     0  1  0  0  1  1  1  0
                 *    10 01 00 10 01 01 01 00
                 * 
                 * That's four medium transitions vs two short ones. So we know we're going
                 * to miscalculate the clock the first time round.
                 */

                clock_period = fluxmap_seek_clock(fluxmap, &cursor, 16);

                /* 
                 * Okay, this looks good. We'll assume this is clock/2 --- 250kHz
                 * for HD floppies; this is one short transition. We're also going
                 * to assume that this was a 0 bit and set the phase, god help us.
                 */

                double short_time = clock_period;
                double medium_time = short_time * 1.5;
                double long_time = short_time * 2.0;

                period0 = short_time - short_time * CLOCK_ERROR_BOUNDS;
                period1 = (short_time + medium_time) / 2.0;
                period2 = (medium_time + long_time) / 2.0;
                period3 = long_time + long_time * CLOCK_ERROR_BOUNDS;
                phase = true;
                phaselocked = true;
                has_queued = false;

                while (phaselocked && (cursor < inputlen))
                {
                    if (read_bit())
                        goto found_byte;
                }
            }
        found_byte:;
            fifo = 1;
            bitcount = 1;
            outputbufferpos = 0;
        }

        if (process_byte(read_byte()))
        {
            sql_write_record(outdb, track, side, record, outputbuffer, outputbufferpos);
            record++;
        }
    }

    if (verbose)
        printf("\n    ");
    printf(" = %d records\n", record);
}

void cmd_mfmdecode(char* const* argv)
{
    argv = parse_options(argv);
    if (countargs(argv) != 1)
        syntax_error();
    if (!inputfilename)
        error("you must supply a filename to read from");
    if (!outputfilename)
        error("you must supply a filename to write to");

    open_files();
    sql_stmt(outdb, "BEGIN");
    sql_stmt(outdb, "DELETE FROM records");
    sql_for_all_flux_data(indb, decode_track_cb);
    sql_stmt(outdb, "COMMIT");
}