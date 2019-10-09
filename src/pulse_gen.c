/** @file
    tx_tools - pulse_gen, pulse data I/Q waveform generator.

    Copyright (C) 2019 by Christian Zuckschwerdt <zany@triq.net>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pulse_text.h"
#include "iq_render.h"
#include "common.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/types.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#ifdef _MSC_VER
#include "getopt/getopt.h"
#define F_OK 0
#endif
#endif
#ifndef _MSC_VER
#include <unistd.h>
#include <getopt.h>
#endif

#include <time.h>

#include "optparse.h"

static void print_version(void)
{
    fprintf(stderr, "pulse_gen version 0.1\n");
    fprintf(stderr, "Use -h for usage help and see https://triq.org/ for documentation.\n");
}

__attribute__((noreturn))
static void usage(int exitcode)
{
    fprintf(stderr,
            "\npulse_gen, pulse data I/Q waveform generator\n\n"
            "Usage:"
            "\t[-h] Output this usage help and exit\n"
            "\t[-V] Output the version string and exit\n"
            "\t[-v] Increase verbosity (can be used multiple times).\n"
            "\t[-s sample_rate (default: 2048000 Hz)]\n"
            "\t[-m OOK|ASK|FSK|PSK] preset mode defaults\n"
            "\t[-f|-F frequency Hz] set default mark|space frequency\n"
            "\t[-a|-A attenuation dB] set default mark|space attenuation\n"
            "\t[-p|-P phase deg] set default mark|space phase\n"
            "\t[-n noise floor dBFS or multiplier]\n"
            "\t[-N noise on signal dBFS or multiplier]\n"
            "\t Noise level < 0 for attenuation in dBFS, otherwise amplitude multiplier, 0 is off.\n"
            "\t[-g signal gain dBFS or multiplier]\n"
            "\t Gain level < 0 for attenuation in dBFS, otherwise amplitude multiplier, 0 is 0 dBFS.\n"
            "\t Levels as dbFS or multiplier are peak values, e.g. 0 dB or 1.0 x are equivalent to -3 dB RMS.\n"
            "\t[-b output_block_size (default: 16 * 16384) bytes]\n"
            "\t[-r file] read code from file ('-' reads from stdin)\n"
            "\t[-t pulse_text] parse given code text\n"
            "\t[-S rand_seed] set random seed for reproducible output\n"
            "\t[-M full_scale] limit the output full scale, e.g. use -F 2048 with CS16\n"
            "\t[-w file] write samples to file ('-' writes to stdout)\n\n");
    exit(exitcode);
}

#ifdef _WIN32
BOOL WINAPI
sighandler(int signum)
{
    if (CTRL_C_EVENT == signum) {
        fprintf(stderr, "Signal caught, exiting!\n");
        abort_render = 1;
        return TRUE;
    }
    return FALSE;
}
#else
static void sighandler(int signum)
{
    fprintf(stderr, "Signal caught, exiting!\n");
    abort_render = 1;
}
#endif

int main(int argc, char **argv)
{
    int verbosity = 0;

    char *wr_filename = NULL;

    iq_render_t spec = {0};
    iq_render_defaults(&spec);

    pulse_setup_t defaults = {0};
    pulse_setup_defaults(&defaults, "OOK");

    char *pulse_text = NULL;
    unsigned rand_seed = 1;

    print_version();

    int opt;
    while ((opt = getopt(argc, argv, "hVvs:m:f:F:a:A:p:P:n:N:g:b:r:w:t:M:S:")) != -1) {
        switch (opt) {
        case 'h':
            usage(0);
        case 'V':
            exit(0); // we already printed the version
        case 'v':
            verbosity++;
            break;
        case 's':
            spec.sample_rate = atodu_metric(optarg, "-s: ");
            break;
        case 'm':
            pulse_setup_defaults(&defaults, optarg);
            break;
        case 'f':
            defaults.freq_mark = atoi_metric(optarg, "-f: ");
            break;
        case 'a':
            defaults.att_mark = atoi_metric(optarg, "-a: ");
            break;
        case 'p':
            defaults.phase_mark = atoi_metric(optarg, "-p: ");
            break;
        case 'F':
            defaults.freq_space = atoi_metric(optarg, "-F: ");
            break;
        case 'A':
            defaults.att_space = atoi_metric(optarg, "-A: ");
            break;
        case 'P':
            defaults.phase_space = atoi_metric(optarg, "-P: ");
            break;
        case 'n':
            spec.noise_floor = atod_metric(optarg, "-n: ");
            break;
        case 'N':
            spec.noise_signal = atod_metric(optarg, "-N: ");
            break;
        case 'g':
            spec.gain = atod_metric(optarg, "-g: ");
            break;
        case 'b':
            spec.frame_size = atou_metric(optarg, "-b: ");
            break;
        case 'r':
            pulse_text = read_text_file(optarg);
            break;
        case 'w':
            wr_filename = optarg;
            break;
        case 't':
            pulse_text = strdup(optarg);
            break;
        case 'M':
            spec.full_scale = atof(optarg);
            break;
        case 'S':
            rand_seed = (unsigned)atoi(optarg);
            break;
        default:
            usage(1);
        }
    }

    if (argc > optind) {
        fprintf(stderr, "\nExtra arguments? \"%s\"...\n", argv[optind]);
        usage(1);
    }

    if (!pulse_text) {
        fprintf(stderr, "Input from stdin.\n");
        pulse_text = read_text_fd(fileno(stdin), "STDIN");
    }

    if (!wr_filename) {
        fprintf(stderr, "Output to stdout.\n");
        wr_filename = "-";
    }

    spec.sample_format = file_info(&wr_filename);
    if (verbosity)
        fprintf(stderr, "Output format %s.\n", sample_format_str(spec.sample_format));

    if (spec.frame_size < MINIMAL_BUF_LENGTH ||
            spec.frame_size > MAXIMAL_BUF_LENGTH) {
        fprintf(stderr, "Output block size wrong value, falling back to default\n");
        fprintf(stderr, "Minimal length: %u\n", MINIMAL_BUF_LENGTH);
        fprintf(stderr, "Maximal length: %u\n", MAXIMAL_BUF_LENGTH);
        spec.frame_size = DEFAULT_BUF_LENGTH;
    }

#ifndef _WIN32
    struct sigaction sigact;
    sigact.sa_handler = sighandler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGQUIT, &sigact, NULL);
    sigaction(SIGPIPE, &sigact, NULL);
#else
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)sighandler, TRUE);
#endif

    srand(rand_seed);

    tone_t *tones = parse_pulses(pulse_text, &defaults);

    if (verbosity > 1)
        output_pulses(tones);

    if (verbosity) {
        size_t length_us = iq_render_length_us(tones);
        size_t length_smp = iq_render_length_smp(&spec, tones);
        fprintf(stderr, "Signal length: %zu us, %zu smp\n\n", length_us, length_smp);
    }

    iq_render_file(wr_filename, &spec, tones);
    // void *buf;
    // size_t len;
    // iq_render_buf(&spec, tones, &buf, &len);
    // free(buf);

    free(tones);

    free(pulse_text);
}
