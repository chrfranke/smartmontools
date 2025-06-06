/*
 * Home page of code is: https://www.smartmontools.org
 *
 * Copyright (C) 2002-11 Bruce Allen
 * Copyright (C) 2008-25 Christian Franke
 * Copyright (C) 2000    Michael Cornwell <cornwell@acm.org>
 * Copyright (C) 2008    Oliver Bock <brevilo@users.sourceforge.net>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"
#define __STDC_FORMAT_MACROS 1 // enable PRI* for C++

// unconditionally included files
#include <inttypes.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>   // umask
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <syslog.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <getopt.h>

#include <algorithm> // std::replace()
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// conditionally included files
#ifndef _WIN32
#include <sys/wait.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef _WIN32
#include "os_win32/popen.h" // popen_as_rstr_user(), pclose()
#ifdef _MSC_VER
#pragma warning(disable:4761) // "conversion supplied"
typedef unsigned short mode_t;
typedef int pid_t;
#endif
#include <io.h> // umask()
#include <process.h> // getpid()
#endif // _WIN32

#ifdef __CYGWIN__
#include <io.h> // setmode()
#endif // __CYGWIN__

#ifdef HAVE_LIBCAP_NG
#include <cap-ng.h>
#endif // LIBCAP_NG

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#endif // HAVE_LIBSYSTEMD

// locally included files
#include "atacmds.h"
#include "dev_interface.h"
#include "knowndrives.h"
#include "scsicmds.h"
#include "nvmecmds.h"
#include "utility.h"
#include "sg_unaligned.h"

#ifdef HAVE_POSIX_API
#include "popen_as_ugid.h"
#endif

#ifdef _WIN32
// fork()/signal()/initd simulation for native Windows
#include "os_win32/daemon_win32.h" // daemon_main/detach/signal()
#define strsignal daemon_strsignal
#define sleep     daemon_sleep
// SIGQUIT does not exist, CONTROL-Break signals SIGBREAK.
#define SIGQUIT SIGBREAK
#define SIGQUIT_KEYNAME "CONTROL-Break"
#else  // _WIN32
#define SIGQUIT_KEYNAME "CONTROL-\\"
#endif // _WIN32

const char * smartd_cpp_cvsid = "$Id: smartd.cpp 5696 2025-04-22 14:23:18Z chrfranke $"
  CONFIG_H_CVSID;

extern "C" {
  typedef void (*signal_handler_type)(int);
}

static void set_signal_if_not_ignored(int sig, signal_handler_type handler)
{
#if defined(_WIN32)
  // signal() emulation
  daemon_signal(sig, handler);

#else
  // SVr4, POSIX.1-2001, ..., POSIX.1-2024
  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sigaction(sig, (struct sigaction *)0, &sa);
  if (sa.sa_handler == SIG_IGN)
    return;

  sa = {};
  sa.sa_handler = handler;
  sa.sa_flags = SA_RESTART; // BSD signal() semantics
  sigaction(sig, &sa, (struct sigaction *)0);
#endif
}

using namespace smartmontools;

static const int scsiLogRespLen = 252;

// smartd exit codes
#define EXIT_BADCMD    1   // command line did not parse
#define EXIT_BADCONF   2   // syntax error in config file
#define EXIT_STARTUP   3   // problem forking daemon
#define EXIT_PID       4   // problem creating pid file
#define EXIT_NOCONF    5   // config file does not exist
#define EXIT_READCONF  6   // config file exists but cannot be read

#define EXIT_NOMEM     8   // out of memory
#define EXIT_BADCODE   10  // internal error - should NEVER happen

#define EXIT_BADDEV    16  // we can't monitor this device
#define EXIT_NODEV     17  // no devices to monitor

#define EXIT_SIGNAL    254 // abort on signal


// command-line: 1=debug mode, 2=print presets
static unsigned char debugmode = 0;

// command-line: how long to sleep between checks
static constexpr int default_checktime = 1800;
static int checktime = default_checktime;
static int checktime_min = 0; // Minimum individual check time, 0 if none

// command-line: name of PID file (empty for no pid file)
static std::string pid_file;

// command-line: path prefix of persistent state file, empty if no persistence.
static std::string state_path_prefix
#ifdef SMARTMONTOOLS_SAVESTATES
          = SMARTMONTOOLS_SAVESTATES
#endif
                                    ;

// command-line: path prefix of attribute log file, empty if no logs.
static std::string attrlog_path_prefix
#ifdef SMARTMONTOOLS_ATTRIBUTELOG
          = SMARTMONTOOLS_ATTRIBUTELOG
#endif
                                    ;

// configuration file name
static const char * configfile;
// configuration file "name" if read from stdin
static const char * const configfile_stdin = "<stdin>";
// path of alternate configuration file
static std::string configfile_alt;

// warning script file
static std::string warning_script;

#ifdef HAVE_POSIX_API
// run warning script as non-privileged user
static bool warn_as_user;
static uid_t warn_uid;
static gid_t warn_gid;
static std::string warn_uname, warn_gname;
#elif defined(_WIN32)
// run warning script as restricted user
static bool warn_as_restr_user;
#endif

// command-line: when should we exit?
enum quit_t {
  QUIT_NODEV, QUIT_NODEVSTARTUP, QUIT_NEVER, QUIT_ONECHECK,
  QUIT_SHOWTESTS, QUIT_ERRORS
};
static quit_t quit = QUIT_NODEV;
static bool quit_nodev0 = false;

// command-line; this is the default syslog(3) log facility to use.
static int facility=LOG_DAEMON;

#ifndef _WIN32
// command-line: fork into background?
static bool do_fork=true;
#endif

// TODO: This smartctl only variable is also used in some os_*.cpp
unsigned char failuretest_permissive = 0;

// set to one if we catch a USR1 (check devices now)
static volatile int caughtsigUSR1=0;

#ifdef _WIN32
// set to one if we catch a USR2 (toggle debug mode)
static volatile int caughtsigUSR2=0;
#endif

// set to one if we catch a HUP (reload config file). In debug mode,
// set to two, if we catch INT (also reload config file).
static volatile int caughtsigHUP=0;

// set to signal value if we catch INT, QUIT, or TERM
static volatile int caughtsigEXIT=0;

// This function prints either to stdout or to the syslog as needed.
static void PrintOut(int priority, const char *fmt, ...)
                     __attribute_format_printf(2, 3);

#ifdef HAVE_LIBSYSTEMD
// systemd notify support

static bool notify_enabled = false;
static bool notify_ready = false;

static inline void notify_init()
{
  if (!getenv("NOTIFY_SOCKET"))
    return;
  notify_enabled = true;
}

static inline bool notify_post_init()
{
  if (!notify_enabled)
    return true;
  if (do_fork) {
    PrintOut(LOG_CRIT, "Option -n (--no-fork) is required if 'Type=notify' is set.\n");
    return false;
  }
  return true;
}

static inline void notify_extend_timeout()
{
  if (!notify_enabled)
    return;
  if (notify_ready)
    return;
  const char * notify = "EXTEND_TIMEOUT_USEC=20000000"; // typical drive spinup time is 20s tops
  if (debugmode) {
    pout("sd_notify(0, \"%s\")\n", notify);
    return;
  }
  sd_notify(0, notify);
}

static void notify_msg(const char * msg, bool ready = false)
{
  if (!notify_enabled)
    return;
  if (debugmode) {
    pout("sd_notify(0, \"%sSTATUS=%s\")\n", (ready ? "READY=1\\n" : ""), msg);
    return;
  }
  sd_notifyf(0, "%sSTATUS=%s", (ready ? "READY=1\n" : ""), msg);
}

static void notify_check(int numdev)
{
  if (!notify_enabled)
    return;
  char msg[32];
  snprintf(msg, sizeof(msg), "Checking %d device%s ...",
           numdev, (numdev != 1 ? "s" : ""));
  notify_msg(msg);
}

static void notify_wait(time_t wakeuptime, int numdev)
{
  if (!notify_enabled)
    return;
  char ts[16] = ""; struct tm tmbuf;
  strftime(ts, sizeof(ts), "%H:%M:%S", time_to_tm_local(&tmbuf, wakeuptime));
  char msg[64];
  snprintf(msg, sizeof(msg), "Next check of %d device%s will start at %s",
           numdev, (numdev != 1 ? "s" : ""), ts);
  notify_msg(msg, !notify_ready); // first call notifies READY=1
  notify_ready = true;
}

static void notify_exit(int status)
{
  if (!notify_enabled)
    return;
  const char * msg;
  switch (status) {
    case 0:             msg = "Exiting ..."; break;
    case EXIT_BADCMD:   msg = "Error in command line (see SYSLOG)"; break;
    case EXIT_BADCONF: case EXIT_NOCONF:
    case EXIT_READCONF: msg = "Error in config file (see SYSLOG)"; break;
    case EXIT_BADDEV:   msg = "Unable to register a device (see SYSLOG)"; break;
    case EXIT_NODEV:    msg = "No devices to monitor"; break;
    default:            msg = "Error (see SYSLOG)"; break;
  }
  // Ensure that READY=1 is notified before 'exit(0)' because otherwise
  // systemd will report a service (protocol) failure
  notify_msg(msg, (!status && !notify_ready));
}

#else // HAVE_LIBSYSTEMD
// No systemd notify support

static inline bool notify_post_init()
{
#ifdef __linux__
  if (getenv("NOTIFY_SOCKET")) {
    PrintOut(LOG_CRIT, "This version of smartd was build without 'Type=notify' support.\n");
    return false;
  }
#endif
  return true;
}

static inline void notify_init() { }
static inline void notify_extend_timeout() { }
static inline void notify_msg(const char *) { }
static inline void notify_check(int) { }
static inline void notify_wait(time_t, int) { }
static inline void notify_exit(int) { }

#endif // HAVE_LIBSYSTEMD

// Email frequencies
enum class emailfreqs : unsigned char {
  unknown, once, always, daily, diminishing
};

// Attribute monitoring flags.
// See monitor_attr_flags below.
enum {
  MONITOR_IGN_FAILUSE = 0x01,
  MONITOR_IGNORE      = 0x02,
  MONITOR_RAW_PRINT   = 0x04,
  MONITOR_RAW         = 0x08,
  MONITOR_AS_CRIT     = 0x10,
  MONITOR_RAW_AS_CRIT = 0x20,
};

// Array of flags for each attribute.
class attribute_flags
{
public:
  bool is_set(int id, unsigned char flag) const
    { return (0 < id && id < (int)sizeof(m_flags) && (m_flags[id] & flag)); }

  void set(int id, unsigned char flags)
    {
      if (0 < id && id < (int)sizeof(m_flags))
        m_flags[id] |= flags;
    }

private:
  unsigned char m_flags[256]{};
};


/// Configuration data for a device. Read from smartd.conf.
/// Supports copy & assignment and is compatible with STL containers.
struct dev_config
{
  int lineno{};                           // Line number of entry in file
  std::string name;                       // Device name (with optional extra info)
  std::string dev_name;                   // Device name (plain, for SMARTD_DEVICE variable)
  std::string dev_type;                   // Device type argument from -d directive, empty if none
  std::string dev_idinfo;                 // Device identify info for warning emails and duplicate check
  std::string dev_idinfo_bc;              // Same without namespace id for duplicate check
  std::string state_file;                 // Path of the persistent state file, empty if none
  std::string attrlog_file;               // Path of the persistent attrlog file, empty if none
  int checktime{};                        // Individual check interval, 0 if none
  bool ignore{};                          // Ignore this entry
  bool id_is_unique{};                    // True if dev_idinfo is unique (includes S/N or WWN)
  bool smartcheck{};                      // Check SMART status
  uint8_t smartcheck_nvme{};              // Check these bits from NVMe Critical Warning byte
  bool usagefailed{};                     // Check for failed Usage Attributes
  bool prefail{};                         // Track changes in Prefail Attributes
  bool usage{};                           // Track changes in Usage Attributes
  bool selftest{};                        // Monitor number of selftest errors
  bool errorlog{};                        // Monitor number of ATA errors
  bool xerrorlog{};                       // Monitor number of ATA errors (Extended Comprehensive error log)
  bool offlinests{};                      // Monitor changes in offline data collection status
  bool offlinests_ns{};                   // Disable auto standby if in progress
  bool selfteststs{};                     // Monitor changes in self-test execution status
  bool selfteststs_ns{};                  // Disable auto standby if in progress
  bool permissive{};                      // Ignore failed SMART commands
  char autosave{};                        // 1=disable, 2=enable Autosave Attributes
  char autoofflinetest{};                 // 1=disable, 2=enable Auto Offline Test
  firmwarebug_defs firmwarebugs;          // -F directives from drivedb or smartd.conf
  bool ignorepresets{};                   // Ignore database of -v options
  bool showpresets{};                     // Show database entry for this device
  bool removable{};                       // Device may disappear (not be present)
  char powermode{};                       // skip check, if disk in idle or standby mode
  bool powerquiet{};                      // skip powermode 'skipping checks' message
  int powerskipmax{};                     // how many times can be check skipped
  unsigned char tempdiff{};               // Track Temperature changes >= this limit
  unsigned char tempinfo{}, tempcrit{};   // Track Temperatures >= these limits as LOG_INFO, LOG_CRIT+mail
  regular_expression test_regex;          // Regex for scheduled testing
  unsigned test_offset_factor{};          // Factor for staggering of scheduled tests

  // Configuration of email warning messages
  std::string emailcmdline;               // script to execute, empty if no messages
  std::string emailaddress;               // email address, or empty
  emailfreqs emailfreq{};                 // Send emails once, daily, diminishing
  bool emailtest{};                       // Send test email?

  // ATA ONLY
  int dev_rpm{};                          // rotation rate, 0 = unknown, 1 = SSD, >1 = HDD
  int set_aam{};                          // disable(-1), enable(1..255->0..254) Automatic Acoustic Management
  int set_apm{};                          // disable(-1), enable(2..255->1..254) Advanced Power Management
  int set_lookahead{};                    // disable(-1), enable(1) read look-ahead
  int set_standby{};                      // set(1..255->0..254) standby timer
  bool set_security_freeze{};             // Freeze ATA security
  int set_wcache{};                       // disable(-1), enable(1) write cache
  int set_dsn{};                          // disable(0x2), enable(0x1) DSN

  bool sct_erc_set{};                     // set SCT ERC to:
  unsigned short sct_erc_readtime{};      // ERC read time (deciseconds)
  unsigned short sct_erc_writetime{};     // ERC write time (deciseconds)

  unsigned char curr_pending_id{};        // ID of current pending sector count, 0 if none
  unsigned char offl_pending_id{};        // ID of offline uncorrectable sector count, 0 if none
  bool curr_pending_incr{}, offl_pending_incr{}; // True if current/offline pending values increase
  bool curr_pending_set{},  offl_pending_set{};  // True if '-C', '-U' set in smartd.conf

  attribute_flags monitor_attr_flags;     // MONITOR_* flags for each attribute

  ata_vendor_attr_defs attribute_defs;    // -v options

  // NVMe only
  unsigned nvme_err_log_max_entries{};    // size of error log
};

// Number of allowed mail message types
static const int SMARTD_NMAIL = 13;
// Type for '-M test' mails (state not persistent)
static const int MAILTYPE_TEST = 0;
// TODO: Add const or enum for all mail types.

struct mailinfo {
  int logged{};         // number of times an email has been sent
  time_t firstsent{};   // time first email was sent, as defined by time(2)
  time_t lastsent{};    // time last email was sent, as defined by time(2)
};

/// Persistent state data for a device.
struct persistent_dev_state
{
  unsigned char tempmin{}, tempmax{};     // Min/Max Temperatures

  unsigned char selflogcount{};           // total number of self-test errors
  uint64_t selfloghour{};                 // lifetime hours of last self-test error
                                          // (NVMe self-test log uses a 64 bit value)

  time_t scheduled_test_next_check{};     // Time of next check for scheduled self-tests

  uint64_t selective_test_last_start{};   // Start LBA of last scheduled selective self-test
  uint64_t selective_test_last_end{};     // End LBA of last scheduled selective self-test

  mailinfo maillog[SMARTD_NMAIL];         // log info on when mail sent

  // ATA ONLY
  int ataerrorcount{};                    // Total number of ATA errors

  // Persistent part of ata_smart_values:
  struct ata_attribute {
    unsigned char id{};
    unsigned char val{};
    unsigned char worst{}; // Byte needed for 'raw64' attribute only.
    uint64_t raw{};
    unsigned char resvd{};
  };
  ata_attribute ata_attributes[NUMBER_ATA_SMART_ATTRIBUTES];
  
  // SCSI ONLY

  struct scsi_error_counter_t {
    struct scsiErrorCounter errCounter{};
    unsigned char found{};
  };
  scsi_error_counter_t scsi_error_counters[3];

  struct scsi_nonmedium_error_t {
    struct scsiNonMediumError nme{};
    unsigned char found{};
  };
  scsi_nonmedium_error_t scsi_nonmedium_error;

  // NVMe only
  uint64_t nvme_err_log_entries{};

  // NVMe SMART/Health information: only the fields avail_spare,
  // percent_used and media_errors are persistent.
  nvme_smart_log nvme_smartval{};
};

/// Non-persistent state data for a device.
struct temp_dev_state
{
  bool must_write{};                      // true if persistent part should be written

  bool skip{};                            // skip during next check cycle
  time_t wakeuptime{};                    // next wakeup time, 0 if unknown or global

  bool not_cap_offline{};                 // true == not capable of offline testing
  bool not_cap_conveyance{};
  bool not_cap_short{};
  bool not_cap_long{};
  bool not_cap_selective{};

  unsigned char temperature{};            // last recorded Temperature (in Celsius)
  time_t tempmin_delay{};                 // time where Min Temperature tracking will start

  bool removed{};                         // true if open() failed for removable device

  bool powermodefail{};                   // true if power mode check failed
  int powerskipcnt{};                     // Number of checks skipped due to idle or standby mode
  int lastpowermodeskipped{};             // the last power mode that was skipped

  int attrlog_valid{};                    // nonzero if data is valid for protocol specific
                                          // attribute log: 1=ATA, 2=SCSI, 3=NVMe

  // SCSI ONLY
  // TODO: change to bool
  unsigned char SmartPageSupported{};     // has log sense IE page (0x2f)
  unsigned char TempPageSupported{};      // has log sense temperature page (0xd)
  unsigned char ReadECounterPageSupported{};
  unsigned char WriteECounterPageSupported{};
  unsigned char VerifyECounterPageSupported{};
  unsigned char NonMediumErrorPageSupported{};
  unsigned char SuppressReport{};         // minimize nuisance reports
  unsigned char modese_len{};             // mode sense/select cmd len: 0 (don't
                                          // know yet) 6 or 10
  // ATA ONLY
  uint64_t num_sectors{};                 // Number of sectors
  ata_smart_values smartval{};            // SMART data
  ata_smart_thresholds_pvt smartthres{};  // SMART thresholds
  bool offline_started{};                 // true if offline data collection was started

  // ATA and NVMe
  bool selftest_started{};                // true if self-test was started

  // NVMe only
  uint8_t selftest_op{};                  // last self-test operation
  uint8_t selftest_compl{};               // last self-test completion
};

/// Runtime state data for a device.
struct dev_state
: public persistent_dev_state,
  public temp_dev_state
{
  void update_persistent_state();
  void update_temp_state();
};

/// Container for configuration info for each device.
typedef std::vector<dev_config> dev_config_vector;

/// Container for state info for each device.
typedef std::vector<dev_state> dev_state_vector;

// Copy ATA attributes to persistent state.
void dev_state::update_persistent_state()
{
  for (int i = 0; i < NUMBER_ATA_SMART_ATTRIBUTES; i++) {
    const ata_smart_attribute & ta = smartval.vendor_attributes[i];
    ata_attribute & pa = ata_attributes[i];
    pa.id = ta.id;
    if (ta.id == 0) {
      pa.val = pa.worst = 0; pa.raw = 0;
      continue;
    }
    pa.val = ta.current;
    pa.worst = ta.worst;
    pa.raw =            ta.raw[0]
           | (          ta.raw[1] <<  8)
           | (          ta.raw[2] << 16)
           | ((uint64_t)ta.raw[3] << 24)
           | ((uint64_t)ta.raw[4] << 32)
           | ((uint64_t)ta.raw[5] << 40);
    pa.resvd = ta.reserv;
  }
}

// Copy ATA from persistent to temp state.
void dev_state::update_temp_state()
{
  for (int i = 0; i < NUMBER_ATA_SMART_ATTRIBUTES; i++) {
    const ata_attribute & pa = ata_attributes[i];
    ata_smart_attribute & ta = smartval.vendor_attributes[i];
    ta.id = pa.id;
    if (pa.id == 0) {
      ta.current = ta.worst = 0;
      memset(ta.raw, 0, sizeof(ta.raw));
      continue;
    }
    ta.current = pa.val;
    ta.worst = pa.worst;
    ta.raw[0] = (unsigned char) pa.raw;
    ta.raw[1] = (unsigned char)(pa.raw >>  8);
    ta.raw[2] = (unsigned char)(pa.raw >> 16);
    ta.raw[3] = (unsigned char)(pa.raw >> 24);
    ta.raw[4] = (unsigned char)(pa.raw >> 32);
    ta.raw[5] = (unsigned char)(pa.raw >> 40);
    ta.reserv = pa.resvd;
  }
}

// Convert 128 bit LE integer to uint64_t or its max value on overflow.
static uint64_t le128_to_uint64(const unsigned char (& val)[16])
{
  if (sg_get_unaligned_le64(val + 8))
    return ~(uint64_t)0;
  return sg_get_unaligned_le64(val);
}

// Convert uint64_t to 128 bit LE integer
static void uint64_to_le128(unsigned char (& destval)[16], uint64_t srcval)
{
  sg_put_unaligned_le64(0, destval + 8);
  sg_put_unaligned_le64(srcval, destval);
}

// Parse a line from a state file.
static bool parse_dev_state_line(const char * line, persistent_dev_state & state)
{
  static const regular_expression regex(
    "^ *"
     "((temperature-min)" // (1 (2)
     "|(temperature-max)" // (3)
     "|(self-test-errors)" // (4)
     "|(self-test-last-err-hour)" // (5)
     "|(scheduled-test-next-check)" // (6)
     "|(selective-test-last-start)" // (7)
     "|(selective-test-last-end)" // (8)
     "|(ata-error-count)"  // (9)
     "|(mail\\.([0-9]+)\\." // (10 (11)
       "((count)" // (12 (13)
       "|(first-sent-time)" // (14)
       "|(last-sent-time)" // (15)
       ")" // 12)
      ")" // 10)
     "|(ata-smart-attribute\\.([0-9]+)\\." // (16 (17)
       "((id)" // (18 (19)
       "|(val)" // (20)
       "|(worst)" // (21)
       "|(raw)" // (22)
       "|(resvd)" // (23)
       ")" // 18)
      ")" // 16)
     "|(nvme-err-log-entries)" // (24)
     "|(nvme-available-spare)" // (25)
     "|(nvme-percentage-used)" // (26)
     "|(nvme-media-errors)" // (27)
     ")" // 1)
     " *= *([0-9]+)[ \n]*$" // (28)
  );

  constexpr int nmatch = 1+28;
  regular_expression::match_range match[nmatch];
  if (!regex.execute(line, nmatch, match))
    return false;
  if (match[nmatch-1].rm_so < 0)
    return false;

  uint64_t val = strtoull(line + match[nmatch-1].rm_so, (char **)0, 10);

  int m = 1;
  if (match[++m].rm_so >= 0)
    state.tempmin = (unsigned char)val;
  else if (match[++m].rm_so >= 0)
    state.tempmax = (unsigned char)val;
  else if (match[++m].rm_so >= 0)
    state.selflogcount = (unsigned char)val;
  else if (match[++m].rm_so >= 0)
    state.selfloghour = val;
  else if (match[++m].rm_so >= 0)
    state.scheduled_test_next_check = (time_t)val;
  else if (match[++m].rm_so >= 0)
    state.selective_test_last_start = val;
  else if (match[++m].rm_so >= 0)
    state.selective_test_last_end = val;
  else if (match[++m].rm_so >= 0)
    state.ataerrorcount = (int)val;
  else if (match[m+=2].rm_so >= 0) {
    int i = atoi(line+match[m].rm_so);
    if (!(0 <= i && i < SMARTD_NMAIL))
      return false;
    if (i == MAILTYPE_TEST) // Don't suppress test mails
      return true;
    if (match[m+=2].rm_so >= 0)
      state.maillog[i].logged = (int)val;
    else if (match[++m].rm_so >= 0)
      state.maillog[i].firstsent = (time_t)val;
    else if (match[++m].rm_so >= 0)
      state.maillog[i].lastsent = (time_t)val;
    else
      return false;
  }
  else if (match[m+=5+1].rm_so >= 0) {
    int i = atoi(line+match[m].rm_so);
    if (!(0 <= i && i < NUMBER_ATA_SMART_ATTRIBUTES))
      return false;
    if (match[m+=2].rm_so >= 0)
      state.ata_attributes[i].id = (unsigned char)val;
    else if (match[++m].rm_so >= 0)
      state.ata_attributes[i].val = (unsigned char)val;
    else if (match[++m].rm_so >= 0)
      state.ata_attributes[i].worst = (unsigned char)val;
    else if (match[++m].rm_so >= 0)
      state.ata_attributes[i].raw = val;
    else if (match[++m].rm_so >= 0)
      state.ata_attributes[i].resvd = (unsigned char)val;
    else
      return false;
  }
  else if (match[m+=7].rm_so >= 0)
    state.nvme_err_log_entries = val;
  else if (match[++m].rm_so >= 0)
    state.nvme_smartval.avail_spare = val;
  else if (match[++m].rm_so >= 0)
    state.nvme_smartval.percent_used = val;
  else if (match[++m].rm_so >= 0)
    uint64_to_le128(state.nvme_smartval.media_errors, val);
  else
    return false;
  return true;
}

// Read a state file.
static bool read_dev_state(const char * path, persistent_dev_state & state)
{
  stdio_file f(path, "r");
  if (!f) {
    if (errno != ENOENT)
      pout("Cannot read state file \"%s\"\n", path);
    return false;
  }
#ifdef __CYGWIN__
  setmode(fileno(f), O_TEXT); // Allow files with \r\n
#endif

  persistent_dev_state new_state;
  int good = 0, bad = 0;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    const char * s = line + strspn(line, " \t");
    if (!*s || *s == '#')
      continue;
    if (!parse_dev_state_line(line, new_state))
      bad++;
    else
      good++;
  }

  if (bad) {
    if (!good) {
      pout("%s: format error\n", path);
      return false;
    }
    pout("%s: %d invalid line(s) ignored\n", path, bad);
  }

  // This sets the values missing in the file to 0.
  state = new_state;
  return true;
}

static void write_dev_state_line(FILE * f, const char * name, uint64_t val)
{
  if (val)
    fprintf(f, "%s = %" PRIu64 "\n", name, val);
}

static void write_dev_state_line(FILE * f, const char * name1, int id, const char * name2, uint64_t val)
{
  if (val)
    fprintf(f, "%s.%d.%s = %" PRIu64 "\n", name1, id, name2, val);
}

// Write a state file
static bool write_dev_state(const char * path, const persistent_dev_state & state)
{
  // Rename old "file" to "file~"
  std::string pathbak = path; pathbak += '~';
  unlink(pathbak.c_str());
  rename(path, pathbak.c_str());

  stdio_file f(path, "w");
  if (!f) {
    pout("Cannot create state file \"%s\"\n", path);
    return false;
  }

  fprintf(f, "# smartd state file\n");
  write_dev_state_line(f, "temperature-min", state.tempmin);
  write_dev_state_line(f, "temperature-max", state.tempmax);
  write_dev_state_line(f, "self-test-errors", state.selflogcount);
  write_dev_state_line(f, "self-test-last-err-hour", state.selfloghour);
  write_dev_state_line(f, "scheduled-test-next-check", state.scheduled_test_next_check);
  write_dev_state_line(f, "selective-test-last-start", state.selective_test_last_start);
  write_dev_state_line(f, "selective-test-last-end", state.selective_test_last_end);

  for (int i = 0; i < SMARTD_NMAIL; i++) {
    if (i == MAILTYPE_TEST) // Don't suppress test mails
      continue;
    const mailinfo & mi = state.maillog[i];
    if (!mi.logged)
      continue;
    write_dev_state_line(f, "mail", i, "count", mi.logged);
    write_dev_state_line(f, "mail", i, "first-sent-time", mi.firstsent);
    write_dev_state_line(f, "mail", i, "last-sent-time", mi.lastsent);
  }

  // ATA ONLY
  write_dev_state_line(f, "ata-error-count", state.ataerrorcount);

  for (int i = 0; i < NUMBER_ATA_SMART_ATTRIBUTES; i++) {
    const auto & pa = state.ata_attributes[i];
    if (!pa.id)
      continue;
    write_dev_state_line(f, "ata-smart-attribute", i, "id", pa.id);
    write_dev_state_line(f, "ata-smart-attribute", i, "val", pa.val);
    write_dev_state_line(f, "ata-smart-attribute", i, "worst", pa.worst);
    write_dev_state_line(f, "ata-smart-attribute", i, "raw", pa.raw);
    write_dev_state_line(f, "ata-smart-attribute", i, "resvd", pa.resvd);
  }

  // NVMe only
  write_dev_state_line(f, "nvme-err-log-entries", state.nvme_err_log_entries);
  write_dev_state_line(f, "nvme-available-spare", state.nvme_smartval.avail_spare);
  write_dev_state_line(f, "nvme-percentage-used", state.nvme_smartval.percent_used);
  write_dev_state_line(f, "nvme-media-errors", le128_to_uint64(state.nvme_smartval.media_errors));

  return true;
}

static void write_ata_attrlog(FILE * f, const dev_state & state)
{
  for (const auto & pa : state.ata_attributes) {
    if (!pa.id)
      continue;
    fprintf(f, "\t%d;%d;%" PRIu64 ";", pa.id, pa.val, pa.raw);
  }
}

static void write_scsi_attrlog(FILE * f, const dev_state & state)
{
  const struct scsiErrorCounter * ecp;
  const char * pageNames[3] = {"read", "write", "verify"};
  for (int k = 0; k < 3; ++k) {
    if ( !state.scsi_error_counters[k].found ) continue;
    ecp = &state.scsi_error_counters[k].errCounter;
     fprintf(f, "\t%s-corr-by-ecc-fast;%" PRIu64 ";"
       "\t%s-corr-by-ecc-delayed;%" PRIu64 ";"
       "\t%s-corr-by-retry;%" PRIu64 ";"
       "\t%s-total-err-corrected;%" PRIu64 ";"
       "\t%s-corr-algorithm-invocations;%" PRIu64 ";"
       "\t%s-gb-processed;%.3f;"
       "\t%s-total-unc-errors;%" PRIu64 ";",
       pageNames[k], ecp->counter[0],
       pageNames[k], ecp->counter[1],
       pageNames[k], ecp->counter[2],
       pageNames[k], ecp->counter[3],
       pageNames[k], ecp->counter[4],
       pageNames[k], (ecp->counter[5] / 1000000000.0),
       pageNames[k], ecp->counter[6]);
  }
  if(state.scsi_nonmedium_error.found && state.scsi_nonmedium_error.nme.gotPC0) {
    fprintf(f, "\tnon-medium-errors;%" PRIu64 ";", state.scsi_nonmedium_error.nme.counterPC0);
  }
  // write SCSI current temperature if it is monitored
  if (state.temperature)
    fprintf(f, "\ttemperature;%d;", state.temperature);
}

static void write_nvme_attrlog(FILE * f, const dev_state & state)
{
  const nvme_smart_log & s = state.nvme_smartval;
  // Names similar to smartctl JSON output with '-' instead of '_'
  fprintf(f,
    "\tcritical-warning;%d;"
    "\ttemperature;%d;"
    "\tavailable-spare;%d;"
    "\tavailable-spare-threshold;%d;"
    "\tpercentage-used;%d;"
    "\tdata-units-read;%" PRIu64 ";"
    "\tdata-units-written;%" PRIu64 ";"
    "\thost-reads;%" PRIu64 ";"
    "\thost-writes;%" PRIu64 ";"
    "\tcontroller-busy-time;%" PRIu64 ";"
    "\tpower-cycles;%" PRIu64 ";"
    "\tpower-on-hours;%" PRIu64 ";"
    "\tunsafe-shutdowns;%" PRIu64 ";"
    "\tmedia-errors;%" PRIu64 ";"
    "\tnum-err-log-entries;%" PRIu64 ";",
    s.critical_warning,
    (int)sg_get_unaligned_le16(s.temperature) - 273,
    s.avail_spare,
    s.spare_thresh,
    s.percent_used,
    le128_to_uint64(s.data_units_read),
    le128_to_uint64(s.data_units_written),
    le128_to_uint64(s.host_reads),
    le128_to_uint64(s.host_writes),
    le128_to_uint64(s.ctrl_busy_time),
    le128_to_uint64(s.power_cycles),
    le128_to_uint64(s.power_on_hours),
    le128_to_uint64(s.unsafe_shutdowns),
    le128_to_uint64(s.media_errors),
    le128_to_uint64(s.num_err_log_entries)
  );
}

// Write to the attrlog file
static bool write_dev_attrlog(const char * path, const dev_state & state)
{
  stdio_file f(path, "a");
  if (!f) {
    pout("Cannot create attribute log file \"%s\"\n", path);
    return false;
  }

  time_t now = time(nullptr);
  struct tm tmbuf, * tms = time_to_tm_local(&tmbuf, now);
  fprintf(f, "%d-%02d-%02d %02d:%02d:%02d;",
             1900+tms->tm_year, 1+tms->tm_mon, tms->tm_mday,
             tms->tm_hour, tms->tm_min, tms->tm_sec);

  switch (state.attrlog_valid) {
    case 1: write_ata_attrlog(f, state); break;
    case 2: write_scsi_attrlog(f, state); break;
    case 3: write_nvme_attrlog(f, state); break;
  }

  fprintf(f, "\n");
  return true;
}

// Write all state files. If write_always is false, don't write
// unless must_write is set.
static void write_all_dev_states(const dev_config_vector & configs,
                                 dev_state_vector & states,
                                 bool write_always = true)
{
  for (unsigned i = 0; i < states.size(); i++) {
    const dev_config & cfg = configs.at(i);
    if (cfg.state_file.empty())
      continue;
    dev_state & state = states[i];
    if (!write_always && !state.must_write)
      continue;
    if (!write_dev_state(cfg.state_file.c_str(), state))
      continue;
    state.must_write = false;
    if (write_always || debugmode)
      PrintOut(LOG_INFO, "Device: %s, state written to %s\n",
               cfg.name.c_str(), cfg.state_file.c_str());
  }
}

// Write to all attrlog files
static void write_all_dev_attrlogs(const dev_config_vector & configs,
                                   dev_state_vector & states)
{
  for (unsigned i = 0; i < states.size(); i++) {
    const dev_config & cfg = configs.at(i);
    if (cfg.attrlog_file.empty())
      continue;
    dev_state & state = states[i];
    if (!state.attrlog_valid)
      continue;
    write_dev_attrlog(cfg.attrlog_file.c_str(), state);
    state.attrlog_valid = 0;
    if (debugmode)
      PrintOut(LOG_INFO, "Device: %s, attribute log written to %s\n",
               cfg.name.c_str(), cfg.attrlog_file.c_str());
  }
}

extern "C" { // signal handlers require C-linkage

//  Note if we catch a SIGUSR1
static void USR1handler(int sig)
{
  if (SIGUSR1==sig)
    caughtsigUSR1=1;
  return;
}

#ifdef _WIN32
//  Note if we catch a SIGUSR2
static void USR2handler(int sig)
{
  if (SIGUSR2==sig)
    caughtsigUSR2=1;
  return;
}
#endif

// Note if we catch a HUP (or INT in debug mode)
static void HUPhandler(int sig)
{
  if (sig==SIGHUP)
    caughtsigHUP=1;
  else
    caughtsigHUP=2;
  return;
}

// signal handler for TERM, QUIT, and INT (if not in debug mode)
static void sighandler(int sig)
{
  if (!caughtsigEXIT)
    caughtsigEXIT=sig;
  return;
}

} // extern "C"

#ifdef HAVE_LIBCAP_NG
// capabilities(7) support

static int capabilities_mode /* = 0 */; // 1=enabled, 2=mail

static void capabilities_drop_now()
{
  if (!capabilities_mode)
    return;
  capng_clear(CAPNG_SELECT_BOTH);
  capng_updatev(CAPNG_ADD, (capng_type_t)(CAPNG_EFFECTIVE|CAPNG_PERMITTED),
    CAP_SYS_ADMIN, CAP_MKNOD, CAP_SYS_RAWIO, -1);
  if (warn_as_user && (warn_uid || warn_gid)) {
    // For popen_as_ugid()
    capng_updatev(CAPNG_ADD, (capng_type_t)(CAPNG_EFFECTIVE|CAPNG_PERMITTED),
      CAP_SETGID, CAP_SETUID, -1);
  }
  if (capabilities_mode > 1) {
    // For exim MTA
    capng_updatev(CAPNG_ADD, CAPNG_BOUNDING_SET,
      CAP_SETGID, CAP_SETUID, CAP_CHOWN, CAP_FOWNER, CAP_DAC_OVERRIDE, -1);
  }
  capng_apply(CAPNG_SELECT_BOTH);
}

static void capabilities_log_error_hint()
{
  if (!capabilities_mode)
    return;
  PrintOut(LOG_INFO, "If mail notification does not work with '--capabilities%s\n",
           (capabilities_mode == 1 ? "', try '--capabilities=mail'"
                                   : "=mail', please inform " PACKAGE_BUGREPORT));
}

#else // HAVE_LIBCAP_NG
// No capabilities(7) support

static inline void capabilities_drop_now() { }
static inline void capabilities_log_error_hint() { }

#endif // HAVE_LIBCAP_NG

// a replacement for setenv() which is not available on all platforms.
// Note that the string passed to putenv must not be freed or made
// invalid, since a pointer to it is kept by putenv(). This means that
// it must either be a static buffer or allocated off the heap. The
// string can be freed if the environment variable is redefined via
// another call to putenv(). There is no portable way to unset a variable
// with putenv(). So we manage the buffer in a static object.
// Using setenv() if available is not considered because some
// implementations may produce memory leaks.

class env_buffer
{
public:
  env_buffer() = default;
  env_buffer(const env_buffer &) = delete;
  void operator=(const env_buffer &) = delete;

  void set(const char * name, const char * value);
private:
  char * m_buf = nullptr;
};

void env_buffer::set(const char * name, const char * value)
{
  int size = strlen(name) + 1 + strlen(value) + 1;
  char * newbuf = new char[size];
  snprintf(newbuf, size, "%s=%s", name, value);

  if (putenv(newbuf))
    throw std::runtime_error("putenv() failed");

  // This assumes that the same NAME is passed on each call
  delete [] m_buf;
  m_buf = newbuf;
}

#define EBUFLEN 1024

static void MailWarning(const dev_config & cfg, dev_state & state, int which, const char *fmt, ...)
                        __attribute_format_printf(4, 5);

// If either address or executable path is non-null then send and log
// a warning email, or execute executable
static void MailWarning(const dev_config & cfg, dev_state & state, int which, const char *fmt, ...)
{
  // See if user wants us to send mail
  if (cfg.emailaddress.empty() && cfg.emailcmdline.empty())
    return;

  // Which type of mail are we sending?
  static const char * const whichfail[] = {
    "EmailTest",                  // 0
    "Health",                     // 1
    "Usage",                      // 2
    "SelfTest",                   // 3
    "ErrorCount",                 // 4
    "FailedHealthCheck",          // 5
    "FailedReadSmartData",        // 6
    "FailedReadSmartErrorLog",    // 7
    "FailedReadSmartSelfTestLog", // 8
    "FailedOpenDevice",           // 9
    "CurrentPendingSector",       // 10
    "OfflineUncorrectableSector", // 11
    "Temperature"                 // 12
  };
  STATIC_ASSERT(sizeof(whichfail) == SMARTD_NMAIL * sizeof(whichfail[0]));
  
  if (!(0 <= which && which < SMARTD_NMAIL)) {
    PrintOut(LOG_CRIT, "Internal error in MailWarning(): which=%d\n", which);
    return;
  }
  mailinfo * mail = state.maillog + which;

  // Calc current and next interval for warning reminder emails
  int days, nextdays;
  if (which == 0)
    days = nextdays = -1; // EmailTest
  else switch (cfg.emailfreq) {
    case emailfreqs::once:
      days = nextdays = -1; break;
    case emailfreqs::always:
      days = nextdays = 0; break;
    case emailfreqs::daily:
      days = nextdays = 1; break;
    case emailfreqs::diminishing:
      // 0, 1, 2, 3, 4, 5, 6, 7, ... => 1, 2, 4, 8, 16, 32, 32, 32, ...
      nextdays = 1 << ((unsigned)mail->logged <= 5 ? mail->logged : 5);
      // 0, 1, 2, 3, 4, 5, 6, 7, ... => 0, 1, 2, 4,  8, 16, 32, 32, ... (0 not used below)
      days = ((unsigned)mail->logged <= 5 ? nextdays >> 1 : nextdays);
      break;
    default:
      PrintOut(LOG_CRIT, "Internal error in MailWarning(): cfg.emailfreq=%d\n", (int)cfg.emailfreq);
      return;
  }

  time_t now = time(nullptr);
  if (mail->logged) {
    // Return if no warning reminder email needs to be sent (now)
    if (days < 0)
      return; // '-M once' or EmailTest
    if (days > 0 && now < mail->lastsent + days * 24 * 3600)
      return; // '-M daily/diminishing' and too early
  }
  else {
    // Record the time of this first email message
    mail->firstsent = now;
  }

  // Record the time of this email message
  mail->lastsent = now;

  // print warning string into message
  // Note: Message length may reach ~300 characters as device names may be
  // very long on certain platforms (macOS ~230 characters).
  // Message length must not exceed email line length limit, see RFC 5322:
  // "... MUST be no more than 998 characters, ... excluding the CRLF."
  char message[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(message, sizeof(message), fmt, ap);
  va_end(ap);

  // replace commas by spaces to separate recipients
  std::string address = cfg.emailaddress;
  std::replace(address.begin(), address.end(), ',', ' ');

  // Export information in environment variables that will be useful
  // for user scripts
  const char * executable = cfg.emailcmdline.c_str();
  static env_buffer env[13];
  env[0].set("SMARTD_MAILER", executable);
  env[1].set("SMARTD_MESSAGE", message);
  char dates[DATEANDEPOCHLEN];
  snprintf(dates, sizeof(dates), "%d", mail->logged);
  env[2].set("SMARTD_PREVCNT", dates);
  dateandtimezoneepoch(dates, mail->firstsent);
  env[3].set("SMARTD_TFIRST", dates);
  snprintf(dates, DATEANDEPOCHLEN,"%d", (int)mail->firstsent);
  env[4].set("SMARTD_TFIRSTEPOCH", dates);
  env[5].set("SMARTD_FAILTYPE", whichfail[which]);
  env[6].set("SMARTD_ADDRESS", address.c_str());
  env[7].set("SMARTD_DEVICESTRING", cfg.name.c_str());

  // Allow 'smartctl ... -d $SMARTD_DEVICETYPE $SMARTD_DEVICE'
  env[8].set("SMARTD_DEVICETYPE",
             (!cfg.dev_type.empty() ? cfg.dev_type.c_str() : "auto"));
  env[9].set("SMARTD_DEVICE", cfg.dev_name.c_str());

  env[10].set("SMARTD_DEVICEINFO", cfg.dev_idinfo.c_str());
  dates[0] = 0;
  if (nextdays >= 0)
    snprintf(dates, sizeof(dates), "%d", nextdays);
  env[11].set("SMARTD_NEXTDAYS", dates);
  // Avoid false positive recursion detection by smartd_warning.{sh,cmd}
  env[12].set("SMARTD_SUBJECT", "");

  // now construct a command to send this as EMAIL
  if (!*executable)
    executable = "<mail>";
  const char * newadd = (!address.empty()? address.c_str() : "<nomailer>");
  const char * newwarn = (which? "Warning via" : "Test of");

  char command[256];
#ifdef _WIN32
  // Path may contain spaces
  snprintf(command, sizeof(command), "\"%s\" 2>&1", warning_script.c_str());
#else
  snprintf(command, sizeof(command), "%s 2>&1", warning_script.c_str());
#endif

  // tell SYSLOG what we are about to do...
  PrintOut(LOG_INFO,"%s %s to %s%s ...\n",
           (which ? "Sending warning via" : "Executing test of"), executable, newadd,
           (
#ifdef HAVE_POSIX_API
            warn_as_user ?
            strprintf(" (uid=%u(%s) gid=%u(%s))",
                      (unsigned)warn_uid, warn_uname.c_str(),
                      (unsigned)warn_gid, warn_gname.c_str() ).c_str() :
#elif defined(_WIN32)
            warn_as_restr_user ? " (restricted user)" :
#endif
            ""
           )
  );
  
  // issue the command to send mail or to run the user's executable
  errno=0;
  FILE * pfp;

#ifdef HAVE_POSIX_API
  if (warn_as_user) {
    pfp = popen_as_ugid(command, "r", warn_uid, warn_gid);
  } else
#endif
  {
#ifdef _WIN32
    pfp = popen_as_restr_user(command, "r", warn_as_restr_user);
#else
    pfp = popen(command, "r");
#endif
  }

  if (!pfp)
    // failed to popen() mail process
    PrintOut(LOG_CRIT,"%s %s to %s: failed (fork or pipe failed, or no memory) %s\n", 
             newwarn,  executable, newadd, errno?strerror(errno):"");
  else {
    // pipe succeeded!
    int len;
    char buffer[EBUFLEN];

    // if unexpected output on stdout/stderr, null terminate, print, and flush
    if ((len=fread(buffer, 1, EBUFLEN, pfp))) {
      int count=0;
      int newlen = len<EBUFLEN ? len : EBUFLEN-1;
      buffer[newlen]='\0';
      PrintOut(LOG_CRIT,"%s %s to %s produced unexpected output (%s%d bytes) to STDOUT/STDERR: \n%s\n", 
               newwarn, executable, newadd, len!=newlen?"here truncated to ":"", newlen, buffer);
      
      // flush pipe if needed
      while (fread(buffer, 1, EBUFLEN, pfp) && count<EBUFLEN)
        count++;

      // tell user that pipe was flushed, or that something is really wrong
      if (count && count<EBUFLEN)
        PrintOut(LOG_CRIT,"%s %s to %s: flushed remaining STDOUT/STDERR\n",
                 newwarn, executable, newadd);
      else if (count)
        PrintOut(LOG_CRIT,"%s %s to %s: more than 1 MB STDOUT/STDERR flushed, breaking pipe\n",
                 newwarn, executable, newadd);
    }
    
    // if something went wrong with mail process, print warning
    errno=0;
    int status;

#ifdef HAVE_POSIX_API
    if (warn_as_user) {
      status = pclose_as_ugid(pfp);
    } else
#endif
    {
      status = pclose(pfp);
    }

    if (status == -1)
      PrintOut(LOG_CRIT,"%s %s to %s: pclose(3) failed %s\n", newwarn, executable, newadd,
               errno?strerror(errno):"");
    else {
      // mail process apparently succeeded. Check and report exit status
      if (WIFEXITED(status)) {
        // exited 'normally' (but perhaps with nonzero status)
        int status8 = WEXITSTATUS(status);
        if (status8>128)
          PrintOut(LOG_CRIT,"%s %s to %s: failed (32-bit/8-bit exit status: %d/%d) perhaps caught signal %d [%s]\n",
                   newwarn, executable, newadd, status, status8, status8-128, strsignal(status8-128));
        else if (status8) {
          PrintOut(LOG_CRIT,"%s %s to %s: failed (32-bit/8-bit exit status: %d/%d)\n",
                   newwarn, executable, newadd, status, status8);
          capabilities_log_error_hint();
        }
        else
          PrintOut(LOG_INFO,"%s %s to %s: successful\n", newwarn, executable, newadd);
      }
      
      if (WIFSIGNALED(status))
        PrintOut(LOG_INFO,"%s %s to %s: exited because of uncaught signal %d [%s]\n",
                 newwarn, executable, newadd, WTERMSIG(status), strsignal(WTERMSIG(status)));
      
      // this branch is probably not possible. If subprocess is
      // stopped then pclose() should not return.
      if (WIFSTOPPED(status)) 
        PrintOut(LOG_CRIT,"%s %s to %s: process STOPPED because it caught signal %d [%s]\n",
                 newwarn, executable, newadd, WSTOPSIG(status), strsignal(WSTOPSIG(status)));
      
    }
  }

  // increment mail sent counter
  mail->logged++;
}

static void reset_warning_mail(const dev_config & cfg, dev_state & state, int which, const char *fmt, ...)
                               __attribute_format_printf(4, 5);

static void reset_warning_mail(const dev_config & cfg, dev_state & state, int which, const char *fmt, ...)
{
  if (!(0 <= which && which < SMARTD_NMAIL))
    return;

  // Return if no mail sent yet
  mailinfo & mi = state.maillog[which];
  if (!mi.logged)
    return;

  // Format & print message
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  PrintOut(LOG_INFO, "Device: %s, %s, warning condition reset after %d email%s\n", cfg.name.c_str(),
           msg, mi.logged, (mi.logged==1 ? "" : "s"));

  // Clear mail counter and timestamps
  mi = mailinfo();
  state.must_write = true;
}

#ifndef _WIN32

// Output multiple lines via separate syslog(3) calls.
__attribute_format_printf(2, 0)
static void vsyslog_lines(int priority, const char * fmt, va_list ap)
{
  char buf[512+EBUFLEN]; // enough space for exec cmd output in MailWarning()
  vsnprintf(buf, sizeof(buf), fmt, ap);

  for (char * p = buf, * q; p && *p; p = q) {
    if ((q = strchr(p, '\n')))
      *q++ = 0;
    if (*p)
      syslog(priority, "%s\n", p);
  }
}

#else  // _WIN32
// os_win32/syslog_win32.cpp supports multiple lines.
#define vsyslog_lines vsyslog
#endif // _WIN32

// Printing function for watching ataprint commands, or losing them
// [From GLIBC Manual: Since the prototype doesn't specify types for
// optional arguments, in a call to a variadic function the default
// argument promotions are performed on the optional argument
// values. This means the objects of type char or short int (whether
// signed or not) are promoted to either int or unsigned int, as
// appropriate.]
void pout(const char *fmt, ...){
  va_list ap;

  // get the correct time in syslog()
  FixGlibcTimeZoneBug();
  // initialize variable argument list 
  va_start(ap,fmt);
  // in debugmode==1 mode we will print the output from the ataprint.o functions!
  if (debugmode && debugmode != 2) {
    FILE * f = stdout;
#ifdef _WIN32
    if (facility == LOG_LOCAL1) // logging to stdout
      f = stderr;
#endif
    vfprintf(f, fmt, ap);
    fflush(f);
  }
  // in debugmode==2 mode we print output from knowndrives.o functions
  else if (debugmode==2 || ata_debugmode || scsi_debugmode) {
    openlog("smartd", LOG_PID, facility);
    vsyslog_lines(LOG_INFO, fmt, ap);
    closelog();
  }
  va_end(ap);
  return;
}

// This function prints either to stdout or to the syslog as needed.
static void PrintOut(int priority, const char *fmt, ...){
  va_list ap;
  
  // get the correct time in syslog()
  FixGlibcTimeZoneBug();
  // initialize variable argument list 
  va_start(ap,fmt);
  if (debugmode) {
    FILE * f = stdout;
#ifdef _WIN32
    if (facility == LOG_LOCAL1) // logging to stdout
      f = stderr;
#endif
    vfprintf(f, fmt, ap);
    fflush(f);
  }
  else {
    openlog("smartd", LOG_PID, facility);
    vsyslog_lines(priority, fmt, ap);
    closelog();
  }
  va_end(ap);
  return;
}

// Used to warn users about invalid checksums. Called from atacmds.cpp.
void checksumwarning(const char * string)
{
  pout("Warning! %s error: invalid SMART checksum.\n", string);
}

#ifndef _WIN32

// Wait for the pid file to show up, this makes sure a calling program knows
// that the daemon is really up and running and has a pid to kill it
static bool WaitForPidFile()
{
  int waited, max_wait = 10;
  struct stat stat_buf;

  if (pid_file.empty() || debugmode)
    return true;

  for(waited = 0; waited < max_wait; ++waited) {
    if (!stat(pid_file.c_str(), &stat_buf)) {
      return true;
    } else
      sleep(1);
  }
  return false;
}

#endif // _WIN32

// Forks new process if needed, closes ALL file descriptors,
// redirects stdin, stdout, and stderr.  Not quite daemon().
// See https://www.linuxjournal.com/article/2335
// for a good description of why we do things this way.
static int daemon_init()
{
#ifndef _WIN32

  // flush all buffered streams.  Else we might get two copies of open
  // streams since both parent and child get copies of the buffers.
  fflush(nullptr);

  if (do_fork) {
    pid_t pid;
    if ((pid=fork()) < 0) {
      // unable to fork!
      PrintOut(LOG_CRIT,"smartd unable to fork daemon process!\n");
      return EXIT_STARTUP;
    }
    if (pid) {
      // we are the parent process, wait for pid file, then exit cleanly
      if(!WaitForPidFile()) {
        PrintOut(LOG_CRIT,"PID file %s didn't show up!\n", pid_file.c_str());
        return EXIT_STARTUP;
      }
      return 0;
    }
  
    // from here on, we are the child process.
    setsid();

    // Fork one more time to avoid any possibility of having terminals
    if ((pid=fork()) < 0) {
      // unable to fork!
      PrintOut(LOG_CRIT,"smartd unable to fork daemon process!\n");
      return EXIT_STARTUP;
    }
    if (pid)
      // we are the parent process -- exit cleanly
      return 0;

    // Now we are the child's child...
  }

  // close any open file descriptors
  int open_max = sysconf(_SC_OPEN_MAX);
#ifdef HAVE_CLOSE_RANGE
  if (close_range(0, open_max - 1, 0))
#endif
  {
    // Limit number of unneeded close() calls under the assumption that
    // there are no large gaps between open FDs
    for (int i = 0, failed = 0; i < open_max && failed < 1024; i++)
      failed = (!close(i) ? 0 : failed + 1);
  }
  
  // redirect any IO attempts to /dev/null and change to root directory
  int fd = open("/dev/null", O_RDWR);
  if (!(fd == 0 && dup(fd) == 1 && dup(fd) == 2 && !chdir("/"))) {
    PrintOut(LOG_CRIT, "smartd unable to redirect to /dev/null or to chdir to root!\n");
    return EXIT_STARTUP;
  }
  umask(0022);

  if (do_fork)
    PrintOut(LOG_INFO, "smartd has fork()ed into background mode. New PID=%d.\n", (int)getpid());

#else // _WIN32

  // No fork() on native Win32
  // Detach this process from console
  fflush(nullptr);
  if (daemon_detach("smartd")) {
    PrintOut(LOG_CRIT,"smartd unable to detach from console!\n");
    return EXIT_STARTUP;
  }
  // stdin/out/err now closed if not redirected

#endif // _WIN32

  // No error, continue in main_worker()
  return -1;
}

// create a PID file containing the current process id
static bool write_pid_file()
{
  if (!pid_file.empty()) {
    pid_t pid = getpid();
    mode_t old_umask;
#ifndef __CYGWIN__
    old_umask = umask(0077); // rwx------
#else
    // Cygwin: smartd service runs on system account, ensure PID file can be read by admins
    old_umask = umask(0033); // rwxr--r--
#endif

    stdio_file f(pid_file.c_str(), "w");
    umask(old_umask);
    if (!(f && fprintf(f, "%d\n", (int)pid) > 0 && f.close())) {
      PrintOut(LOG_CRIT, "unable to write PID file %s - exiting.\n", pid_file.c_str());
      return false;
    }
    PrintOut(LOG_INFO, "file %s written containing PID %d\n", pid_file.c_str(), (int)pid);
  }
  return true;
}

// Prints header identifying version of code and home
static void PrintHead()
{
  PrintOut(LOG_INFO, "%s\n", format_version_info("smartd").c_str());
}

// prints help info for configuration file Directives
static void Directives()
{
  PrintOut(LOG_INFO,
           "Configuration file (%s) Directives (after device name):\n"
           "  -d TYPE Set the device type: auto, ignore, removable,\n"
           "          %s\n"
           "  -T TYPE Set the tolerance to one of: normal, permissive\n"
           "  -o VAL  Enable/disable automatic offline tests (on/off)\n"
           "  -S VAL  Enable/disable attribute autosave (on/off)\n"
           "  -n MODE No check if: never, sleep[,N][,q], standby[,N][,q], idle[,N][,q]\n"
           "  -H      Monitor SMART Health Status, report if failed\n"
           "  -H MASK Monitor specific NVMe Critical Warning bits\n"
           "  -s REG  Do Self-Test at time(s) given by regular expression REG\n"
           "  -l TYPE Monitor SMART log or self-test status:\n"
           "          error, selftest, xerror, offlinests[,ns], selfteststs[,ns]\n"
           "  -l scterc,R,W  Set SCT Error Recovery Control\n"
           "  -e      Change device setting: aam,[N|off], apm,[N|off], dsn,[on|off],\n"
           "          lookahead,[on|off], security-freeze, standby,[N|off], wcache,[on|off]\n"
           "  -f      Monitor 'Usage' Attributes, report failures\n"
           "  -m ADD  Send email warning to address ADD\n"
           "  -M TYPE Modify email warning behavior (see man page)\n"
           "  -p      Report changes in 'Prefailure' Attributes\n"
           "  -u      Report changes in 'Usage' Attributes\n"
           "  -t      Equivalent to -p and -u Directives\n"
           "  -r ID   Also report Raw values of Attribute ID with -p, -u or -t\n"
           "  -R ID   Track changes in Attribute ID Raw value with -p, -u or -t\n"
           "  -i ID   Ignore Attribute ID for -f Directive\n"
           "  -I ID   Ignore Attribute ID for -p, -u or -t Directive\n"
           "  -C ID[+] Monitor [increases of] Current Pending Sectors in Attribute ID\n"
           "  -U ID[+] Monitor [increases of] Offline Uncorrectable Sectors in Attribute ID\n"
           "  -W D,I,C Monitor Temperature D)ifference, I)nformal limit, C)ritical limit\n"
           "  -v N,ST Modifies labeling of Attribute N (see man page)  \n"
           "  -P TYPE Drive-specific presets: use, ignore, show, showall\n"
           "  -a      Default: -H -f -t -l error -l selftest -l selfteststs -C 197 -U 198\n"
           "  -F TYPE Use firmware bug workaround:\n"
           "          %s\n"
           "  -c i=N  Set interval between disk checks to N seconds\n"
           "   #      Comment: text after a hash sign is ignored\n"
           "   \\      Line continuation character\n"
           "Attribute ID is a decimal integer 1 <= ID <= 255\n"
           "Use ID = 0 to turn off -C and/or -U Directives\n"
           "Example: /dev/sda -a\n",
           configfile,
           smi()->get_valid_dev_types_str().c_str(),
           get_valid_firmwarebug_args());
}

/* Returns a pointer to a static string containing a formatted list of the valid
   arguments to the option opt or nullptr on failure. */
static const char *GetValidArgList(char opt)
{
  switch (opt) {
  case 'A':
  case 's':
    return "<PATH_PREFIX>, -";
  case 'B':
    return "[+]<FILE_NAME>";
  case 'c':
    return "<FILE_NAME>, -";
  case 'l':
    return "daemon, local0, local1, local2, local3, local4, local5, local6, local7";
  case 'q':
    return "nodev[0], errors[,nodev0], nodev[0]startup, never, onecheck, showtests";
  case 'r':
    return "ioctl[,N], ataioctl[,N], scsiioctl[,N], nvmeioctl[,N]";
  case 'p':
  case 'w':
    return "<FILE_NAME>";
  case 'i':
    return "<INTEGER_SECONDS>";
#ifdef HAVE_POSIX_API
  case 'u':
    return "<USER>[:<GROUP>], -";
#elif defined(_WIN32)
  case 'u':
    return "restricted, unchanged";
#endif
#ifdef HAVE_LIBCAP_NG
  case 'C':
    return "mail, <no_argument>";
#endif
  default:
    return nullptr;
  }
}

/* prints help information for command syntax */
static void Usage()
{
  PrintOut(LOG_INFO,"Usage: smartd [options]\n\n");
#ifdef SMARTMONTOOLS_ATTRIBUTELOG
  PrintOut(LOG_INFO,"  -A PREFIX|-, --attributelog=PREFIX|-\n");
#else
  PrintOut(LOG_INFO,"  -A PREFIX, --attributelog=PREFIX\n");
#endif
  PrintOut(LOG_INFO,"        Log attribute information to {PREFIX}MODEL-SERIAL.TYPE.csv\n");
#ifdef SMARTMONTOOLS_ATTRIBUTELOG
  PrintOut(LOG_INFO,"        [default is " SMARTMONTOOLS_ATTRIBUTELOG "MODEL-SERIAL.TYPE.csv]\n");
#endif
  PrintOut(LOG_INFO,"\n");
  PrintOut(LOG_INFO,"  -B [+]FILE, --drivedb=[+]FILE\n");
  PrintOut(LOG_INFO,"        Read and replace [add] drive database from FILE\n");
  PrintOut(LOG_INFO,"        [default is +%s", get_drivedb_path_add());
#ifdef SMARTMONTOOLS_DRIVEDBDIR
  PrintOut(LOG_INFO,"\n");
  PrintOut(LOG_INFO,"         and then    %s", get_drivedb_path_default());
#endif
  PrintOut(LOG_INFO,"]\n\n");
  PrintOut(LOG_INFO,"  -c NAME|-, --configfile=NAME|-\n");
  PrintOut(LOG_INFO,"        Read configuration file NAME or stdin\n");
  PrintOut(LOG_INFO,"        [default is %s]\n\n", configfile);
#ifdef HAVE_LIBCAP_NG
  PrintOut(LOG_INFO,"  -C, --capabilities[=mail]\n");
  PrintOut(LOG_INFO,"        Drop unneeded Linux process capabilities.\n"
                    "        Warning: Mail notification may not work when used.\n\n");
#endif
  PrintOut(LOG_INFO,"  -d, --debug\n");
  PrintOut(LOG_INFO,"        Start smartd in debug mode\n\n");
  PrintOut(LOG_INFO,"  -D, --showdirectives\n");
  PrintOut(LOG_INFO,"        Print the configuration file Directives and exit\n\n");
  PrintOut(LOG_INFO,"  -h, --help, --usage\n");
  PrintOut(LOG_INFO,"        Display this help and exit\n\n");
  PrintOut(LOG_INFO,"  -i N, --interval=N\n");
  PrintOut(LOG_INFO,"        Set interval between disk checks to N seconds, where N >= 10\n\n");
  PrintOut(LOG_INFO,"  -l local[0-7], --logfacility=local[0-7]\n");
#ifndef _WIN32
  PrintOut(LOG_INFO,"        Use syslog facility local0 - local7 or daemon [default]\n\n");
#else
  PrintOut(LOG_INFO,"        Log to \"./smartd.log\", stdout, stderr [default is event log]\n\n");
#endif
#ifndef _WIN32
  PrintOut(LOG_INFO,"  -n, --no-fork\n");
  PrintOut(LOG_INFO,"        Do not fork into background\n");
#ifdef HAVE_LIBSYSTEMD
  PrintOut(LOG_INFO,"        (systemd 'Type=notify' is assumed if $NOTIFY_SOCKET is set)\n");
#endif // HAVE_LIBSYSTEMD
  PrintOut(LOG_INFO,"\n");
#endif // WIN32
  PrintOut(LOG_INFO,"  -p NAME, --pidfile=NAME\n");
  PrintOut(LOG_INFO,"        Write PID file NAME\n\n");
  PrintOut(LOG_INFO,"  -q WHEN, --quit=WHEN\n");
  PrintOut(LOG_INFO,"        Quit on one of: %s\n\n", GetValidArgList('q'));
  PrintOut(LOG_INFO,"  -r, --report=TYPE\n");
  PrintOut(LOG_INFO,"        Report transactions for one of: %s\n\n", GetValidArgList('r'));
#ifdef SMARTMONTOOLS_SAVESTATES
  PrintOut(LOG_INFO,"  -s PREFIX|-, --savestates=PREFIX|-\n");
#else
  PrintOut(LOG_INFO,"  -s PREFIX, --savestates=PREFIX\n");
#endif
  PrintOut(LOG_INFO,"        Save disk states to {PREFIX}MODEL-SERIAL.TYPE.state\n");
#ifdef SMARTMONTOOLS_SAVESTATES
  PrintOut(LOG_INFO,"        [default is " SMARTMONTOOLS_SAVESTATES "MODEL-SERIAL.TYPE.state]\n");
#endif
  PrintOut(LOG_INFO,"\n");
  PrintOut(LOG_INFO,"  -w NAME, --warnexec=NAME\n");
  PrintOut(LOG_INFO,"        Run executable NAME on warnings\n");
#ifndef _WIN32
  PrintOut(LOG_INFO,"        [default is " SMARTMONTOOLS_SMARTDSCRIPTDIR "/smartd_warning.sh]\n\n");
#else
  PrintOut(LOG_INFO,"        [default is %s/smartd_warning.cmd]\n\n", get_exe_dir().c_str());
#endif
#ifdef HAVE_POSIX_API
  PrintOut(LOG_INFO,"  -u USER[:GROUP], --warn-as-user=USER[:GROUP]\n");
  PrintOut(LOG_INFO,"        Run warning script as non-privileged USER\n\n");
#elif defined(_WIN32)
  PrintOut(LOG_INFO,"  -u MODE, --warn-as-user=MODE\n");
  PrintOut(LOG_INFO,"        Run warning script with modified access token: %s\n\n", GetValidArgList('u'));
#endif
#ifdef _WIN32
  PrintOut(LOG_INFO,"  --service\n");
  PrintOut(LOG_INFO,"        Running as windows service (see man page), install with:\n");
  PrintOut(LOG_INFO,"          smartd install [options]\n");
  PrintOut(LOG_INFO,"        Remove service with:\n");
  PrintOut(LOG_INFO,"          smartd remove\n\n");
#endif // _WIN32
  PrintOut(LOG_INFO,"  -V, --version, --license, --copyright\n");
  PrintOut(LOG_INFO,"        Print License, Copyright, and version information\n");
}

static int CloseDevice(smart_device * device, const char * name)
{
  if (!device->close()){
    PrintOut(LOG_INFO,"Device: %s, %s, close() failed\n", name, device->get_errmsg());
    return 1;
  }
  // device successfully closed
  return 0;
}

// Replace invalid characters in cfg.dev_idinfo
static bool sanitize_dev_idinfo(std::string & s)
{
  bool changed = false;
  for (unsigned i = 0; i < s.size(); i++) {
    char c = s[i];
    STATIC_ASSERT(' ' == 0x20 && '~' == 0x07e); // Assume ASCII
    // Don't pass possible command escapes ('~! COMMAND') to the 'mail' command.
    if ((' ' <= c && c <= '~') && !(i == 0 && c == '~'))
      continue;
    s[i] = '?';
    changed = true;
  }
  return changed;
}

// return true if a char is not allowed in a state file name
static bool not_allowed_in_filename(char c)
{
  return !(   ('0' <= c && c <= '9')
           || ('A' <= c && c <= 'Z')
           || ('a' <= c && c <= 'z'));
}

// Read error count from Summary or Extended Comprehensive SMART error log
// Return -1 on error
static int read_ata_error_count(ata_device * device, const char * name,
                                firmwarebug_defs firmwarebugs, bool extended)
{
  if (!extended) {
    ata_smart_errorlog log;
    if (ataReadErrorLog(device, &log, firmwarebugs)){
      PrintOut(LOG_INFO,"Device: %s, Read Summary SMART Error Log failed\n",name);
      return -1;
    }
    return (log.error_log_pointer ? log.ata_error_count : 0);
  }
  else {
    ata_smart_exterrlog logx;
    if (!ataReadExtErrorLog(device, &logx, 0, 1 /*first sector only*/, firmwarebugs)) {
      PrintOut(LOG_INFO,"Device: %s, Read Extended Comprehensive SMART Error Log failed\n",name);
      return -1;
    }
    // Some disks use the reserved byte as index, see ataprint.cpp.
    return (logx.error_log_index || logx.reserved1 ? logx.device_error_count : 0);
  }
}

// Count error entries in ATA self-test log, set HOUR to power on hours of most
// recent error.  Return error count or -1 on failure.
static int check_ata_self_test_log(ata_device * device, const char * name,
                                   firmwarebug_defs firmwarebugs,
                                   unsigned & hour)
{
  struct ata_smart_selftestlog log;

  hour = 0;
  if (ataReadSelfTestLog(device, &log, firmwarebugs)){
    PrintOut(LOG_INFO,"Device: %s, Read SMART Self Test Log Failed\n",name);
    return -1;
  }

  if (!log.mostrecenttest)
    // No tests logged
    return 0;

  // Count failed self-tests
  int errcnt = 0;
  for (int i = 20; i >= 0; i--) {
    int j = (i + log.mostrecenttest) % 21;
    const ata_smart_selftestlog_struct & entry = log.selftest_struct[j];
    if (!nonempty(&entry, sizeof(entry)))
      continue;

    int status = entry.selfteststatus >> 4;
    if (status == 0x0 && (entry.selftestnumber & 0x7f) == 0x02)
      // First successful extended self-test, stop count
      break;

    if (0x3 <= status && status <= 0x8) {
      // Self-test showed an error
      errcnt++;
      // Keep track of time of most recent error
      if (!hour)
        hour = entry.timestamp;
    }
  }

  return errcnt;
}

// Check offline data collection status
static inline bool is_offl_coll_in_progress(unsigned char status)
{
  return ((status & 0x7f) == 0x03);
}

// Check self-test execution status
static inline bool is_self_test_in_progress(unsigned char status)
{
  return ((status >> 4) == 0xf);
}

// Log offline data collection status
static void log_offline_data_coll_status(const char * name, unsigned char status)
{
  const char * msg;
  switch (status & 0x7f) {
    case 0x00: msg = "was never started"; break;
    case 0x02: msg = "was completed without error"; break;
    case 0x03: msg = "is in progress"; break;
    case 0x04: msg = "was suspended by an interrupting command from host"; break;
    case 0x05: msg = "was aborted by an interrupting command from host"; break;
    case 0x06: msg = "was aborted by the device with a fatal error"; break;
    default:   msg = nullptr;
  }

  if (msg)
    PrintOut(((status & 0x7f) == 0x06 ? LOG_CRIT : LOG_INFO),
             "Device: %s, offline data collection %s%s\n", name, msg,
             ((status & 0x80) ? " (auto:on)" : ""));
  else
    PrintOut(LOG_INFO, "Device: %s, unknown offline data collection status 0x%02x\n",
             name, status);
}

// Log self-test execution status
static void log_self_test_exec_status(const char * name, unsigned char status)
{
  const char * msg;
  switch (status >> 4) {
    case 0x0: msg = "completed without error"; break;
    case 0x1: msg = "was aborted by the host"; break;
    case 0x2: msg = "was interrupted by the host with a reset"; break;
    case 0x3: msg = "could not complete due to a fatal or unknown error"; break;
    case 0x4: msg = "completed with error (unknown test element)"; break;
    case 0x5: msg = "completed with error (electrical test element)"; break;
    case 0x6: msg = "completed with error (servo/seek test element)"; break;
    case 0x7: msg = "completed with error (read test element)"; break;
    case 0x8: msg = "completed with error (handling damage?)"; break;
    default:  msg = nullptr;
  }

  if (msg)
    PrintOut(((status >> 4) >= 0x4 ? LOG_CRIT : LOG_INFO),
             "Device: %s, previous self-test %s\n", name, msg);
  else if ((status >> 4) == 0xf)
    PrintOut(LOG_INFO, "Device: %s, self-test in progress, %u0%% remaining\n",
             name, status & 0x0f);
  else
    PrintOut(LOG_INFO, "Device: %s, unknown self-test status 0x%02x\n",
             name, status);
}

// Check pending sector count id (-C, -U directives).
static bool check_pending_id(const dev_config & cfg, const dev_state & state,
                             unsigned char id, const char * msg)
{
  // Check attribute index
  int i = ata_find_attr_index(id, state.smartval);
  if (i < 0) {
    PrintOut(LOG_INFO, "Device: %s, can't monitor %s count - no Attribute %d\n",
             cfg.name.c_str(), msg, id);
    return false;
  }

  // Check value
  uint64_t rawval = ata_get_attr_raw_value(state.smartval.vendor_attributes[i],
    cfg.attribute_defs);
  if (rawval >= (state.num_sectors ? state.num_sectors : 0xffffffffULL)) {
    PrintOut(LOG_INFO, "Device: %s, ignoring %s count - bogus Attribute %d value %" PRIu64 " (0x%" PRIx64 ")\n",
             cfg.name.c_str(), msg, id, rawval, rawval);
    return false;
  }

  return true;
}

// Called by ATA/SCSI/NVMeDeviceScan() after successful device check
static void finish_device_scan(dev_config & cfg, dev_state & state)
{
  // Set cfg.emailfreq if user hasn't set it
  if ((!cfg.emailaddress.empty() || !cfg.emailcmdline.empty()) && cfg.emailfreq == emailfreqs::unknown) {
    // Avoid that emails are suppressed forever due to state persistence
    if (cfg.state_file.empty())
      cfg.emailfreq = emailfreqs::once;
    else
      cfg.emailfreq = emailfreqs::daily;
  }

  // Start self-test regex check now if time was not read from state file
  if (!cfg.test_regex.empty() && !state.scheduled_test_next_check)
    state.scheduled_test_next_check = time(nullptr);
}

// Common function to format result message for ATA setting
static void format_set_result_msg(std::string & msg, const char * name, bool ok,
                                  int set_option = 0, bool has_value = false)
{
  if (!msg.empty())
    msg += ", ";
  msg += name;
  if (!ok)
    msg += ":--";
  else if (set_option < 0)
    msg += ":off";
  else if (has_value)
    msg += strprintf(":%d", set_option-1);
  else if (set_option > 0)
    msg += ":on";
}

// Return true and print message if CFG.dev_idinfo is already in PREV_CFGS
static bool is_duplicate_dev_idinfo(const dev_config & cfg, const dev_config_vector & prev_cfgs)
{
  if (!cfg.id_is_unique)
    return false;

  for (const auto & prev_cfg : prev_cfgs) {
    if (!prev_cfg.id_is_unique)
      continue;
    if (!(    cfg.dev_idinfo == prev_cfg.dev_idinfo
          // Also check identity without NSID if device does not support multiple namespaces
          || (!cfg.dev_idinfo_bc.empty()      && cfg.dev_idinfo_bc == prev_cfg.dev_idinfo)
          || (!prev_cfg.dev_idinfo_bc.empty() && cfg.dev_idinfo == prev_cfg.dev_idinfo_bc)))
      continue;

    PrintOut(LOG_INFO, "Device: %s, same identity as %s, ignored\n",
             cfg.dev_name.c_str(), prev_cfg.dev_name.c_str());
    return true;
  }

  return false;
}

// TODO: Add '-F swapid' directive
const bool fix_swapped_id = false;

// scan to see what ata devices there are, and if they support SMART
static int ATADeviceScan(dev_config & cfg, dev_state & state, ata_device * atadev,
                         const dev_config_vector * prev_cfgs)
{
  int supported=0;
  struct ata_identify_device drive;
  const char *name = cfg.name.c_str();
  int retid;

  // Device must be open

  // Get drive identity structure
  if ((retid = ata_read_identity(atadev, &drive, fix_swapped_id))) {
    if (retid<0)
      // Unable to read Identity structure
      PrintOut(LOG_INFO,"Device: %s, not ATA, no IDENTIFY DEVICE Structure\n",name);
    else
      PrintOut(LOG_INFO,"Device: %s, packet devices [this device %s] not SMART capable\n",
               name, packetdevicetype(retid-1));
    CloseDevice(atadev, name);
    return 2; 
  }

  // Get drive identity, size and rotation rate (HDD/SSD)
  char model[40+1], serial[20+1], firmware[8+1];
  ata_format_id_string(model, drive.model, sizeof(model)-1);
  ata_format_id_string(serial, drive.serial_no, sizeof(serial)-1);
  ata_format_id_string(firmware, drive.fw_rev, sizeof(firmware)-1);

  ata_size_info sizes;
  ata_get_size_info(&drive, sizes);
  state.num_sectors = sizes.sectors;
  cfg.dev_rpm = ata_get_rotation_rate(&drive);

  char wwn[64]; wwn[0] = 0;
  unsigned oui = 0; uint64_t unique_id = 0;
  int naa = ata_get_wwn(&drive, oui, unique_id);
  if (naa >= 0)
    snprintf(wwn, sizeof(wwn), "WWN:%x-%06x-%09" PRIx64 ", ", naa, oui, unique_id);

  // Format device id string for warning emails
  char cap[32];
  cfg.dev_idinfo = strprintf("%s, S/N:%s, %sFW:%s, %s", model, serial, wwn, firmware,
                     format_capacity(cap, sizeof(cap), sizes.capacity, "."));
  cfg.id_is_unique = true; // TODO: Check serial?
  if (sanitize_dev_idinfo(cfg.dev_idinfo))
    cfg.id_is_unique = false;

  PrintOut(LOG_INFO, "Device: %s, %s\n", name, cfg.dev_idinfo.c_str());

  // Check for duplicates
  if (prev_cfgs && is_duplicate_dev_idinfo(cfg, *prev_cfgs)) {
    CloseDevice(atadev, name);
    return 1;
  }

  // Show if device in database, and use preset vendor attribute
  // options unless user has requested otherwise.
  if (cfg.ignorepresets)
    PrintOut(LOG_INFO, "Device: %s, smartd database not searched (Directive: -P ignore).\n", name);
  else {
    // Apply vendor specific presets, print warning if present
    std::string dbversion;
    const drive_settings * dbentry = lookup_drive_apply_presets(
      &drive, cfg.attribute_defs, cfg.firmwarebugs, dbversion);
    if (!dbentry)
      PrintOut(LOG_INFO, "Device: %s, not found in smartd database%s%s.\n", name,
        (!dbversion.empty() ? " " : ""), (!dbversion.empty() ? dbversion.c_str() : ""));
    else {
      PrintOut(LOG_INFO, "Device: %s, found in smartd database%s%s%s%s\n",
        name, (!dbversion.empty() ? " " : ""), (!dbversion.empty() ? dbversion.c_str() : ""),
        (*dbentry->modelfamily ? ": " : "."), (*dbentry->modelfamily ? dbentry->modelfamily : ""));
      if (*dbentry->warningmsg)
        PrintOut(LOG_CRIT, "Device: %s, WARNING: %s\n", name, dbentry->warningmsg);
    }
  }

  // Check for ATA Security LOCK
  unsigned short word128 = drive.words088_255[128-88];
  bool locked = ((word128 & 0x0007) == 0x0007); // LOCKED|ENABLED|SUPPORTED
  if (locked)
    PrintOut(LOG_INFO, "Device: %s, ATA Security is **LOCKED**\n", name);

  // Set default '-C 197[+]' if no '-C ID' is specified.
  if (!cfg.curr_pending_set)
    cfg.curr_pending_id = get_unc_attr_id(false, cfg.attribute_defs, cfg.curr_pending_incr);
  // Set default '-U 198[+]' if no '-U ID' is specified.
  if (!cfg.offl_pending_set)
    cfg.offl_pending_id = get_unc_attr_id(true, cfg.attribute_defs, cfg.offl_pending_incr);

  // If requested, show which presets would be used for this drive
  if (cfg.showpresets) {
    int savedebugmode=debugmode;
    PrintOut(LOG_INFO, "Device %s: presets are:\n", name);
    if (!debugmode)
      debugmode=2;
    show_presets(&drive);
    debugmode=savedebugmode;
  }

  // see if drive supports SMART
  supported=ataSmartSupport(&drive);
  if (supported!=1) {
    if (supported==0)
      // drive does NOT support SMART
      PrintOut(LOG_INFO,"Device: %s, lacks SMART capability\n",name);
    else
      // can't tell if drive supports SMART
      PrintOut(LOG_INFO,"Device: %s, ATA IDENTIFY DEVICE words 82-83 don't specify if SMART capable.\n",name);
  
    // should we proceed anyway?
    if (cfg.permissive) {
      PrintOut(LOG_INFO,"Device: %s, proceeding since '-T permissive' Directive given.\n",name);
    }
    else {
      PrintOut(LOG_INFO,"Device: %s, to proceed anyway, use '-T permissive' Directive.\n",name);
      CloseDevice(atadev, name);
      return 2;
    }
  }
  
  if (ataEnableSmart(atadev)) {
    // Enable SMART command has failed
    PrintOut(LOG_INFO,"Device: %s, could not enable SMART capability\n",name);

    if (ataIsSmartEnabled(&drive) <= 0) {
      if (!cfg.permissive) {
        PrintOut(LOG_INFO, "Device: %s, to proceed anyway, use '-T permissive' Directive.\n", name);
        CloseDevice(atadev, name);
        return 2;
      }
      PrintOut(LOG_INFO, "Device: %s, proceeding since '-T permissive' Directive given.\n", name);
    }
    else {
      PrintOut(LOG_INFO, "Device: %s, proceeding since SMART is already enabled\n", name);
    }
  }
  
  // disable device attribute autosave...
  if (cfg.autosave==1) {
    if (ataDisableAutoSave(atadev))
      PrintOut(LOG_INFO,"Device: %s, could not disable SMART Attribute Autosave.\n",name);
    else
      PrintOut(LOG_INFO,"Device: %s, disabled SMART Attribute Autosave.\n",name);
  }

  // or enable device attribute autosave
  if (cfg.autosave==2) {
    if (ataEnableAutoSave(atadev))
      PrintOut(LOG_INFO,"Device: %s, could not enable SMART Attribute Autosave.\n",name);
    else
      PrintOut(LOG_INFO,"Device: %s, enabled SMART Attribute Autosave.\n",name);
  }

  // capability check: SMART status
  if (cfg.smartcheck && ataSmartStatus2(atadev) == -1) {
    PrintOut(LOG_INFO,"Device: %s, not capable of SMART Health Status check\n",name);
    cfg.smartcheck = false;
  }
  
  // capability check: Read smart values and thresholds.  Note that
  // smart values are ALSO needed even if we ONLY want to know if the
  // device is self-test log or error-log capable!  After ATA-5, this
  // information was ALSO reproduced in the IDENTIFY DEVICE response,
  // but sadly not for ATA-5.  Sigh.

  // do we need to get SMART data?
  bool smart_val_ok = false;
  if (   cfg.autoofflinetest || cfg.selftest
      || cfg.errorlog        || cfg.xerrorlog
      || cfg.offlinests      || cfg.selfteststs
      || cfg.usagefailed     || cfg.prefail  || cfg.usage
      || cfg.tempdiff        || cfg.tempinfo || cfg.tempcrit
      || cfg.curr_pending_id || cfg.offl_pending_id         ) {

    if (ataReadSmartValues(atadev, &state.smartval)) {
      PrintOut(LOG_INFO, "Device: %s, Read SMART Values failed\n", name);
      cfg.usagefailed = cfg.prefail = cfg.usage = false;
      cfg.tempdiff = cfg.tempinfo = cfg.tempcrit = 0;
      cfg.curr_pending_id = cfg.offl_pending_id = 0;
    }
    else {
      smart_val_ok = true;
      if (ataReadSmartThresholds(atadev, &state.smartthres)) {
        PrintOut(LOG_INFO, "Device: %s, Read SMART Thresholds failed%s\n",
                 name, (cfg.usagefailed ? ", ignoring -f Directive" : ""));
        cfg.usagefailed = false;
        // Let ata_get_attr_state() return ATTRSTATE_NO_THRESHOLD:
        memset(&state.smartthres, 0, sizeof(state.smartthres));
      }
    }

    // see if the necessary Attribute is there to monitor offline or
    // current pending sectors or temperature
    if (   cfg.curr_pending_id
        && !check_pending_id(cfg, state, cfg.curr_pending_id,
              "Current_Pending_Sector"))
      cfg.curr_pending_id = 0;

    if (   cfg.offl_pending_id
        && !check_pending_id(cfg, state, cfg.offl_pending_id,
              "Offline_Uncorrectable"))
      cfg.offl_pending_id = 0;

    if (   (cfg.tempdiff || cfg.tempinfo || cfg.tempcrit)
        && !ata_return_temperature_value(&state.smartval, cfg.attribute_defs)) {
      PrintOut(LOG_INFO, "Device: %s, can't monitor Temperature, ignoring -W %d,%d,%d\n",
               name, cfg.tempdiff, cfg.tempinfo, cfg.tempcrit);
      cfg.tempdiff = cfg.tempinfo = cfg.tempcrit = 0;
    }

    // Report ignored '-r' or '-R' directives
    for (int id = 1; id <= 255; id++) {
      if (cfg.monitor_attr_flags.is_set(id, MONITOR_RAW_PRINT)) {
        char opt = (!cfg.monitor_attr_flags.is_set(id, MONITOR_RAW) ? 'r' : 'R');
        const char * excl = (cfg.monitor_attr_flags.is_set(id,
          (opt == 'r' ? MONITOR_AS_CRIT : MONITOR_RAW_AS_CRIT)) ? "!" : "");

        int idx = ata_find_attr_index(id, state.smartval);
        if (idx < 0)
          PrintOut(LOG_INFO,"Device: %s, no Attribute %d, ignoring -%c %d%s\n", name, id, opt, id, excl);
        else {
          bool prefail = !!ATTRIBUTE_FLAGS_PREFAILURE(state.smartval.vendor_attributes[idx].flags);
          if (!((prefail && cfg.prefail) || (!prefail && cfg.usage)))
            PrintOut(LOG_INFO,"Device: %s, not monitoring %s Attributes, ignoring -%c %d%s\n", name,
                     (prefail ? "Prefailure" : "Usage"), opt, id, excl);
        }
      }
    }
  }
  
  // enable/disable automatic on-line testing
  if (cfg.autoofflinetest) {
    // is this an enable or disable request?
    const char *what=(cfg.autoofflinetest==1)?"disable":"enable";
    if (!smart_val_ok)
      PrintOut(LOG_INFO,"Device: %s, could not %s SMART Automatic Offline Testing.\n",name, what);
    else {
      // if command appears unsupported, issue a warning...
      if (!isSupportAutomaticTimer(&state.smartval))
        PrintOut(LOG_INFO,"Device: %s, SMART Automatic Offline Testing unsupported...\n",name);
      // ... but then try anyway
      if ((cfg.autoofflinetest==1)?ataDisableAutoOffline(atadev):ataEnableAutoOffline(atadev))
        PrintOut(LOG_INFO,"Device: %s, %s SMART Automatic Offline Testing failed.\n", name, what);
      else
        PrintOut(LOG_INFO,"Device: %s, %sd SMART Automatic Offline Testing.\n", name, what);
    }
  }

  // Read log directories if required for capability check
  ata_smart_log_directory smart_logdir, gp_logdir;
  bool smart_logdir_ok = false, gp_logdir_ok = false;

  if (   isGeneralPurposeLoggingCapable(&drive)
      && (cfg.errorlog || cfg.selftest)
      && !cfg.firmwarebugs.is_set(BUG_NOLOGDIR)) {
      if (!ataReadLogDirectory(atadev, &smart_logdir, false))
        smart_logdir_ok = true;
  }

  if (cfg.xerrorlog && !cfg.firmwarebugs.is_set(BUG_NOLOGDIR)) {
    if (!ataReadLogDirectory(atadev, &gp_logdir, true))
      gp_logdir_ok = true;
  }

  // capability check: self-test-log
  state.selflogcount = 0; state.selfloghour = 0;
  if (cfg.selftest) {
    int errcnt = 0; unsigned hour = 0;
    if (!(   cfg.permissive
          || ( smart_logdir_ok && smart_logdir.entry[0x06-1].numsectors)
          || (!smart_logdir_ok && smart_val_ok && isSmartTestLogCapable(&state.smartval, &drive)))) {
      PrintOut(LOG_INFO, "Device: %s, no SMART Self-test Log, ignoring -l selftest (override with -T permissive)\n", name);
      cfg.selftest = false;
    }
    else if ((errcnt = check_ata_self_test_log(atadev, name, cfg.firmwarebugs, hour)) < 0) {
      PrintOut(LOG_INFO, "Device: %s, no SMART Self-test Log, ignoring -l selftest\n", name);
      cfg.selftest = false;
    }
    else {
      state.selflogcount = (unsigned char)errcnt;
      state.selfloghour  = hour;
    }
  }
  
  // capability check: ATA error log
  state.ataerrorcount = 0;
  if (cfg.errorlog) {
    int errcnt1;
    if (!(   cfg.permissive
          || ( smart_logdir_ok && smart_logdir.entry[0x01-1].numsectors)
          || (!smart_logdir_ok && smart_val_ok && isSmartErrorLogCapable(&state.smartval, &drive)))) {
      PrintOut(LOG_INFO, "Device: %s, no SMART Error Log, ignoring -l error (override with -T permissive)\n", name);
      cfg.errorlog = false;
    }
    else if ((errcnt1 = read_ata_error_count(atadev, name, cfg.firmwarebugs, false)) < 0) {
      PrintOut(LOG_INFO, "Device: %s, no SMART Error Log, ignoring -l error\n", name);
      cfg.errorlog = false;
    }
    else
      state.ataerrorcount = errcnt1;
  }

  if (cfg.xerrorlog) {
    int errcnt2;
    if (!(   cfg.permissive || cfg.firmwarebugs.is_set(BUG_NOLOGDIR)
          || (gp_logdir_ok && gp_logdir.entry[0x03-1].numsectors)   )) {
      PrintOut(LOG_INFO, "Device: %s, no Extended Comprehensive SMART Error Log, ignoring -l xerror (override with -T permissive)\n",
               name);
      cfg.xerrorlog = false;
    }
    else if ((errcnt2 = read_ata_error_count(atadev, name, cfg.firmwarebugs, true)) < 0) {
      PrintOut(LOG_INFO, "Device: %s, no Extended Comprehensive SMART Error Log, ignoring -l xerror\n", name);
      cfg.xerrorlog = false;
    }
    else if (cfg.errorlog && state.ataerrorcount != errcnt2) {
      PrintOut(LOG_INFO, "Device: %s, SMART Error Logs report different error counts: %d != %d\n",
               name, state.ataerrorcount, errcnt2);
      // Record max error count
      if (errcnt2 > state.ataerrorcount)
        state.ataerrorcount = errcnt2;
    }
    else
      state.ataerrorcount = errcnt2;
  }

  // capability check: self-test and offline data collection status
  if (cfg.offlinests || cfg.selfteststs) {
    if (!(cfg.permissive || (smart_val_ok && state.smartval.offline_data_collection_capability))) {
      if (cfg.offlinests)
        PrintOut(LOG_INFO, "Device: %s, no SMART Offline Data Collection capability, ignoring -l offlinests (override with -T permissive)\n", name);
      if (cfg.selfteststs)
        PrintOut(LOG_INFO, "Device: %s, no SMART Self-test capability, ignoring -l selfteststs (override with -T permissive)\n", name);
      cfg.offlinests = cfg.selfteststs = false;
    }
  }

  // capabilities check -- does it support powermode?
  if (cfg.powermode) {
    int powermode = ataCheckPowerMode(atadev);
    
    if (-1 == powermode) {
      PrintOut(LOG_CRIT, "Device: %s, no ATA CHECK POWER STATUS support, ignoring -n Directive\n", name);
      cfg.powermode=0;
    } 
    else if (powermode!=0x00 && powermode!=0x01
        && powermode!=0x40 && powermode!=0x41
        && powermode!=0x80 && powermode!=0x81 && powermode!=0x82 && powermode!=0x83
        && powermode!=0xff) {
      PrintOut(LOG_CRIT, "Device: %s, CHECK POWER STATUS returned %d, not ATA compliant, ignoring -n Directive\n",
               name, powermode);
      cfg.powermode=0;
    }
  }

  // Apply ATA settings
  std::string msg;

  if (cfg.set_aam)
    format_set_result_msg(msg, "AAM", (cfg.set_aam > 0 ?
      ata_set_features(atadev, ATA_ENABLE_AAM, cfg.set_aam-1) :
      ata_set_features(atadev, ATA_DISABLE_AAM)), cfg.set_aam, true);

  if (cfg.set_apm)
    format_set_result_msg(msg, "APM", (cfg.set_apm > 0 ?
      ata_set_features(atadev, ATA_ENABLE_APM, cfg.set_apm-1) :
      ata_set_features(atadev, ATA_DISABLE_APM)), cfg.set_apm, true);

  if (cfg.set_lookahead)
    format_set_result_msg(msg, "Rd-ahead", ata_set_features(atadev,
      (cfg.set_lookahead > 0 ? ATA_ENABLE_READ_LOOK_AHEAD : ATA_DISABLE_READ_LOOK_AHEAD)),
      cfg.set_lookahead);

  if (cfg.set_wcache)
    format_set_result_msg(msg, "Wr-cache", ata_set_features(atadev,
      (cfg.set_wcache > 0? ATA_ENABLE_WRITE_CACHE : ATA_DISABLE_WRITE_CACHE)), cfg.set_wcache);

  if (cfg.set_dsn)
    format_set_result_msg(msg, "DSN", ata_set_features(atadev,
      ATA_ENABLE_DISABLE_DSN, (cfg.set_dsn > 0 ? 0x1 : 0x2)));

  if (cfg.set_security_freeze)
    format_set_result_msg(msg, "Security freeze",
      ata_nodata_command(atadev, ATA_SECURITY_FREEZE_LOCK));

  if (cfg.set_standby)
    format_set_result_msg(msg, "Standby",
      ata_nodata_command(atadev, ATA_IDLE, cfg.set_standby-1), cfg.set_standby, true);

  // Report as one log entry
  if (!msg.empty())
    PrintOut(LOG_INFO, "Device: %s, ATA settings applied: %s\n", name, msg.c_str());

  // set SCT Error Recovery Control if requested
  if (cfg.sct_erc_set) {
    if (!isSCTErrorRecoveryControlCapable(&drive))
      PrintOut(LOG_INFO, "Device: %s, no SCT Error Recovery Control support, ignoring -l scterc\n",
               name);
    else if (locked)
      PrintOut(LOG_INFO, "Device: %s, no SCT support if ATA Security is LOCKED, ignoring -l scterc\n",
               name);
    else if (   ataSetSCTErrorRecoveryControltime(atadev, 1, cfg.sct_erc_readtime, false, false )
             || ataSetSCTErrorRecoveryControltime(atadev, 2, cfg.sct_erc_writetime, false, false))
      PrintOut(LOG_INFO, "Device: %s, set of SCT Error Recovery Control failed\n", name);
    else
      PrintOut(LOG_INFO, "Device: %s, SCT Error Recovery Control set to: Read: %u, Write: %u\n",
               name, cfg.sct_erc_readtime, cfg.sct_erc_writetime);
  }

  // If no tests available or selected, return
  if (!(   cfg.smartcheck  || cfg.selftest
        || cfg.errorlog    || cfg.xerrorlog
        || cfg.offlinests  || cfg.selfteststs
        || cfg.usagefailed || cfg.prefail  || cfg.usage
        || cfg.tempdiff    || cfg.tempinfo || cfg.tempcrit)) {
    CloseDevice(atadev, name);
    return 3;
  }
  
  // tell user we are registering device
  PrintOut(LOG_INFO,"Device: %s, is SMART capable. Adding to \"monitor\" list.\n",name);
  
  // close file descriptor
  CloseDevice(atadev, name);

  if (!state_path_prefix.empty() || !attrlog_path_prefix.empty()) {
    // Build file name for state file
    std::replace_if(model, model+strlen(model), not_allowed_in_filename, '_');
    std::replace_if(serial, serial+strlen(serial), not_allowed_in_filename, '_');
    if (!state_path_prefix.empty()) {
      cfg.state_file = strprintf("%s%s-%s.ata.state", state_path_prefix.c_str(), model, serial);
      // Read previous state
      if (read_dev_state(cfg.state_file.c_str(), state)) {
        PrintOut(LOG_INFO, "Device: %s, state read from %s\n", name, cfg.state_file.c_str());
        // Copy ATA attribute values to temp state
        state.update_temp_state();
      }
    }
    if (!attrlog_path_prefix.empty())
      cfg.attrlog_file = strprintf("%s%s-%s.ata.csv", attrlog_path_prefix.c_str(), model, serial);
  }

  finish_device_scan(cfg, state);

  return 0;
}

// on success, return 0. On failure, return >0.  Never return <0,
// please.
static int SCSIDeviceScan(dev_config & cfg, dev_state & state, scsi_device * scsidev,
                          const dev_config_vector * prev_cfgs)
{
  int err, req_len, avail_len, version, len;
  const char *device = cfg.name.c_str();
  struct scsi_iec_mode_page iec;
  uint8_t  tBuf[64];
  uint8_t  inqBuf[96];
  uint8_t  vpdBuf[252];
  char lu_id[64], serial[256], vendor[40], model[40];

  // Device must be open
  memset(inqBuf, 0, 96);
  req_len = 36;
  if ((err = scsiStdInquiry(scsidev, inqBuf, req_len))) {
    /* Marvell controllers fail on a 36 bytes StdInquiry, but 64 suffices */
    req_len = 64;
    int err64;
    if ((err64 = scsiStdInquiry(scsidev, inqBuf, req_len))) {
      PrintOut(LOG_INFO, "Device: %s, Both 36 and 64 byte INQUIRY failed; "
               "skip device [err=%d, %d]\n", device, err, err64);
      return 2;
    }
  }
  version = (inqBuf[2] & 0x7f); /* Accept old ISO/IEC 9316:1995 variants */

  avail_len = inqBuf[4] + 5;
  len = (avail_len < req_len) ? avail_len : req_len;
  if (len < 36) {
    PrintOut(LOG_INFO, "Device: %s, INQUIRY response less than 36 bytes; "
             "skip device\n", device);
    return 2;
  }

  int pdt = inqBuf[0] & 0x1f;

  switch (pdt) {
  case SCSI_PT_DIRECT_ACCESS:
  case SCSI_PT_WO:
  case SCSI_PT_CDROM:
  case SCSI_PT_OPTICAL:
  case SCSI_PT_RBC:             /* Reduced Block commands */
  case SCSI_PT_HOST_MANAGED:    /* Zoned disk */
    break;
  default:
    PrintOut(LOG_INFO, "Device: %s, not a disk like device [PDT=0x%x], "
             "skip\n", device, pdt);
    return 2;
  }

  if (supported_vpd_pages_p) {
    delete supported_vpd_pages_p;
    supported_vpd_pages_p = nullptr;
  }
  supported_vpd_pages_p = new supported_vpd_pages(scsidev);

  lu_id[0] = '\0';
  if (version >= 0x3) {
    /* SPC to SPC-5, assume SPC-6 is version==8 or higher */
    if (0 == scsiInquiryVpd(scsidev, SCSI_VPD_DEVICE_IDENTIFICATION,
                            vpdBuf, sizeof(vpdBuf))) {
      len = vpdBuf[3];
      scsi_decode_lu_dev_id(vpdBuf + 4, len, lu_id, sizeof(lu_id), nullptr);
    }
  }
  serial[0] = '\0';
  if (0 == scsiInquiryVpd(scsidev, SCSI_VPD_UNIT_SERIAL_NUMBER,
                          vpdBuf, sizeof(vpdBuf))) {
          len = vpdBuf[3];
          vpdBuf[4 + len] = '\0';
          scsi_format_id_string(serial, &vpdBuf[4], len);
  }

  char si_str[64];
  struct scsi_readcap_resp srr;
  uint64_t capacity = scsiGetSize(scsidev, scsidev->use_rcap16(), &srr);

  if (capacity)
    format_capacity(si_str, sizeof(si_str), capacity, ".");
  else
    si_str[0] = '\0';

  // Format device id string for warning emails
  cfg.dev_idinfo = strprintf("[%.8s %.16s %.4s]%s%s%s%s%s%s",
                     (char *)&inqBuf[8], (char *)&inqBuf[16], (char *)&inqBuf[32],
                     (lu_id[0] ? ", lu id: " : ""), (lu_id[0] ? lu_id : ""),
                     (serial[0] ? ", S/N: " : ""), (serial[0] ? serial : ""),
                     (si_str[0] ? ", " : ""), (si_str[0] ? si_str : ""));
  cfg.id_is_unique = (lu_id[0] || serial[0]);
  if (sanitize_dev_idinfo(cfg.dev_idinfo))
    cfg.id_is_unique = false;

  // format "model" string
  scsi_format_id_string(vendor, &inqBuf[8], 8);
  scsi_format_id_string(model, &inqBuf[16], 16);
  PrintOut(LOG_INFO, "Device: %s, %s\n", device, cfg.dev_idinfo.c_str());

  // Check for duplicates
  if (prev_cfgs && is_duplicate_dev_idinfo(cfg, *prev_cfgs)) {
    CloseDevice(scsidev, device);
    return 1;
  }

  // check that device is ready for commands. IE stores its stuff on
  // the media.
  if ((err = scsiTestUnitReady(scsidev))) {
    if (SIMPLE_ERR_NOT_READY == err)
      PrintOut(LOG_INFO, "Device: %s, NOT READY (e.g. spun down); skip device\n", device);
    else if (SIMPLE_ERR_NO_MEDIUM == err)
      PrintOut(LOG_INFO, "Device: %s, NO MEDIUM present; skip device\n", device);
    else if (SIMPLE_ERR_BECOMING_READY == err)
      PrintOut(LOG_INFO, "Device: %s, BECOMING (but not yet) READY; skip device\n", device);
    else
      PrintOut(LOG_CRIT, "Device: %s, failed Test Unit Ready [err=%d]\n", device, err);
    CloseDevice(scsidev, device);
    return 2; 
  }
  
  // Badly-conforming USB storage devices may fail this check.
  // The response to the following IE mode page fetch (current and
  // changeable values) is carefully examined. It has been found
  // that various USB devices that malform the response will lock up
  // if asked for a log page (e.g. temperature) so it is best to
  // bail out now.
  if (!(err = scsiFetchIECmpage(scsidev, &iec, state.modese_len)))
    state.modese_len = iec.modese_len;
  else if (SIMPLE_ERR_BAD_FIELD == err)
    ;  /* continue since it is reasonable not to support IE mpage */
  else { /* any other error (including malformed response) unreasonable */
    PrintOut(LOG_INFO, 
             "Device: %s, Bad IEC (SMART) mode page, err=%d, skip device\n", 
             device, err);
    CloseDevice(scsidev, device);
    return 3;
  }
  
  // N.B. The following is passive (i.e. it doesn't attempt to turn on
  // smart if it is off). This may change to be the same as the ATA side.
  if (!scsi_IsExceptionControlEnabled(&iec)) {
    PrintOut(LOG_INFO, "Device: %s, IE (SMART) not enabled, skip device\n"
                       "Try 'smartctl -s on %s' to turn on SMART features\n",
                        device, device);
    CloseDevice(scsidev, device);
    return 3;
  }
  
  // Flag that certain log pages are supported (information may be
  // available from other sources).
  if (0 == scsiLogSense(scsidev, SUPPORTED_LPAGES, 0, tBuf, sizeof(tBuf), 0) ||
      0 == scsiLogSense(scsidev, SUPPORTED_LPAGES, 0, tBuf, sizeof(tBuf), 68))
      /* workaround for the bug #678 on ST8000NM0075/E001. Up to 64 pages + 4b header */
  {
    for (int k = 4; k < tBuf[3] + LOGPAGEHDRSIZE; ++k) {
      switch (tBuf[k]) { 
      case TEMPERATURE_LPAGE:
        state.TempPageSupported = 1;
        break;
      case IE_LPAGE:
        state.SmartPageSupported = 1;
        break;
      case READ_ERROR_COUNTER_LPAGE:
        state.ReadECounterPageSupported = 1;
        break;
      case WRITE_ERROR_COUNTER_LPAGE:
        state.WriteECounterPageSupported = 1;
        break;
      case VERIFY_ERROR_COUNTER_LPAGE:
        state.VerifyECounterPageSupported = 1;
        break;
      case NON_MEDIUM_ERROR_LPAGE:
        state.NonMediumErrorPageSupported = 1;
        break;
      default:
        break;
      }
    }   
  }
  
  // Check if scsiCheckIE() is going to work
  {
    uint8_t asc = 0;
    uint8_t ascq = 0;
    uint8_t currenttemp = 0;
    uint8_t triptemp = 0;
    
    if (scsiCheckIE(scsidev, state.SmartPageSupported, state.TempPageSupported,
                    &asc, &ascq, &currenttemp, &triptemp)) {
      PrintOut(LOG_INFO, "Device: %s, unexpectedly failed to read SMART values\n", device);
      state.SuppressReport = 1;
    }
    if (   (state.SuppressReport || !currenttemp)
        && (cfg.tempdiff || cfg.tempinfo || cfg.tempcrit)) {
      PrintOut(LOG_INFO, "Device: %s, can't monitor Temperature, ignoring -W %d,%d,%d\n",
               device, cfg.tempdiff, cfg.tempinfo, cfg.tempcrit);
      cfg.tempdiff = cfg.tempinfo = cfg.tempcrit = 0;
    }
  }
  
  // capability check: self-test-log
  if (cfg.selftest){
    int retval = scsiCountFailedSelfTests(scsidev, 0);
    if (retval<0) {
      // no self-test log, turn off monitoring
      PrintOut(LOG_INFO, "Device: %s, does not support SMART Self-Test Log.\n", device);
      cfg.selftest = false;
      state.selflogcount = 0;
      state.selfloghour = 0;
    }
    else {
      // register starting values to watch for changes
      state.selflogcount = retval & 0xff;
      state.selfloghour  = (retval >> 8) & 0xffff;
    }
  }
  
  // disable autosave (set GLTSD bit)
  if (cfg.autosave==1){
    if (scsiSetControlGLTSD(scsidev, 1, state.modese_len))
      PrintOut(LOG_INFO,"Device: %s, could not disable autosave (set GLTSD bit).\n",device);
    else
      PrintOut(LOG_INFO,"Device: %s, disabled autosave (set GLTSD bit).\n",device);
  }

  // or enable autosave (clear GLTSD bit)
  if (cfg.autosave==2){
    if (scsiSetControlGLTSD(scsidev, 0, state.modese_len))
      PrintOut(LOG_INFO,"Device: %s, could not enable autosave (clear GLTSD bit).\n",device);
    else
      PrintOut(LOG_INFO,"Device: %s, enabled autosave (cleared GLTSD bit).\n",device);
  }
  
  // tell user we are registering device
  PrintOut(LOG_INFO, "Device: %s, is SMART capable. Adding to \"monitor\" list.\n", device);

  // Disable ATA specific self-tests
  state.not_cap_conveyance = state.not_cap_offline = state.not_cap_selective = true;

  // Make sure that init_standby_check() ignores SCSI devices
  cfg.offlinests_ns = cfg.selfteststs_ns = false;

  // close file descriptor
  CloseDevice(scsidev, device);

  if (!state_path_prefix.empty() || !attrlog_path_prefix.empty()) {
    // Build file name for state file
    std::replace_if(model, model+strlen(model), not_allowed_in_filename, '_');
    std::replace_if(serial, serial+strlen(serial), not_allowed_in_filename, '_');
    if (!state_path_prefix.empty()) {
      cfg.state_file = strprintf("%s%s-%s-%s.scsi.state", state_path_prefix.c_str(), vendor, model, serial);
      // Read previous state
      if (read_dev_state(cfg.state_file.c_str(), state)) {
        PrintOut(LOG_INFO, "Device: %s, state read from %s\n", device, cfg.state_file.c_str());
        // Copy ATA attribute values to temp state
        state.update_temp_state();
      }
    }
    if (!attrlog_path_prefix.empty())
      cfg.attrlog_file = strprintf("%s%s-%s-%s.scsi.csv", attrlog_path_prefix.c_str(), vendor, model, serial);
  }

  finish_device_scan(cfg, state);

  return 0;
}

// Check the NVMe Error Information log for device related errors.
static bool check_nvme_error_log(const dev_config & cfg, dev_state & state, nvme_device * nvmedev,
  uint64_t newcnt = 0)
{
  // Limit transfer size to one page (64 entries) to avoid problems with
  // limits of NVMe pass-through layer or too low MDTS values.
  unsigned want_entries = 64;
  if (want_entries > cfg.nvme_err_log_max_entries)
    want_entries = cfg.nvme_err_log_max_entries;
  raw_buffer error_log_buf(want_entries * sizeof(nvme_error_log_page));
  nvme_error_log_page * error_log =
    reinterpret_cast<nvme_error_log_page *>(error_log_buf.data());
  unsigned read_entries = nvme_read_error_log(nvmedev, error_log, want_entries, false /*!lpo_sup*/);
  if (!read_entries) {
    PrintOut(LOG_INFO, "Device: %s, Read %u entries from Error Information Log failed\n",
      cfg.name.c_str(), want_entries);
    return false;
  }

  if (!newcnt)
    return true; // Support check only

  // Scan log, find device related errors
  uint64_t oldcnt = state.nvme_err_log_entries, mincnt = newcnt;
  int err = 0, ign = 0;
  for (unsigned i = 0; i < read_entries; i++) {
    const nvme_error_log_page & e = error_log[i];
    if (!e.error_count)
      continue; // unused
    if (e.error_count <= oldcnt)
      break; // stop on first old entry
    if (e.error_count < mincnt)
      mincnt = e.error_count; // min known error
    if (e.error_count > newcnt)
      newcnt = e.error_count; // adjust maximum
    uint16_t status = e.status_field >> 1;
    if (!nvme_status_is_error(status) || nvme_status_to_errno(status) == EINVAL) {
      ign++; // Not a device related error
      continue;
    }

    // Log the most recent 8 errors
    if (++err > 8)
      continue;
    char buf[64];
    PrintOut(LOG_INFO, "Device: %s, NVMe error [%u], count %" PRIu64 ", status 0x%04x: %s\n",
      cfg.name.c_str(), i, e.error_count, e.status_field,
      nvme_status_to_info_str(buf, e.status_field >> 1));
  }

  std::string msg = strprintf("Device: %s, NVMe error count increased from %" PRIu64 " to %" PRIu64
                              " (%d new, %d ignored, %" PRIu64 " unknown)",
                              cfg.name.c_str(), oldcnt, newcnt, err, ign,
                              (mincnt > oldcnt + 1 ? mincnt - oldcnt - 1 : 0));
  // LOG_CRIT only if device related errors are found
  if (!err) {
    PrintOut(LOG_INFO, "%s\n", msg.c_str());
  }
  else {
    PrintOut(LOG_CRIT, "%s\n", msg.c_str());
    MailWarning(cfg, state, 4, "%s", msg.c_str());
  }

  state.nvme_err_log_entries = newcnt;
  state.must_write = true;
  return true;
}

static int NVMeDeviceScan(dev_config & cfg, dev_state & state, nvme_device * nvmedev,
                          const dev_config_vector * prev_cfgs)
{
  const char *name = cfg.name.c_str();

  // Device must be open

  // Get ID Controller
  nvme_id_ctrl id_ctrl;
  if (!nvme_read_id_ctrl(nvmedev, id_ctrl)) {
    PrintOut(LOG_INFO, "Device: %s, NVMe Identify Controller failed\n", name);
    CloseDevice(nvmedev, name);
    return 2;
  }

  // Get drive identity
  char model[40+1], serial[20+1], firmware[8+1];
  format_char_array(model, id_ctrl.mn);
  format_char_array(serial, id_ctrl.sn);
  format_char_array(firmware, id_ctrl.fr);

  // Format device id string for warning emails
  char nsstr[32] = "", capstr[32] = "";
  unsigned nsid = nvmedev->get_nsid();
  if (nsid != nvme_broadcast_nsid)
    snprintf(nsstr, sizeof(nsstr), ", NSID:%u", nsid);
  uint64_t capacity = le128_to_uint64(id_ctrl.tnvmcap);
  if (capacity)
    format_capacity(capstr, sizeof(capstr), capacity, ".");

  auto idinfo = &dev_config::dev_idinfo;
  for (;;) {
    cfg.*idinfo = strprintf("%s, S/N:%s, FW:%s%s%s%s", model, serial, firmware,
                            nsstr, (capstr[0] ? ", " : ""), capstr);
    if (!(nsstr[0] && id_ctrl.nn == 1))
      break; // No namespace id or device supports multiple namespaces
    // Keep version without namespace id for 'is_duplicate_dev_idinfo()'
    nsstr[0] = 0;
    idinfo = &dev_config::dev_idinfo_bc;
  }

  cfg.id_is_unique = true; // TODO: Check serial?
  if (sanitize_dev_idinfo(cfg.dev_idinfo))
    cfg.id_is_unique = false;

  PrintOut(LOG_INFO, "Device: %s, %s\n", name, cfg.dev_idinfo.c_str());

  // Check for duplicates
  if (prev_cfgs && is_duplicate_dev_idinfo(cfg, *prev_cfgs)) {
    CloseDevice(nvmedev, name);
    return 1;
  }

  // Read SMART/Health log
  // TODO: Support per namespace SMART/Health log
  nvme_smart_log & smart_log = state.nvme_smartval;
  if (!nvme_read_smart_log(nvmedev, nvme_broadcast_nsid, smart_log)) {
    PrintOut(LOG_INFO, "Device: %s, failed to read NVMe SMART/Health Information\n", name);
    CloseDevice(nvmedev, name);
    return 2;
  }

  // Check temperature sensor support
  if (cfg.tempdiff || cfg.tempinfo || cfg.tempcrit) {
    if (!sg_get_unaligned_le16(smart_log.temperature)) {
      PrintOut(LOG_INFO, "Device: %s, no Temperature sensors, ignoring -W %d,%d,%d\n",
               name, cfg.tempdiff, cfg.tempinfo, cfg.tempcrit);
      cfg.tempdiff = cfg.tempinfo = cfg.tempcrit = 0;
    }
  }

  // Init total error count
  cfg.nvme_err_log_max_entries = id_ctrl.elpe + 1; // 0's based value
  if (cfg.errorlog || cfg.xerrorlog) {
    if (!check_nvme_error_log(cfg, state, nvmedev)) {
      PrintOut(LOG_INFO, "Device: %s, Error Information unavailable, ignoring -l [x]error\n", name);
      cfg.errorlog = cfg.xerrorlog = false;
    }
    else
      state.nvme_err_log_entries = le128_to_uint64(smart_log.num_err_log_entries);
  }

  // Check for self-test support
  state.not_cap_short = state.not_cap_long = !(id_ctrl.oacs & 0x0010);
  state.selflogcount = 0; state.selfloghour = 0;
  if (cfg.selftest || cfg.selfteststs || !cfg.test_regex.empty()) {
    nvme_self_test_log self_test_log;
    if (   !state.not_cap_short
        && !nvme_read_self_test_log(nvmedev, nvme_broadcast_nsid, self_test_log)) {
      PrintOut(LOG_INFO, "Device: %s, Read NVMe Self-test Log failed: %s\n", name,
               nvmedev->get_errmsg());
      state.not_cap_short = state.not_cap_long = true;
    }
    if (state.not_cap_short) {
      PrintOut(LOG_INFO, "Device: %s, does not support NVMe Self-tests, ignoring%s%s%s%s\n", name,
               (cfg.selftest ? " -l selftest" : ""),
               (cfg.selfteststs ? " -l selfteststs" : ""),
               (!cfg.test_regex.empty() ? " -s " : ""), cfg.test_regex.get_pattern());
      cfg.selftest = cfg.selfteststs = false; cfg.test_regex = {};
    }
  }

  // If no supported tests selected, return
  if (!(   cfg.smartcheck_nvme
        || cfg.prefail  || cfg.usage || cfg.usagefailed
        || cfg.errorlog || cfg.xerrorlog
        || cfg.selftest || cfg.selfteststs || !cfg.test_regex.empty()
        || cfg.tempdiff || cfg.tempinfo || cfg.tempcrit              )) {
    CloseDevice(nvmedev, name);
    return 3;
  }

  // Tell user we are registering device
  PrintOut(LOG_INFO,"Device: %s, is SMART capable. Adding to \"monitor\" list.\n", name);

  // Disable ATA specific self-tests
  state.not_cap_conveyance = state.not_cap_offline = state.not_cap_selective = true;

  // Make sure that init_standby_check() ignores NVMe devices
  // TODO: Implement '-l selfteststs,ns' for NVMe
  cfg.offlinests_ns = cfg.selfteststs_ns = false;

  CloseDevice(nvmedev, name);

  if (!state_path_prefix.empty() || !attrlog_path_prefix.empty()) {
    // Build file name for state file
    std::replace_if(model, model+strlen(model), not_allowed_in_filename, '_');
    std::replace_if(serial, serial+strlen(serial), not_allowed_in_filename, '_');
    nsstr[0] = 0;
    if (nsid != nvme_broadcast_nsid)
      snprintf(nsstr, sizeof(nsstr), "-n%u", nsid);
    if (!state_path_prefix.empty()) {
      cfg.state_file = strprintf("%s%s-%s%s.nvme.state", state_path_prefix.c_str(), model, serial, nsstr);
      // Read previous state
      if (read_dev_state(cfg.state_file.c_str(), state))
        PrintOut(LOG_INFO, "Device: %s, state read from %s\n", name, cfg.state_file.c_str());
    }
    if (!attrlog_path_prefix.empty())
      cfg.attrlog_file = strprintf("%s%s-%s%s.nvme.csv", attrlog_path_prefix.c_str(), model, serial, nsstr);
  }

  finish_device_scan(cfg, state);

  return 0;
}

// Open device for next check, return false on error
static bool open_device(const dev_config & cfg, dev_state & state, smart_device * device,
                        const char * type)
{
  const char * name = cfg.name.c_str();

  // If user has asked, test the email warning system
  if (cfg.emailtest)
    MailWarning(cfg, state, 0, "TEST EMAIL from smartd for device: %s", name);

  // User may have requested (with the -n Directive) to leave the disk
  // alone if it is in idle or standby mode.  In this case check the
  // power mode first before opening the device for full access,
  // and exit without check if disk is reported in standby.
  if (device->is_ata() && cfg.powermode && !state.powermodefail && !state.removed) {
    // Note that 'is_powered_down()' handles opening the device itself, and
    // can be used before calling 'open()' (that's the whole point of 'is_powered_down()'!).
    if (device->is_powered_down())
    {
      // skip at most powerskipmax checks
      if (!cfg.powerskipmax || state.powerskipcnt<cfg.powerskipmax) {
        // report first only except if state has changed, avoid waking up system disk
        if ((!state.powerskipcnt || state.lastpowermodeskipped != -1) && !cfg.powerquiet) {
          PrintOut(LOG_INFO, "Device: %s, is in %s mode, suspending checks\n", name, "STANDBY (OS)");
          state.lastpowermodeskipped = -1;
        }
        state.powerskipcnt++;
        return false;
      }
    }
  }

  // if we can't open device, fail gracefully rather than hard --
  // perhaps the next time around we'll be able to open it
  if (!device->open()) {
    // For removable devices, print error message only once and suppress email
    if (!cfg.removable) {
      PrintOut(LOG_INFO, "Device: %s, open() of %s device failed: %s\n", name, type, device->get_errmsg());
      MailWarning(cfg, state, 9, "Device: %s, unable to open %s device", name, type);
    }
    else if (!state.removed) {
      PrintOut(LOG_INFO, "Device: %s, removed %s device: %s\n", name, type, device->get_errmsg());
      state.removed = true;
    }
    else if (debugmode)
      PrintOut(LOG_INFO, "Device: %s, %s device still removed: %s\n", name, type, device->get_errmsg());
    return false;
  }

  if (debugmode)
    PrintOut(LOG_INFO,"Device: %s, opened %s device\n", name, type);

  if (!cfg.removable)
    reset_warning_mail(cfg, state, 9, "open of %s device worked again", type);
  else if (state.removed) {
    PrintOut(LOG_INFO, "Device: %s, reconnected %s device\n", name, type);
    state.removed = false;
  }

  return true;
}

// If the self-test log has got more self-test errors (or more recent
// self-test errors) recorded, then notify user.
static void report_self_test_log_changes(const dev_config & cfg, dev_state & state,
                                         int errcnt, uint64_t hour)
{
  const char * name = cfg.name.c_str();

  if (errcnt < 0)
    // command failed
    // TODO: Move this to ATA/SCSICheckDevice()
    MailWarning(cfg, state, 8, "Device: %s, Read SMART Self-Test Log Failed", name);
  else {
    reset_warning_mail(cfg, state, 8, "Read SMART Self-Test Log worked again");

    if (state.selflogcount < errcnt) {
      // increase in error count
      PrintOut(LOG_CRIT, "Device: %s, Self-Test Log error count increased from %d to %d\n",
               name, state.selflogcount, errcnt);
      MailWarning(cfg, state, 3, "Device: %s, Self-Test Log error count increased from %d to %d",
                  name, state.selflogcount, errcnt);
      state.must_write = true;
    }
    else if (errcnt > 0 && state.selfloghour != hour) {
      // more recent error
      // ATA: a 'more recent' error might actually be a smaller hour number,
      // if the hour number has wrapped.
      // There's still a bug here.  You might just happen to run a new test
      // exactly 32768 hours after the previous failure, and have run exactly
      // 20 tests between the two, in which case smartd will miss the
      // new failure.
      PrintOut(LOG_CRIT, "Device: %s, new Self-Test Log error at hour timestamp %" PRIu64 "\n",
               name, hour);
      MailWarning(cfg, state, 3, "Device: %s, new Self-Test Log error at hour timestamp %" PRIu64 "\n",
                   name, hour);
      state.must_write = true;
    }

    // Print info if error entries have disappeared
    // or newer successful extended self-test exists
    if (state.selflogcount > errcnt) {
      PrintOut(LOG_INFO, "Device: %s, Self-Test Log error count decreased from %d to %d\n",
               name, state.selflogcount, errcnt);
      if (errcnt == 0)
        reset_warning_mail(cfg, state, 3, "Self-Test Log does no longer report errors");
    }

    state.selflogcount = errcnt;
    state.selfloghour  = hour;
  }
  return;
}

// Test types, ordered by priority.
static const char test_type_chars[] = "LncrSCO";
static const unsigned num_test_types = sizeof(test_type_chars)-1;

// returns test type if time to do test of type testtype,
// 0 if not time to do test.
static char next_scheduled_test(const dev_config & cfg, dev_state & state, time_t usetime = 0)
{
  // check that self-testing has been requested
  if (cfg.test_regex.empty())
    return 0;

  // Exit if drive not capable of any test
  if (   state.not_cap_long && state.not_cap_short
      && state.not_cap_conveyance && state.not_cap_offline && state.not_cap_selective)
    return 0;

  // since we are about to call localtime(), be sure glibc is informed
  // of any timezone changes we make.
  if (!usetime)
    FixGlibcTimeZoneBug();
  
  // Is it time for next check?
  time_t now = (!usetime ? time(nullptr) : usetime);
  if (now < state.scheduled_test_next_check) {
    if (state.scheduled_test_next_check <= now + 3600)
      return 0; // Next check within one hour
    // More than one hour, assume system clock time adjusted to the past
    state.scheduled_test_next_check = now;
  }
  else if (state.scheduled_test_next_check + (3600L*24*90) < now) {
    // Limit time check interval to 90 days
    state.scheduled_test_next_check = now - (3600L*24*90);
  }

  // Find ':NNN[-LLL]' in regex for possible offsets and limits
  const unsigned max_offsets = 1 + num_test_types;
  unsigned offsets[max_offsets] = {0, }, limits[max_offsets] = {0, };
  unsigned num_offsets = 1; // offsets/limits[0] == 0 always
  for (const char * p = cfg.test_regex.get_pattern(); num_offsets < max_offsets; ) {
    const char * q = strchr(p, ':');
    if (!q)
      break;
    p = q + 1;
    unsigned offset = 0, limit = 0; int n1 = -1, n2 = -1, n3 = -1;
    sscanf(p, "%u%n-%n%u%n", &offset, &n1, &n2, &limit, &n3);
    if (!(n1 == 3 && (n2 < 0 || (n3 == 3+1+3 && limit > 0))))
      continue;
    offsets[num_offsets] = offset; limits[num_offsets] = limit;
    num_offsets++;
    p += (n3 > 0 ? n3 : n1);
  }

  // Check interval [state.scheduled_test_next_check, now] for scheduled tests
  char testtype = 0;
  time_t testtime = 0;
  int maxtest = num_test_types-1;

  for (time_t t = state.scheduled_test_next_check; ; ) {
    // Check offset 0 and then all offsets for ':NNN' found above
    for (unsigned i = 0; i < num_offsets; i++) {
      unsigned offset = offsets[i], limit = limits[i];
      unsigned delay = cfg.test_offset_factor * offset;
      if (0 < limit && limit < delay)
        delay %= limit + 1;
      struct tm tmbuf, * tms = time_to_tm_local(&tmbuf, t - (delay * 3600));

      // tm_wday is 0 (Sunday) to 6 (Saturday).  We use 1 (Monday) to 7 (Sunday).
      int weekday = (tms->tm_wday ? tms->tm_wday : 7);
      for (int j = 0; j <= maxtest; j++) {
        // Skip if drive not capable of this test
        switch (test_type_chars[j]) {
          case 'L': if (state.not_cap_long)       continue; break;
          case 'S': if (state.not_cap_short)      continue; break;
          case 'C': if (state.not_cap_conveyance) continue; break;
          case 'O': if (state.not_cap_offline)    continue; break;
          case 'c': case 'n':
          case 'r': if (state.not_cap_selective)  continue; break;
          default: continue;
        }
        // Try match of "T/MM/DD/d/HH[:NNN]"
        char pattern[64];
        snprintf(pattern, sizeof(pattern), "%c/%02d/%02d/%1d/%02d",
          test_type_chars[j], tms->tm_mon+1, tms->tm_mday, weekday, tms->tm_hour);
        if (i > 0) {
          const unsigned len = sizeof("S/01/01/1/01") - 1;
          snprintf(pattern + len, sizeof(pattern) - len, ":%03u", offset);
          if (limit > 0)
            snprintf(pattern + len + 4, sizeof(pattern) - len - 4, "-%03u", limit);
        }
        if (cfg.test_regex.full_match(pattern)) {
          // Test found
          testtype = pattern[0];
          testtime = t;
          // Limit further matches to higher priority self-tests
          maxtest = j-1;
          break;
        }
      }
    }

    // Exit if no tests left or current time reached
    if (maxtest < 0)
      break;
    if (t >= now)
      break;
    // Check next hour
    if ((t += 3600) > now)
      t = now;
   }

  // Do next check not before next hour.
  struct tm tmbuf, * tmnow = time_to_tm_local(&tmbuf, now);
  state.scheduled_test_next_check = now + (3600 - tmnow->tm_min*60 - tmnow->tm_sec);

  if (testtype) {
    state.must_write = true;
    // Tell user if an old test was found.
    if (!usetime && (testtime / 3600) < (now / 3600)) {
      char datebuf[DATEANDEPOCHLEN]; dateandtimezoneepoch(datebuf, testtime);
      PrintOut(LOG_INFO, "Device: %s, old test of type %c not run at %s, starting now.\n",
        cfg.name.c_str(), testtype, datebuf);
    }
  }

  return testtype;
}

// Print a list of future tests.
static void PrintTestSchedule(const dev_config_vector & configs, dev_state_vector & states, const smart_device_list & devices)
{
  unsigned numdev = configs.size();
  if (!numdev)
    return;
  std::vector<int> testcnts(numdev * num_test_types, 0);

  PrintOut(LOG_INFO, "\nNext scheduled self tests (at most 5 of each type per device):\n");

  // FixGlibcTimeZoneBug(); // done in PrintOut()
  time_t now = time(nullptr);
  char datenow[DATEANDEPOCHLEN], date[DATEANDEPOCHLEN];
  dateandtimezoneepoch(datenow, now);

  long seconds;
  for (seconds=checktime; seconds<3600L*24*90; seconds+=checktime) {
    // Check for each device whether a test will be run
    time_t testtime = now + seconds;
    for (unsigned i = 0; i < numdev; i++) {
      const dev_config & cfg = configs.at(i);
      dev_state & state = states.at(i);
      const char * p;
      char testtype = next_scheduled_test(cfg, state, testtime);
      if (testtype && (p = strchr(test_type_chars, testtype))) {
        unsigned t = (p - test_type_chars);
        // Report at most 5 tests of each type
        if (++testcnts[i*num_test_types + t] <= 5) {
          dateandtimezoneepoch(date, testtime);
          PrintOut(LOG_INFO, "Device: %s, will do test %d of type %c at %s\n", cfg.name.c_str(),
            testcnts[i*num_test_types + t], testtype, date);
        }
      }
    }
  }

  // Report totals
  dateandtimezoneepoch(date, now+seconds);
  PrintOut(LOG_INFO, "\nTotals [%s - %s]:\n", datenow, date);
  for (unsigned i = 0; i < numdev; i++) {
    const dev_config & cfg = configs.at(i);
    bool ata = devices.at(i)->is_ata();
    for (unsigned t = 0; t < num_test_types; t++) {
      int cnt = testcnts[i*num_test_types + t];
      if (cnt == 0 && !strchr((ata ? "LSCO" : "LS"), test_type_chars[t]))
        continue;
      PrintOut(LOG_INFO, "Device: %s, will do %3d test%s of type %c\n", cfg.name.c_str(),
        cnt, (cnt==1?"":"s"), test_type_chars[t]);
    }
  }

}

// Return zero on success, nonzero on failure. Perform offline (background)
// short or long (extended) self test on given scsi device.
static int DoSCSISelfTest(const dev_config & cfg, dev_state & state, scsi_device * device, char testtype)
{
  int retval = 0;
  const char *testname = nullptr;
  const char *name = cfg.name.c_str();
  int inProgress;

  if (scsiSelfTestInProgress(device, &inProgress)) {
    PrintOut(LOG_CRIT, "Device: %s, does not support Self-Tests\n", name);
    state.not_cap_short = state.not_cap_long = true;
    return 1;
  }

  if (1 == inProgress) {
    PrintOut(LOG_INFO, "Device: %s, skip since Self-Test already in "
             "progress.\n", name);
    return 1;
  }

  switch (testtype) {
  case 'S':
    testname = "Short Self";
    retval = scsiSmartShortSelfTest(device);
    break;
  case 'L':
    testname = "Long Self";
    retval = scsiSmartExtendSelfTest(device);
    break;
  }
  // If we can't do the test, exit
  if (!testname) {
    PrintOut(LOG_CRIT, "Device: %s, not capable of %c Self-Test\n", name, 
             testtype);
    return 1;
  }
  if (retval) {
    if ((SIMPLE_ERR_BAD_OPCODE == retval) || 
        (SIMPLE_ERR_BAD_FIELD == retval)) {
      PrintOut(LOG_CRIT, "Device: %s, not capable of %s-Test\n", name, 
               testname);
      if ('L'==testtype)
        state.not_cap_long = true;
      else
        state.not_cap_short = true;
     
      return 1;
    }
    PrintOut(LOG_CRIT, "Device: %s, execute %s-Test failed (err: %d)\n", name, 
             testname, retval);
    return 1;
  }
  
  PrintOut(LOG_INFO, "Device: %s, starting scheduled %s-Test.\n", name, testname);
  
  return 0;
}

// Do an offline immediate or self-test.  Return zero on success,
// nonzero on failure.
static int DoATASelfTest(const dev_config & cfg, dev_state & state, ata_device * device, char testtype)
{
  const char *name = cfg.name.c_str();

  // Read current smart data and check status/capability
  // TODO: Reuse smart data already read in ATACheckDevice()
  struct ata_smart_values data;
  if (ataReadSmartValues(device, &data) || !(data.offline_data_collection_capability)) {
    PrintOut(LOG_CRIT, "Device: %s, not capable of Offline or Self-Testing.\n", name);
    return 1;
  }
  
  // Check for capability to do the test
  int dotest = -1, mode = 0;
  const char *testname = nullptr;
  switch (testtype) {
  case 'O':
    testname="Offline Immediate ";
    if (isSupportExecuteOfflineImmediate(&data))
      dotest=OFFLINE_FULL_SCAN;
    else
      state.not_cap_offline = true;
    break;
  case 'C':
    testname="Conveyance Self-";
    if (isSupportConveyanceSelfTest(&data))
      dotest=CONVEYANCE_SELF_TEST;
    else
      state.not_cap_conveyance = true;
    break;
  case 'S':
    testname="Short Self-";
    if (isSupportSelfTest(&data))
      dotest=SHORT_SELF_TEST;
    else
      state.not_cap_short = true;
    break;
  case 'L':
    testname="Long Self-";
    if (isSupportSelfTest(&data))
      dotest=EXTEND_SELF_TEST;
    else
      state.not_cap_long = true;
    break;

  case 'c': case 'n': case 'r':
    testname = "Selective Self-";
    if (isSupportSelectiveSelfTest(&data)) {
      dotest = SELECTIVE_SELF_TEST;
      switch (testtype) {
        case 'c': mode = SEL_CONT; break;
        case 'n': mode = SEL_NEXT; break;
        case 'r': mode = SEL_REDO; break;
      }
    }
    else
      state.not_cap_selective = true;
    break;
  }
  
  // If we can't do the test, exit
  if (dotest<0) {
    PrintOut(LOG_CRIT, "Device: %s, not capable of %sTest\n", name, testname);
    return 1;
  }
  
  // If currently running a self-test, do not interrupt it to start another.
  if (15==(data.self_test_exec_status >> 4)) {
    if (cfg.firmwarebugs.is_set(BUG_SAMSUNG3) && data.self_test_exec_status == 0xf0) {
      PrintOut(LOG_INFO, "Device: %s, will not skip scheduled %sTest "
               "despite unclear Self-Test byte (SAMSUNG Firmware bug).\n", name, testname);
    } else {
      PrintOut(LOG_INFO, "Device: %s, skip scheduled %sTest; %1d0%% remaining of current Self-Test.\n",
               name, testname, (int)(data.self_test_exec_status & 0x0f));
      return 1;
    }
  }

  if (dotest == SELECTIVE_SELF_TEST) {
    // Set test span
    ata_selective_selftest_args selargs, prev_args;
    selargs.num_spans = 1;
    selargs.span[0].mode = mode;
    prev_args.num_spans = 1;
    prev_args.span[0].start = state.selective_test_last_start;
    prev_args.span[0].end   = state.selective_test_last_end;
    if (ataWriteSelectiveSelfTestLog(device, selargs, &data, state.num_sectors, &prev_args)) {
      PrintOut(LOG_CRIT, "Device: %s, prepare %sTest failed\n", name, testname);
      return 1;
    }
    uint64_t start = selargs.span[0].start, end = selargs.span[0].end;
    PrintOut(LOG_INFO, "Device: %s, %s test span at LBA %" PRIu64 " - %" PRIu64 " (%" PRIu64 " sectors, %u%% - %u%% of disk).\n",
      name, (selargs.span[0].mode == SEL_NEXT ? "next" : "redo"),
      start, end, end - start + 1,
      (unsigned)((100 * start + state.num_sectors/2) / state.num_sectors),
      (unsigned)((100 * end   + state.num_sectors/2) / state.num_sectors));
    state.selective_test_last_start = start;
    state.selective_test_last_end = end;
  }

  // execute the test, and return status
  int retval = smartcommandhandler(device, IMMEDIATE_OFFLINE, dotest, nullptr);
  if (retval) {
    PrintOut(LOG_CRIT, "Device: %s, execute %sTest failed.\n", name, testname);
    return retval;
  }

  // Report recent test start to do_disable_standby_check()
  // and force log of next test status
  if (testtype == 'O')
    state.offline_started = true;
  else
    state.selftest_started = true;

  PrintOut(LOG_INFO, "Device: %s, starting scheduled %sTest.\n", name, testname);
  return 0;
}

// Check pending sector count attribute values (-C, -U directives).
static void check_pending(const dev_config & cfg, dev_state & state,
                          unsigned char id, bool increase_only,
                          const ata_smart_values & smartval,
                          int mailtype, const char * msg)
{
  // Find attribute index
  int i = ata_find_attr_index(id, smartval);
  if (!(i >= 0 && ata_find_attr_index(id, state.smartval) == i))
    return;

  // No report if no sectors pending.
  uint64_t rawval = ata_get_attr_raw_value(smartval.vendor_attributes[i], cfg.attribute_defs);
  if (rawval == 0) {
    reset_warning_mail(cfg, state, mailtype, "No more %s", msg);
    return;
  }

  // If attribute is not reset, report only sector count increases.
  uint64_t prev_rawval = ata_get_attr_raw_value(state.smartval.vendor_attributes[i], cfg.attribute_defs);
  if (!(!increase_only || prev_rawval < rawval))
    return;

  // Format message.
  std::string s = strprintf("Device: %s, %" PRId64 " %s", cfg.name.c_str(), rawval, msg);
  if (prev_rawval > 0 && rawval != prev_rawval)
    s += strprintf(" (changed %+" PRId64 ")", rawval - prev_rawval);

  PrintOut(LOG_CRIT, "%s\n", s.c_str());
  MailWarning(cfg, state, mailtype, "%s", s.c_str());
  state.must_write = true;
}

// Format Temperature value
static const char * fmt_temp(unsigned char x, char (& buf)[20])
{
  if (!x) // unset
    return "??";
  snprintf(buf, sizeof(buf), "%u", x);
  return buf;
}

// Check Temperature limits
static void CheckTemperature(const dev_config & cfg, dev_state & state, unsigned char currtemp, unsigned char triptemp)
{
  if (!(0 < currtemp && currtemp < 255)) {
    PrintOut(LOG_INFO, "Device: %s, failed to read Temperature\n", cfg.name.c_str());
    return;
  }

  // Update Max Temperature
  const char * minchg = "", * maxchg = "";
  if (currtemp > state.tempmax) {
    if (state.tempmax)
      maxchg = "!";
    state.tempmax = currtemp;
    state.must_write = true;
  }

  char buf[20];
  if (!state.temperature) {
    // First check
    if (!state.tempmin || currtemp < state.tempmin)
        // Delay Min Temperature update by ~ 30 minutes.
        state.tempmin_delay = time(nullptr) + default_checktime - 60;
    PrintOut(LOG_INFO, "Device: %s, initial Temperature is %d Celsius (Min/Max %s/%u%s)\n",
      cfg.name.c_str(), (int)currtemp, fmt_temp(state.tempmin, buf), state.tempmax, maxchg);
    if (triptemp)
      PrintOut(LOG_INFO, "    [trip Temperature is %d Celsius]\n", (int)triptemp);
    state.temperature = currtemp;
  }
  else {
    if (state.tempmin_delay) {
      // End Min Temperature update delay if ...
      if (   (state.tempmin && currtemp > state.tempmin) // current temp exceeds recorded min,
          || (state.tempmin_delay <= time(nullptr))) {   // or delay time is over.
        state.tempmin_delay = 0;
        if (!state.tempmin)
          state.tempmin = 255;
      }
    }

    // Update Min Temperature
    if (!state.tempmin_delay && currtemp < state.tempmin) {
      state.tempmin = currtemp;
      state.must_write = true;
      if (currtemp != state.temperature)
        minchg = "!";
    }

    // Track changes
    if (cfg.tempdiff && (*minchg || *maxchg || abs((int)currtemp - (int)state.temperature) >= cfg.tempdiff)) {
      PrintOut(LOG_INFO, "Device: %s, Temperature changed %+d Celsius to %u Celsius (Min/Max %s%s/%u%s)\n",
        cfg.name.c_str(), (int)currtemp-(int)state.temperature, currtemp, fmt_temp(state.tempmin, buf), minchg, state.tempmax, maxchg);
      state.temperature = currtemp;
    }
  }

  // Check limits
  if (cfg.tempcrit && currtemp >= cfg.tempcrit) {
    PrintOut(LOG_CRIT, "Device: %s, Temperature %u Celsius reached critical limit of %u Celsius (Min/Max %s%s/%u%s)\n",
      cfg.name.c_str(), currtemp, cfg.tempcrit, fmt_temp(state.tempmin, buf), minchg, state.tempmax, maxchg);
    MailWarning(cfg, state, 12, "Device: %s, Temperature %d Celsius reached critical limit of %u Celsius (Min/Max %s%s/%u%s)",
      cfg.name.c_str(), currtemp, cfg.tempcrit, fmt_temp(state.tempmin, buf), minchg, state.tempmax, maxchg);
  }
  else if (cfg.tempinfo && currtemp >= cfg.tempinfo) {
    PrintOut(LOG_INFO, "Device: %s, Temperature %u Celsius reached limit of %u Celsius (Min/Max %s%s/%u%s)\n",
      cfg.name.c_str(), currtemp, cfg.tempinfo, fmt_temp(state.tempmin, buf), minchg, state.tempmax, maxchg);
  }
  else if (cfg.tempcrit) {
    unsigned char limit = (cfg.tempinfo ? cfg.tempinfo : cfg.tempcrit-5);
    if (currtemp < limit)
      reset_warning_mail(cfg, state, 12, "Temperature %u Celsius dropped below %u Celsius", currtemp, limit);
  }
}

// Check normalized and raw attribute values.
static void check_attribute(const dev_config & cfg, dev_state & state,
                            const ata_smart_attribute & attr,
                            const ata_smart_attribute & prev,
                            int attridx,
                            const ata_smart_threshold_entry * thresholds)
{
  // Check attribute and threshold
  ata_attr_state attrstate = ata_get_attr_state(attr, attridx, thresholds, cfg.attribute_defs);
  if (attrstate == ATTRSTATE_NON_EXISTING)
    return;

  // If requested, check for usage attributes that have failed.
  if (   cfg.usagefailed && attrstate == ATTRSTATE_FAILED_NOW
      && !cfg.monitor_attr_flags.is_set(attr.id, MONITOR_IGN_FAILUSE)) {
    std::string attrname = ata_get_smart_attr_name(attr.id, cfg.attribute_defs, cfg.dev_rpm);
    PrintOut(LOG_CRIT, "Device: %s, Failed SMART usage Attribute: %d %s.\n", cfg.name.c_str(), attr.id, attrname.c_str());
    MailWarning(cfg, state, 2, "Device: %s, Failed SMART usage Attribute: %d %s.", cfg.name.c_str(), attr.id, attrname.c_str());
    state.must_write = true;
  }

  // Return if we're not tracking this type of attribute
  bool prefail = !!ATTRIBUTE_FLAGS_PREFAILURE(attr.flags);
  if (!(   ( prefail && cfg.prefail)
        || (!prefail && cfg.usage  )))
    return;

  // Return if '-I ID' was specified
  if (cfg.monitor_attr_flags.is_set(attr.id, MONITOR_IGNORE))
    return;

  // Issue warning if they don't have the same ID in all structures.
  if (attr.id != prev.id) {
    PrintOut(LOG_INFO,"Device: %s, same Attribute has different ID numbers: %d = %d\n",
             cfg.name.c_str(), attr.id, prev.id);
    return;
  }

  // Compare normalized values if valid.
  bool valchanged = false;
  if (attrstate > ATTRSTATE_NO_NORMVAL) {
    if (attr.current != prev.current)
      valchanged = true;
  }

  // Compare raw values if requested.
  bool rawchanged = false;
  if (cfg.monitor_attr_flags.is_set(attr.id, MONITOR_RAW)) {
    if (   ata_get_attr_raw_value(attr, cfg.attribute_defs)
        != ata_get_attr_raw_value(prev, cfg.attribute_defs))
      rawchanged = true;
  }

  // Return if no change
  if (!(valchanged || rawchanged))
    return;

  // Format value strings
  std::string currstr, prevstr;
  if (attrstate == ATTRSTATE_NO_NORMVAL) {
    // Print raw values only
    currstr = strprintf("%s (Raw)",
      ata_format_attr_raw_value(attr, cfg.attribute_defs).c_str());
    prevstr = strprintf("%s (Raw)",
      ata_format_attr_raw_value(prev, cfg.attribute_defs).c_str());
  }
  else if (cfg.monitor_attr_flags.is_set(attr.id, MONITOR_RAW_PRINT)) {
    // Print normalized and raw values
    currstr = strprintf("%d [Raw %s]", attr.current,
      ata_format_attr_raw_value(attr, cfg.attribute_defs).c_str());
    prevstr = strprintf("%d [Raw %s]", prev.current,
      ata_format_attr_raw_value(prev, cfg.attribute_defs).c_str());
  }
  else {
    // Print normalized values only
    currstr = strprintf("%d", attr.current);
    prevstr = strprintf("%d", prev.current);
  }

  // Format message
  std::string msg = strprintf("Device: %s, SMART %s Attribute: %d %s changed from %s to %s",
                              cfg.name.c_str(), (prefail ? "Prefailure" : "Usage"), attr.id,
                              ata_get_smart_attr_name(attr.id, cfg.attribute_defs, cfg.dev_rpm).c_str(),
                              prevstr.c_str(), currstr.c_str());

  // Report this change as critical ?
  if (   (valchanged && cfg.monitor_attr_flags.is_set(attr.id, MONITOR_AS_CRIT))
      || (rawchanged && cfg.monitor_attr_flags.is_set(attr.id, MONITOR_RAW_AS_CRIT))) {
    PrintOut(LOG_CRIT, "%s\n", msg.c_str());
    MailWarning(cfg, state, 2, "%s", msg.c_str());
  }
  else {
    PrintOut(LOG_INFO, "%s\n", msg.c_str());
  }
  state.must_write = true;
}


static int ATACheckDevice(const dev_config & cfg, dev_state & state, ata_device * atadev,
                          bool firstpass, bool allow_selftests)
{
  if (!open_device(cfg, state, atadev, "ATA"))
    return 1;

  const char * name = cfg.name.c_str();

  // user may have requested (with the -n Directive) to leave the disk
  // alone if it is in idle or sleeping mode.  In this case check the
  // power mode and exit without check if needed
  if (cfg.powermode && !state.powermodefail) {
    int dontcheck=0, powermode=ataCheckPowerMode(atadev);
    const char * mode = 0;
    if (0 <= powermode && powermode < 0xff) {
      // wait for possible spin up and check again
      int powermode2;
      sleep(5);
      powermode2 = ataCheckPowerMode(atadev);
      if (powermode2 > powermode)
        PrintOut(LOG_INFO, "Device: %s, CHECK POWER STATUS spins up disk (0x%02x -> 0x%02x)\n", name, powermode, powermode2);
      powermode = powermode2;
    }
        
    switch (powermode){
    case -1:
      // SLEEP
      mode="SLEEP";
      if (cfg.powermode>=1)
        dontcheck=1;
      break;
    case 0x00:
      // STANDBY
      mode="STANDBY";
      if (cfg.powermode>=2)
        dontcheck=1;
      break;
    case 0x01:
      // STANDBY_Y
      mode="STANDBY_Y";
      if (cfg.powermode>=2)
        dontcheck=1;
      break;
    case 0x80:
      // IDLE
      mode="IDLE";
      if (cfg.powermode>=3)
        dontcheck=1;
      break;
    case 0x81:
      // IDLE_A
      mode="IDLE_A";
      if (cfg.powermode>=3)
        dontcheck=1;
      break;
    case 0x82:
      // IDLE_B
      mode="IDLE_B";
      if (cfg.powermode>=3)
        dontcheck=1;
      break;
    case 0x83:
      // IDLE_C
      mode="IDLE_C";
      if (cfg.powermode>=3)
        dontcheck=1;
      break;
    case 0xff:
      // ACTIVE/IDLE
    case 0x40:
      // ACTIVE
    case 0x41:
      // ACTIVE
      mode="ACTIVE or IDLE";
      break;
    default:
      // UNKNOWN
      PrintOut(LOG_CRIT, "Device: %s, CHECK POWER STATUS returned %d, not ATA compliant, ignoring -n Directive\n",
        name, powermode);
      state.powermodefail = true;
      break;
    }

    // if we are going to skip a check, return now
    if (dontcheck){
      // skip at most powerskipmax checks
      if (!cfg.powerskipmax || state.powerskipcnt<cfg.powerskipmax) {
        CloseDevice(atadev, name);
        // report first only except if state has changed, avoid waking up system disk
        if ((!state.powerskipcnt || state.lastpowermodeskipped != powermode) && !cfg.powerquiet) {
          PrintOut(LOG_INFO, "Device: %s, is in %s mode, suspending checks\n", name, mode);
          state.lastpowermodeskipped = powermode;
        }
        state.powerskipcnt++;
        return 0;
      }
      else {
        PrintOut(LOG_INFO, "Device: %s, %s mode ignored due to reached limit of skipped checks (%d check%s skipped)\n",
          name, mode, state.powerskipcnt, (state.powerskipcnt==1?"":"s"));
      }
      state.powerskipcnt = 0;
      state.tempmin_delay = time(nullptr) + default_checktime - 60; // Delay Min Temperature update
    }
    else if (state.powerskipcnt) {
      PrintOut(LOG_INFO, "Device: %s, is back in %s mode, resuming checks (%d check%s skipped)\n",
        name, mode, state.powerskipcnt, (state.powerskipcnt==1?"":"s"));
      state.powerskipcnt = 0;
      state.tempmin_delay = time(nullptr) + default_checktime - 60; // Delay Min Temperature update
    }
  }

  // check smart status
  if (cfg.smartcheck) {
    int status=ataSmartStatus2(atadev);
    if (status==-1){
      PrintOut(LOG_INFO,"Device: %s, not capable of SMART self-check\n",name);
      MailWarning(cfg, state, 5, "Device: %s, not capable of SMART self-check", name);
      state.must_write = true;
    }
    else if (status==1){
      PrintOut(LOG_CRIT, "Device: %s, FAILED SMART self-check. BACK UP DATA NOW!\n", name);
      MailWarning(cfg, state, 1, "Device: %s, FAILED SMART self-check. BACK UP DATA NOW!", name);
      state.must_write = true;
    }
  }
  
  // Check everything that depends upon SMART Data (eg, Attribute values)
  if (   cfg.usagefailed || cfg.prefail || cfg.usage
      || cfg.curr_pending_id || cfg.offl_pending_id
      || cfg.tempdiff || cfg.tempinfo || cfg.tempcrit
      || cfg.selftest ||  cfg.offlinests || cfg.selfteststs) {

    // Read current attribute values.
    ata_smart_values curval;
    if (ataReadSmartValues(atadev, &curval)){
      PrintOut(LOG_CRIT, "Device: %s, failed to read SMART Attribute Data\n", name);
      MailWarning(cfg, state, 6, "Device: %s, failed to read SMART Attribute Data", name);
      state.must_write = true;
    }
    else {
      reset_warning_mail(cfg, state, 6, "read SMART Attribute Data worked again");

      // look for current or offline pending sectors
      if (cfg.curr_pending_id)
        check_pending(cfg, state, cfg.curr_pending_id, cfg.curr_pending_incr, curval, 10,
                      (!cfg.curr_pending_incr ? "Currently unreadable (pending) sectors"
                                              : "Total unreadable (pending) sectors"    ));

      if (cfg.offl_pending_id)
        check_pending(cfg, state, cfg.offl_pending_id, cfg.offl_pending_incr, curval, 11,
                      (!cfg.offl_pending_incr ? "Offline uncorrectable sectors"
                                              : "Total offline uncorrectable sectors"));

      // check temperature limits
      if (cfg.tempdiff || cfg.tempinfo || cfg.tempcrit)
        CheckTemperature(cfg, state, ata_return_temperature_value(&curval, cfg.attribute_defs), 0);

      // look for failed usage attributes, or track usage or prefail attributes
      if (cfg.usagefailed || cfg.prefail || cfg.usage) {
        for (int i = 0; i < NUMBER_ATA_SMART_ATTRIBUTES; i++) {
          check_attribute(cfg, state,
                          curval.vendor_attributes[i],
                          state.smartval.vendor_attributes[i],
                          i, state.smartthres.thres_entries);
        }
      }

      // Log changes of offline data collection status
      if (cfg.offlinests) {
        if (   curval.offline_data_collection_status
                != state.smartval.offline_data_collection_status
            || state.offline_started // test was started in previous call
            || (firstpass && (debugmode || (curval.offline_data_collection_status & 0x7d))))
          log_offline_data_coll_status(name, curval.offline_data_collection_status);
      }

      // Log changes of self-test execution status
      if (cfg.selfteststs) {
        if (   curval.self_test_exec_status != state.smartval.self_test_exec_status
            || state.selftest_started // test was started in previous call
            || (firstpass && (debugmode || (curval.self_test_exec_status & 0xf0))))
          log_self_test_exec_status(name, curval.self_test_exec_status);
      }

      // Save the new values for the next time around
      state.smartval = curval;
      state.update_persistent_state();
      state.attrlog_valid = 1; // ATA attributes valid
    }
  }
  state.offline_started = state.selftest_started = false;
  
  // check if number of selftest errors has increased (note: may also DECREASE)
  if (cfg.selftest) {
    unsigned hour = 0;
    int errcnt = check_ata_self_test_log(atadev, name, cfg.firmwarebugs, hour);
    report_self_test_log_changes(cfg, state, errcnt, hour);
  }

  // check if number of ATA errors has increased
  if (cfg.errorlog || cfg.xerrorlog) {

    int errcnt1 = -1, errcnt2 = -1;
    if (cfg.errorlog)
      errcnt1 = read_ata_error_count(atadev, name, cfg.firmwarebugs, false);
    if (cfg.xerrorlog)
      errcnt2 = read_ata_error_count(atadev, name, cfg.firmwarebugs, true);

    // new number of errors is max of both logs
    int newc = (errcnt1 >= errcnt2 ? errcnt1 : errcnt2);

    // did command fail?
    if (newc<0)
      // lack of PrintOut here is INTENTIONAL
      MailWarning(cfg, state, 7, "Device: %s, Read SMART Error Log Failed", name);

    // has error count increased?
    int oldc = state.ataerrorcount;
    if (newc>oldc){
      PrintOut(LOG_CRIT, "Device: %s, ATA error count increased from %d to %d\n",
               name, oldc, newc);
      MailWarning(cfg, state, 4, "Device: %s, ATA error count increased from %d to %d",
                   name, oldc, newc);
      state.must_write = true;
    }

    if (newc>=0)
      state.ataerrorcount=newc;
  }

  // if the user has asked, and device is capable (or we're not yet
  // sure) check whether a self test should be done now.
  if (allow_selftests && !cfg.test_regex.empty()) {
    char testtype = next_scheduled_test(cfg, state, false/*!scsi*/);
    if (testtype)
      DoATASelfTest(cfg, state, atadev, testtype);
  }

  // Don't leave device open -- the OS/user may want to access it
  // before the next smartd cycle!
  CloseDevice(atadev, name);
  return 0;
}

static int SCSICheckDevice(const dev_config & cfg, dev_state & state, scsi_device * scsidev, bool allow_selftests)
{
  if (!open_device(cfg, state, scsidev, "SCSI"))
    return 1;

  const char * name = cfg.name.c_str();

  uint8_t asc = 0, ascq = 0;
  uint8_t currenttemp = 0, triptemp = 0;
  if (!state.SuppressReport) {
    if (scsiCheckIE(scsidev, state.SmartPageSupported, state.TempPageSupported,
                    &asc, &ascq, &currenttemp, &triptemp)) {
      PrintOut(LOG_INFO, "Device: %s, failed to read SMART values\n",
               name);
      MailWarning(cfg, state, 6, "Device: %s, failed to read SMART values", name);
      state.SuppressReport = 1;
    }
  }
  if (asc > 0) {
    char b[128];
    const char * cp = scsiGetIEString(asc, ascq, b, sizeof(b));

    if (cp) {
      PrintOut(LOG_CRIT, "Device: %s, SMART Failure: %s\n", name, cp);
      MailWarning(cfg, state, 1,"Device: %s, SMART Failure: %s", name, cp);
    } else if (asc == 4 && ascq == 9) {
      PrintOut(LOG_INFO,"Device: %s, self-test in progress\n", name);
    } else if (debugmode)
      PrintOut(LOG_INFO,"Device: %s, non-SMART asc,ascq: %d,%d\n",
               name, (int)asc, (int)ascq);
  } else if (debugmode)
    PrintOut(LOG_INFO,"Device: %s, SMART health: passed\n", name);

  // check temperature limits
  if (cfg.tempdiff || cfg.tempinfo || cfg.tempcrit)
    CheckTemperature(cfg, state, currenttemp, triptemp);

  // check if number of selftest errors has increased (note: may also DECREASE)
  if (cfg.selftest) {
    int retval = scsiCountFailedSelfTests(scsidev, 0);
    report_self_test_log_changes(cfg, state, (retval >= 0 ? (retval & 0xff) : -1), retval >> 8);
  }

  if (allow_selftests && !cfg.test_regex.empty()) {
    char testtype = next_scheduled_test(cfg, state);
    if (testtype)
      DoSCSISelfTest(cfg, state, scsidev, testtype);
  }

  if (!cfg.attrlog_file.empty()){
    state.scsi_error_counters[0] = {};
    state.scsi_error_counters[1] = {};
    state.scsi_error_counters[2] = {};
    state.scsi_nonmedium_error = {};
    bool found = false;

    // saving error counters to state
    uint8_t tBuf[252];
    if (state.ReadECounterPageSupported && (0 == scsiLogSense(scsidev,
      READ_ERROR_COUNTER_LPAGE, 0, tBuf, sizeof(tBuf), 0))) {
      scsiDecodeErrCounterPage(tBuf, &state.scsi_error_counters[0].errCounter,
                               scsiLogRespLen);
      state.scsi_error_counters[0].found=1;
      found = true;
    }
    if (state.WriteECounterPageSupported && (0 == scsiLogSense(scsidev,
      WRITE_ERROR_COUNTER_LPAGE, 0, tBuf, sizeof(tBuf), 0))) {
      scsiDecodeErrCounterPage(tBuf, &state.scsi_error_counters[1].errCounter,
                               scsiLogRespLen);
      state.scsi_error_counters[1].found=1;
      found = true;
    }
    if (state.VerifyECounterPageSupported && (0 == scsiLogSense(scsidev,
      VERIFY_ERROR_COUNTER_LPAGE, 0, tBuf, sizeof(tBuf), 0))) {
      scsiDecodeErrCounterPage(tBuf, &state.scsi_error_counters[2].errCounter,
                               scsiLogRespLen);
      state.scsi_error_counters[2].found=1;
      found = true;
    }
    if (state.NonMediumErrorPageSupported && (0 == scsiLogSense(scsidev,
      NON_MEDIUM_ERROR_LPAGE, 0, tBuf, sizeof(tBuf), 0))) {
      scsiDecodeNonMediumErrPage(tBuf, &state.scsi_nonmedium_error.nme,
                                 scsiLogRespLen);
      state.scsi_nonmedium_error.found=1;
      found = true;
    }
    // store temperature if not done by CheckTemperature() above
    if (!(cfg.tempdiff || cfg.tempinfo || cfg.tempcrit))
      state.temperature = currenttemp;

    if (found || state.temperature)
      state.attrlog_valid = 2; // SCSI attributes valid
  }

  CloseDevice(scsidev, name);
  return 0;
}

// Log changes of a NVMe SMART/Health value
static void log_nvme_smart_change(const dev_config & cfg, dev_state & state,
  const char * valname, uint64_t oldval, uint64_t newval,
  bool critical, bool info = true)
{
  if (!(newval != oldval && (critical || info)))
    return;

  std::string msg = strprintf("Device: %s, SMART/Health value: %s changed "
                              "from %" PRIu64 " to %" PRIu64,
                              cfg.name.c_str(), valname, oldval, newval);
  if (!critical)
    PrintOut(LOG_INFO, "%s\n", msg.c_str());
  else {
    PrintOut(LOG_CRIT, "%s\n", msg.c_str());
    MailWarning(cfg, state, 2, "%s", msg.c_str());
  }
  state.must_write = true;
}

// Log NVMe self-test execution status changes
static void log_nvme_self_test_exec_status(const char * name, dev_state & state, bool firstpass,
                                           const nvme_self_test_log & self_test_log)
{
  uint8_t curr_op = self_test_log.current_operation & 0xf;
  uint8_t curr_compl = self_test_log.current_completion & 0x7f;

  // Return if no changes and log not forced
  if (!(   curr_op != state.selftest_op
        || curr_compl != state.selftest_compl
        || state.selftest_started // test was started in previous call
        || (firstpass && (debugmode || curr_op))))
    return;

  state.selftest_op = curr_op;
  state.selftest_compl = curr_compl;

  const nvme_self_test_result & r = self_test_log.results[0];
  uint8_t op0 = r.self_test_status >> 4, res0 = r.self_test_status & 0xf;

  uint8_t op = (curr_op ? curr_op : op0);
  const char * t; char tb[32];
  switch (op) {
    case 0x0: t = ""; break;
    case 0x1: t = "short"; break;
    case 0x2: t = "extended"; break;
    case 0xe: t = "vendor specific"; break;
    default:  snprintf(tb, sizeof(tb), "unknown (0x%x)", op);
              t = tb; break;
  }

  if (curr_op) {
    PrintOut(LOG_INFO, "Device %s, %s self-test in progress, %d%% remaining\n",
             name, t, 100 - curr_compl);
  }
  else if (!op0 || res0 == 0xf) { // First entry unused
    PrintOut(LOG_INFO, "Device %s, no self-test has ever been run\n", name);
  }
  else {
    // Report last test result from first log entry
    const char * m; char mb[48];
    switch (res0) {
      case 0x0: m = "completed without error"; break;
      case 0x1: m = "was aborted by a self-test command"; break;
      case 0x2: m = "was aborted by a controller reset"; break;
      case 0x3: m = "was aborted due to a namespace removal"; break;
      case 0x4: m = "was aborted by a format NVM command"; break;
      case 0x5: m = "completed with error (fatal or unknown error)"; break;
      case 0x6: m = "completed with error (unknown failed segment)"; break;
      case 0x7: m = "completed with error (failed segments)"; break;
      case 0x8: m = "was aborted (unknown reason)"; break;
      case 0x9: m = "was aborted due to a sanitize operation"; break;
      default:  snprintf(mb, sizeof(mb), "returned an unknown result (0x%x)", res0);
                m = mb; break;
    }

    char ns[32] = "";
    if (r.valid & 0x01)
      snprintf(ns, sizeof(ns), " of NSID 0x%x", r.nsid);

    PrintOut((0x5 <= res0 && res0 <= 0x7 ? LOG_CRIT : LOG_INFO),
             "Device %s, previous %s self-test%s %s\n", name, t, ns, m);
  }
}

// Count error entries in NVMe self-test log, set HOUR to power on hours of most
// recent error.  Return the error count.
static int check_nvme_self_test_log(uint32_t nsid, const nvme_self_test_log & self_test_log,
                                    uint64_t & hour)
{
  hour = 0;
  int errcnt = 0;

  for (unsigned i = 0; i < 20; i++) {
    const nvme_self_test_result & r = self_test_log.results[i];
    uint8_t op = r.self_test_status >> 4;
    uint8_t res = r.self_test_status & 0xf;
    if (!op || res == 0xf)
      continue; // Unused entry

    if (!(   nsid == nvme_broadcast_nsid
          || !(r.valid & 0x01) /* No NSID */
          || r.nsid == nvme_broadcast_nsid || r.nsid == nsid))
      continue; // Different individual namespace

    if (op == 0x2 /* Extended */ && !res /* Completed without error */)
      break; // Stop count at first successful extended test

    if (!(0x5 <= res && res <= 0x7))
      continue; // No error or aborted

    // Error found
    if (++errcnt != 1)
      continue; // Not most recent error

    // Keep track of time of most recent error
    hour = sg_get_unaligned_le64(r.power_on_hours);
  }

  return errcnt;
}

static int start_nvme_self_test(const dev_config & cfg, dev_state & state, nvme_device * device,
                                char testtype, const nvme_self_test_log & self_test_log)
{
  const char *name = cfg.name.c_str();
  unsigned nsid = device->get_nsid();

  const char *testname; uint8_t stc;
  switch (testtype) {
    case 'S': testname = "Short";    stc = 1; break;
    case 'L': testname = "Extended"; stc = 2; break;
    default: // Should not happen
      PrintOut(LOG_INFO, "Device: %s, not capable of %c Self-Test\n", name, testtype);
      return 1;
  }

  // If currently running a self-test, do not try to start another.
  if (self_test_log.current_operation & 0xf) {
    PrintOut(LOG_INFO, "Device: %s, skip scheduled %s Self-Test (NSID 0x%x); %d%% remaining of current Self-Test.\n",
             name, testname, nsid, 100 - (self_test_log.current_completion & 0x7f));
    return 1;
  }

  if (!nvme_self_test(device, stc, nsid)) {
    PrintOut(LOG_CRIT, "Device: %s, execute %s Self-Test failed (NSID 0x%x): %s.\n",
             name, testname, nsid, device->get_errmsg());
    return 1;
  }

  // Report recent test start to do_disable_standby_check()
  // and force log of next test status
  // TODO: Add NVMe support to do_disable_standby_check()
  state.selftest_started = true;

  PrintOut(LOG_INFO, "Device: %s, starting scheduled %s Self-Test (NSID 0x%x).\n",
           name, testname, nsid);
  return 0;
}

static int NVMeCheckDevice(const dev_config & cfg, dev_state & state, nvme_device * nvmedev, bool firstpass, bool allow_selftests)
{
  if (!open_device(cfg, state, nvmedev, "NVMe"))
    return 1;

  const char * name = cfg.name.c_str();

  // Read SMART/Health log
  // TODO: Support per namespace SMART/Health log
  nvme_smart_log smart_log;
  if (!nvme_read_smart_log(nvmedev, nvme_broadcast_nsid, smart_log)) {
      CloseDevice(nvmedev, name);
      PrintOut(LOG_INFO, "Device: %s, failed to read NVMe SMART/Health Information\n", name);
      MailWarning(cfg, state, 6, "Device: %s, failed to read NVMe SMART/Health Information", name);
      state.must_write = true;
      return 0;
  }

  // Check Critical Warning bits
  uint8_t w = smart_log.critical_warning, wm = w & cfg.smartcheck_nvme;
  if (wm) {
    std::string msg;
    static const char * const wnames[8] = {
      "LowSpare", "Temperature", "Reliability", "R/O",
      "VolMemBackup", "PersistMem", "Bit_6", "Bit_7"
    };

    for (unsigned b = 0, cnt = 0; b < 8 ; b++) {
      uint8_t mask = 1 << b;
      if (!(w & mask))
        continue;
      if (cnt)
        msg += ", ";
      if (++cnt > 3) {
        msg += "..."; break;
      }
      if (!(wm & mask))
         msg += '[';
      msg += wnames[b];
      if (!(wm & mask))
         msg += ']';
    }

    PrintOut(LOG_CRIT, "Device: %s, Critical Warning (0x%02x): %s\n", name, w, msg.c_str());
    MailWarning(cfg, state, 1, "Device: %s, Critical Warning (0x%02x): %s", name, w, msg.c_str());
    state.must_write = true;
  }

  // Check some SMART/Health values
  // Names similar to smartctl plaintext output
  if (cfg.prefail) {
    log_nvme_smart_change(cfg, state, "Available Spare",
      state.nvme_smartval.avail_spare, smart_log.avail_spare,
      (   smart_log.avail_spare < smart_log.spare_thresh
       && smart_log.spare_thresh <= 100 /* 101-255: "reserved" */));
  }

  if (cfg.usage || cfg.usagefailed) {
    log_nvme_smart_change(cfg, state, "Percentage Used",
      state.nvme_smartval.percent_used, smart_log.percent_used,
      (cfg.usagefailed && smart_log.percent_used > 95), cfg.usage);

    uint64_t old_me = le128_to_uint64(state.nvme_smartval.media_errors);
    uint64_t new_me = le128_to_uint64(smart_log.media_errors);
    log_nvme_smart_change(cfg, state, "Media and Data Integrity Errors",
      old_me, new_me, (cfg.usagefailed && new_me > old_me), cfg.usage);
  }

  // Check temperature limits
  if (cfg.tempdiff || cfg.tempinfo || cfg.tempcrit) {
    uint16_t k = sg_get_unaligned_le16(smart_log.temperature);
    // Convert Kelvin to positive Celsius (TODO: Allow negative temperatures)
    int c = (int)k - 273;
    if (c < 1)
      c = 1;
    else if (c > 0xff)
      c = 0xff;
    CheckTemperature(cfg, state, c, 0);
  }

  // Check for test schedule
  char testtype = (allow_selftests && !cfg.test_regex.empty()
                   ? next_scheduled_test(cfg, state) : 0);

  // Read the self-test log if required
  nvme_self_test_log self_test_log{};
  if (testtype || cfg.selftest || cfg.selfteststs) {
    if (!nvme_read_self_test_log(nvmedev, nvme_broadcast_nsid, self_test_log)) {
      PrintOut(LOG_CRIT, "Device: %s, Read Self-test Log failed: %s\n",
               name, nvmedev->get_errmsg());
      MailWarning(cfg, state, 8, "Device: %s, Read Self-test Log failed: %s\n",
                  name, nvmedev->get_errmsg());
      testtype = 0;
    }
    else {
      reset_warning_mail(cfg, state, 8, "Read Self-Test Log worked again");

      // Log changes of self-test execution status
      if (cfg.selfteststs)
        log_nvme_self_test_exec_status(name, state, firstpass, self_test_log);

      // Check if number of selftest errors has increased (note: may also DECREASE)
      if (cfg.selftest) {
        uint64_t hour = 0;
        int errcnt = check_nvme_self_test_log(nvmedev->get_nsid(), self_test_log, hour);
        report_self_test_log_changes(cfg, state, errcnt, hour);
      }
    }
  }
  state.selftest_started = false;

  // Check if number of errors has increased
  if (cfg.errorlog || cfg.xerrorlog) {
    uint64_t newcnt = le128_to_uint64(smart_log.num_err_log_entries);
    if (newcnt > state.nvme_err_log_entries) {
      // Warn only if device related errors are found
      check_nvme_error_log(cfg, state, nvmedev, newcnt);
    }
    // else // TODO: Handle decrease of count?
  }

  // Start self-test if scheduled
  if (testtype)
    start_nvme_self_test(cfg, state, nvmedev, testtype, self_test_log);

  CloseDevice(nvmedev, name);

  // Preserve new SMART/Health info for state file and attribute log
  state.nvme_smartval = smart_log;
  state.attrlog_valid = 3; // NVMe attributes valid
  return 0;
}

// 0=not used, 1=not disabled, 2=disable rejected by OS, 3=disabled
static int standby_disable_state = 0;

static void init_disable_standby_check(const dev_config_vector & configs)
{
  // Check for '-l offlinests,ns' or '-l selfteststs,ns' directives
  bool sts1 = false, sts2 = false;
  for (const auto & cfg : configs) {
    if (cfg.offlinests_ns)
      sts1 = true;
    if (cfg.selfteststs_ns)
      sts2 = true;
  }

  // Check for support of disable auto standby
  // Reenable standby if smartd.conf was reread
  if (sts1 || sts2 || standby_disable_state == 3) {
   if (!smi()->disable_system_auto_standby(false)) {
      if (standby_disable_state == 3)
        PrintOut(LOG_CRIT, "System auto standby enable failed: %s\n", smi()->get_errmsg());
      if (sts1 || sts2) {
        PrintOut(LOG_INFO, "Disable auto standby not supported, ignoring ',ns' from %s%s%s\n",
          (sts1 ? "-l offlinests,ns" : ""), (sts1 && sts2 ? " and " : ""), (sts2 ? "-l selfteststs,ns" : ""));
        sts1 = sts2 = false;
      }
    }
  }

  standby_disable_state = (sts1 || sts2 ? 1 : 0);
}

static void do_disable_standby_check(const dev_config_vector & configs, const dev_state_vector & states)
{
  if (!standby_disable_state)
    return;

  // Check for just started or still running self-tests
  bool running = false;
  for (unsigned i = 0; i < configs.size() && !running; i++) {
    const dev_config & cfg = configs.at(i); const dev_state & state = states.at(i);

    if (   (   cfg.offlinests_ns
            && (state.offline_started ||
                is_offl_coll_in_progress(state.smartval.offline_data_collection_status)))
        || (   cfg.selfteststs_ns
            && (state.selftest_started ||
                is_self_test_in_progress(state.smartval.self_test_exec_status)))         )
      running = true;
    // state.offline/selftest_started will be reset after next logging of test status
  }

  // Disable/enable auto standby and log state changes
  if (!running) {
    if (standby_disable_state != 1) {
      if (!smi()->disable_system_auto_standby(false))
        PrintOut(LOG_CRIT, "Self-test(s) completed, system auto standby enable failed: %s\n",
                 smi()->get_errmsg());
      else
        PrintOut(LOG_INFO, "Self-test(s) completed, system auto standby enabled\n");
      standby_disable_state = 1;
    }
  }
  else if (!smi()->disable_system_auto_standby(true)) {
    if (standby_disable_state != 2) {
      PrintOut(LOG_INFO, "Self-test(s) in progress, system auto standby disable rejected: %s\n",
               smi()->get_errmsg());
      standby_disable_state = 2;
    }
  }
  else {
    if (standby_disable_state != 3) {
      PrintOut(LOG_INFO, "Self-test(s) in progress, system auto standby disabled\n");
      standby_disable_state = 3;
    }
  }
}

// Checks the SMART status of all ATA and SCSI devices
static void CheckDevicesOnce(const dev_config_vector & configs, dev_state_vector & states,
                             smart_device_list & devices, bool firstpass, bool allow_selftests)
{
  for (unsigned i = 0; i < configs.size(); i++) {
    const dev_config & cfg = configs.at(i);
    dev_state & state = states.at(i);
    if (state.skip) {
      if (debugmode)
        PrintOut(LOG_INFO, "Device: %s, skipped (interval=%d)\n", cfg.name.c_str(),
                 (cfg.checktime ? cfg.checktime : checktime));
      continue;
    }

    smart_device * dev = devices.at(i);
    if (dev->is_ata())
      ATACheckDevice(cfg, state, dev->to_ata(), firstpass, allow_selftests);
    else if (dev->is_scsi())
      SCSICheckDevice(cfg, state, dev->to_scsi(), allow_selftests);
    else if (dev->is_nvme())
      NVMeCheckDevice(cfg, state, dev->to_nvme(), firstpass, allow_selftests);

    // Prevent systemd unit startup timeout when checking many devices on startup
    notify_extend_timeout();
  }

  do_disable_standby_check(configs, states);
}

// Install all signal handlers
static void install_signal_handlers()
{
  // normal and abnormal exit
  set_signal_if_not_ignored(SIGTERM, sighandler);
  set_signal_if_not_ignored(SIGQUIT, sighandler);
  
  // in debug mode, <CONTROL-C> ==> HUP
  set_signal_if_not_ignored(SIGINT, (debugmode ? HUPhandler : sighandler));
  
  // Catch HUP and USR1
  set_signal_if_not_ignored(SIGHUP, HUPhandler);
  set_signal_if_not_ignored(SIGUSR1, USR1handler);
#ifdef _WIN32
  set_signal_if_not_ignored(SIGUSR2, USR2handler);
#endif
}

#ifdef _WIN32
// Toggle debug mode implemented for native windows only
// (there is no easy way to reopen tty on *nix)
static void ToggleDebugMode()
{
  if (!debugmode) {
    PrintOut(LOG_INFO,"Signal USR2 - enabling debug mode\n");
    if (!daemon_enable_console("smartd [Debug]")) {
      debugmode = 1;
      daemon_signal(SIGINT, HUPhandler);
      PrintOut(LOG_INFO,"smartd debug mode enabled, PID=%d\n", getpid());
    }
    else
      PrintOut(LOG_INFO,"enable console failed\n");
  }
  else if (debugmode == 1) {
    daemon_disable_console();
    debugmode = 0;
    daemon_signal(SIGINT, sighandler);
    PrintOut(LOG_INFO,"Signal USR2 - debug mode disabled\n");
  }
  else
    PrintOut(LOG_INFO,"Signal USR2 - debug mode %d not changed\n", debugmode);
}
#endif

static time_t calc_next_wakeuptime(time_t wakeuptime, time_t timenow, int ct)
{
  if (timenow < wakeuptime)
    return wakeuptime;
  return timenow + ct - (timenow - wakeuptime) % ct;
}

static time_t dosleep(time_t wakeuptime, const dev_config_vector & configs,
  dev_state_vector & states, bool & sigwakeup)
{
  // If past wake-up-time, compute next wake-up-time
  time_t timenow = time(nullptr);
  unsigned n = configs.size();
  int ct;
  if (!checktime_min) {
    // Same for all devices
    wakeuptime = calc_next_wakeuptime(wakeuptime, timenow, checktime);
    ct = checktime;
  }
  else {
    // Determine wakeuptime of next device(s)
    wakeuptime = 0;
    for (unsigned i = 0; i < n; i++) {
      const dev_config & cfg = configs.at(i);
      dev_state & state = states.at(i);
      if (!state.skip)
        state.wakeuptime = calc_next_wakeuptime((state.wakeuptime ? state.wakeuptime : timenow),
          timenow, (cfg.checktime ? cfg.checktime : checktime));
      if (!wakeuptime || state.wakeuptime < wakeuptime)
        wakeuptime = state.wakeuptime;
    }
    ct = checktime_min;
  }

  notify_wait(wakeuptime, n);

  // Sleep until we catch a signal or have completed sleeping
  bool no_skip = false;
  int addtime = 0;
  while (timenow < wakeuptime+addtime && !caughtsigUSR1 && !caughtsigHUP && !caughtsigEXIT) {
    // Restart if system clock has been adjusted to the past
    if (wakeuptime > timenow + ct) {
      PrintOut(LOG_INFO, "System clock time adjusted to the past. Resetting next wakeup time.\n");
      wakeuptime = timenow + ct;
      for (auto & state : states)
        state.wakeuptime = 0;
      no_skip = true;
    }
    
    // Exit sleep when time interval has expired or a signal is received
    sleep(wakeuptime+addtime-timenow);

#ifdef _WIN32
    // toggle debug mode?
    if (caughtsigUSR2) {
      ToggleDebugMode();
      caughtsigUSR2 = 0;
    }
#endif

    timenow = time(nullptr);

    // Actual sleep time too long?
    if (!addtime && timenow > wakeuptime+60) {
      if (debugmode)
        PrintOut(LOG_INFO, "Sleep time was %d seconds too long, assuming wakeup from standby mode.\n",
          (int)(timenow-wakeuptime));
      // Wait another 20 seconds to avoid I/O errors during disk spin-up
      addtime = timenow-wakeuptime+20;
      // Use next wake-up-time if close
      int nextcheck = ct - addtime % ct;
      if (nextcheck <= 20)
        addtime += nextcheck;
    }
  }
 
  // if we caught a SIGUSR1 then print message and clear signal
  if (caughtsigUSR1){
    PrintOut(LOG_INFO,"Signal USR1 - checking devices now rather than in %d seconds.\n",
             wakeuptime-timenow>0?(int)(wakeuptime-timenow):0);
    caughtsigUSR1=0;
    sigwakeup = no_skip = true;
  }

  // Check which devices must be skipped in this cycle
  if (checktime_min) {
    for (auto & state : states)
      state.skip = (!no_skip && timenow < state.wakeuptime);
  }
  
  // return adjusted wakeuptime
  return wakeuptime;
}

// Print out a list of valid arguments for the Directive d
static void printoutvaliddirectiveargs(int priority, char d)
{
  switch (d) {
  case 'n':
    PrintOut(priority, "never[,N][,q], sleep[,N][,q], standby[,N][,q], idle[,N][,q]");
    break;
  case 's':
    PrintOut(priority, "valid_regular_expression");
    break;
  case 'd':
    PrintOut(priority, "%s", smi()->get_valid_dev_types_str().c_str());
    break;
  case 'T':
    PrintOut(priority, "normal, permissive");
    break;
  case 'o':
  case 'S':
    PrintOut(priority, "on, off");
    break;
  case 'l':
    PrintOut(priority, "error, selftest");
    break;
  case 'M':
    PrintOut(priority, "\"once\", \"always\", \"daily\", \"diminishing\", \"test\", \"exec\"");
    break;
  case 'v':
    PrintOut(priority, "\n%s\n", create_vendor_attribute_arg_list().c_str());
    break;
  case 'P':
    PrintOut(priority, "use, ignore, show, showall");
    break;
  case 'F':
    PrintOut(priority, "%s", get_valid_firmwarebug_args());
    break;
  case 'e':
    PrintOut(priority, "aam,[N|off], apm,[N|off], lookahead,[on|off], dsn,[on|off] "
                       "security-freeze, standby,[N|off], wcache,[on|off]");
    break;
  case 'c':
    PrintOut(priority, "i=N, interval=N");
    break;
  }
}

// exits with an error message, or returns integer value of token
static int GetInteger(const char *arg, const char *name, const char *token, int lineno, const char *cfgfile,
               int min, int max, char * suffix = 0)
{
  // make sure argument is there
  if (!arg) {
    PrintOut(LOG_CRIT,"File %s line %d (drive %s): Directive: %s takes integer argument from %d to %d.\n",
             cfgfile, lineno, name, token, min, max);
    return -1;
  }
  
  // get argument value (base 10), check that it's integer, and in-range
  char *endptr;
  int val = strtol(arg,&endptr,10);

  // optional suffix present?
  if (suffix) {
    if (!strcmp(endptr, suffix))
      endptr += strlen(suffix);
    else
      *suffix = 0;
  }

  if (!(!*endptr && min <= val && val <= max)) {
    PrintOut(LOG_CRIT,"File %s line %d (drive %s): Directive: %s has argument: %s; needs integer from %d to %d.\n",
             cfgfile, lineno, name, token, arg, min, max);
    return -1;
  }

  // all is well; return value
  return val;
}


// Get 1-3 small integer(s) for '-W' directive
static int Get3Integers(const char *arg, const char *name, const char *token, int lineno, const char *cfgfile,
                 unsigned char *val1, unsigned char *val2, unsigned char *val3)
{
  unsigned v1 = 0, v2 = 0, v3 = 0;
  int n1 = -1, n2 = -1, n3 = -1, len;
  if (!arg) {
    PrintOut(LOG_CRIT,"File %s line %d (drive %s): Directive: %s takes 1-3 integer argument(s) from 0 to 255.\n",
             cfgfile, lineno, name, token);
    return -1;
  }

  len = strlen(arg);
  if (!(   sscanf(arg, "%u%n,%u%n,%u%n", &v1, &n1, &v2, &n2, &v3, &n3) >= 1
        && (n1 == len || n2 == len || n3 == len) && v1 <= 255 && v2 <= 255 && v3 <= 255)) {
    PrintOut(LOG_CRIT,"File %s line %d (drive %s): Directive: %s has argument: %s; needs 1-3 integer(s) from 0 to 255.\n",
             cfgfile, lineno, name, token, arg);
    return -1;
  }
  *val1 = (unsigned char)v1; *val2 = (unsigned char)v2; *val3 = (unsigned char)v3;
  return 0;
}


#ifdef _WIN32

// Concatenate strtok() results if quoted with "..."
static const char * strtok_dequote(const char * delimiters)
{
  const char * t = strtok(nullptr, delimiters);
  if (!t || t[0] != '"')
    return t;

  static std::string token;
  token = t+1;
  for (;;) {
    t = strtok(nullptr, delimiters);
    if (!t || !*t)
      return "\"";
    token += ' ';
    int len = strlen(t);
    if (t[len-1] == '"') {
      token += std::string(t, len-1);
      break;
    }
    token += t;
  }
  return token.c_str();
}

#endif // _WIN32


// This function returns 1 if it has correctly parsed one token (and
// any arguments), else zero if no tokens remain.  It returns -1 if an
// error was encountered.
static int ParseToken(char * & token, dev_config & cfg, smart_devtype_list & scan_types)
{
  char sym;
  const char * name = cfg.name.c_str();
  int lineno=cfg.lineno;
  const char *delim = " \n\t";
  int badarg = 0;
  int missingarg = 0;
  const char *arg = 0;

  // Get next token unless lookahead (from '-H') is available
  if (!token) {
    token = strtok(nullptr, delim);
    if (!token)
      return 0;
  }

  // is the rest of the line a comment
  if (*token=='#')
    return 1;
  
  // is the token not recognized?
  if (*token!='-' || strlen(token)!=2) {
    PrintOut(LOG_CRIT,"File %s line %d (drive %s): unknown Directive: %s\n",
             configfile, lineno, name, token);
    PrintOut(LOG_CRIT, "Run smartd -D to print a list of valid Directives.\n");
    return -1;
  }
  
  // token we will be parsing:
  sym=token[1];

  // parse the token and swallow its argument
  int val;
  char plus[] = "+", excl[] = "!";

  switch (sym) {
  case 'C':
    // monitor current pending sector count (default 197)
    if ((val = GetInteger((arg = strtok(nullptr, delim)), name, token, lineno, configfile, 0, 255, plus)) < 0)
      return -1;
    cfg.curr_pending_id = (unsigned char)val;
    cfg.curr_pending_incr = (*plus == '+');
    cfg.curr_pending_set = true;
    break;
  case 'U':
    // monitor offline uncorrectable sectors (default 198)
    if ((val = GetInteger((arg = strtok(nullptr, delim)), name, token, lineno, configfile, 0, 255, plus)) < 0)
      return -1;
    cfg.offl_pending_id = (unsigned char)val;
    cfg.offl_pending_incr = (*plus == '+');
    cfg.offl_pending_set = true;
    break;
  case 'T':
    // Set tolerance level for SMART command failures
    if (!(arg = strtok(nullptr, delim))) {
      missingarg = 1;
    } else if (!strcmp(arg, "normal")) {
      // Normal mode: exit on failure of a mandatory S.M.A.R.T. command, but
      // not on failure of an optional S.M.A.R.T. command.
      // This is the default so we don't need to actually do anything here.
      cfg.permissive = false;
    } else if (!strcmp(arg, "permissive")) {
      // Permissive mode; ignore errors from Mandatory SMART commands
      cfg.permissive = true;
    } else {
      badarg = 1;
    }
    break;
  case 'd':
    // specify the device type
    if (!(arg = strtok(nullptr, delim))) {
      missingarg = 1;
    } else if (!strcmp(arg, "ignore")) {
      cfg.ignore = true;
    } else if (!strcmp(arg, "removable")) {
      cfg.removable = true;
    } else if (!strcmp(arg, "auto")) {
      cfg.dev_type = "";
      scan_types.clear();
    } else {
      cfg.dev_type = arg;
      scan_types.push_back(arg);
    }
    break;
  case 'F':
    // fix firmware bug
    if (!(arg = strtok(nullptr, delim)))
      missingarg = 1;
    else if (!parse_firmwarebug_def(arg, cfg.firmwarebugs))
      badarg = 1;
    break;
  case 'H':
    // check SMART status
    cfg.smartcheck = true;
    cfg.smartcheck_nvme = 0xff;
    // Lookahead for optional NVMe bitmask
    {
      char * next_token = strtok(nullptr, delim);
      if (!next_token)
        return 0;
      if (*next_token == '-') {
        // Continue with next directive
        token = next_token;
        return 1;
      }
      arg = next_token;
      unsigned u = ~0; int nc = -1;
      sscanf(arg, "0x%x%n", &u, &nc);
      if (nc == (int)strlen(arg) && u <= 0xff)
        cfg.smartcheck_nvme = (uint8_t)u;
      else
        badarg = 1;
    }
    break;
  case 'f':
    // check for failure of usage attributes
    cfg.usagefailed = true;
    break;
  case 't':
    // track changes in all vendor attributes
    cfg.prefail = true;
    cfg.usage = true;
    break;
  case 'p':
    // track changes in prefail vendor attributes
    cfg.prefail = true;
    break;
  case 'u':
    //  track changes in usage vendor attributes
    cfg.usage = true;
    break;
  case 'l':
    // track changes in SMART logs
    if (!(arg = strtok(nullptr, delim))) {
      missingarg = 1;
    } else if (!strcmp(arg, "selftest")) {
      // track changes in self-test log
      cfg.selftest = true;
    } else if (!strcmp(arg, "error")) {
      // track changes in ATA error log
      cfg.errorlog = true;
    } else if (!strcmp(arg, "xerror")) {
      // track changes in Extended Comprehensive SMART error log
      cfg.xerrorlog = true;
    } else if (!strcmp(arg, "offlinests")) {
      // track changes in offline data collection status
      cfg.offlinests = true;
    } else if (!strcmp(arg, "offlinests,ns")) {
      // track changes in offline data collection status, disable auto standby
      cfg.offlinests = cfg.offlinests_ns = true;
    } else if (!strcmp(arg, "selfteststs")) {
      // track changes in self-test execution status
      cfg.selfteststs = true;
    } else if (!strcmp(arg, "selfteststs,ns")) {
      // track changes in self-test execution status, disable auto standby
      cfg.selfteststs = cfg.selfteststs_ns = true;
    } else if (!strncmp(arg, "scterc,", sizeof("scterc,")-1)) {
        // set SCT Error Recovery Control
        unsigned rt = ~0, wt = ~0; int nc = -1;
        sscanf(arg,"scterc,%u,%u%n", &rt, &wt, &nc);
        if (nc == (int)strlen(arg) && rt <= 999 && wt <= 999) {
          cfg.sct_erc_set = true;
          cfg.sct_erc_readtime = rt;
          cfg.sct_erc_writetime = wt;
        }
        else
          badarg = 1;
    } else {
      badarg = 1;
    }
    break;
  case 'a':
    // monitor everything
    cfg.smartcheck = true;
    cfg.smartcheck_nvme = 0xff;
    cfg.prefail = true;
    cfg.usagefailed = true;
    cfg.usage = true;
    cfg.selftest = true;
    cfg.errorlog = true;
    cfg.selfteststs = true;
    break;
  case 'o':
    // automatic offline testing enable/disable
    if (!(arg = strtok(nullptr, delim))) {
      missingarg = 1;
    } else if (!strcmp(arg, "on")) {
      cfg.autoofflinetest = 2;
    } else if (!strcmp(arg, "off")) {
      cfg.autoofflinetest = 1;
    } else {
      badarg = 1;
    }
    break;
  case 'n':
    // skip disk check if in idle or standby mode
    if (!(arg = strtok(nullptr, delim)))
      missingarg = 1;
    else {
      char *endptr = nullptr;
      char *next = strchr(const_cast<char*>(arg), ',');

      cfg.powerquiet = false;
      cfg.powerskipmax = 0;

      if (next)
        *next = '\0';
      if (!strcmp(arg, "never"))
        cfg.powermode = 0;
      else if (!strcmp(arg, "sleep"))
        cfg.powermode = 1;
      else if (!strcmp(arg, "standby"))
        cfg.powermode = 2;
      else if (!strcmp(arg, "idle"))
        cfg.powermode = 3;
      else
        badarg = 1;

      // if optional arguments are present
      if (!badarg && next) {
        next++;
        cfg.powerskipmax = strtol(next, &endptr, 10);
        if (endptr == next)
          cfg.powerskipmax = 0;
        else {
          next = endptr + (*endptr != '\0');
          if (cfg.powerskipmax <= 0)
            badarg = 1;
        }
        if (*next != '\0') {
          if (!strcmp("q", next))
            cfg.powerquiet = true;
          else {
            badarg = 1;
          }
        }
      }
    }
    break;
  case 'S':
    // automatic attribute autosave enable/disable
    if (!(arg = strtok(nullptr, delim))) {
      missingarg = 1;
    } else if (!strcmp(arg, "on")) {
      cfg.autosave = 2;
    } else if (!strcmp(arg, "off")) {
      cfg.autosave = 1;
    } else {
      badarg = 1;
    }
    break;
  case 's':
    // warn user, and delete any previously given -s REGEXP Directives
    if (!cfg.test_regex.empty()){
      PrintOut(LOG_INFO, "File %s line %d (drive %s): ignoring previous Test Directive -s %s\n",
               configfile, lineno, name, cfg.test_regex.get_pattern());
      cfg.test_regex = regular_expression();
    }
    // check for missing argument
    if (!(arg = strtok(nullptr, delim))) {
      missingarg = 1;
    }
    // Compile regex
    else {
      if (!cfg.test_regex.compile(arg)) {
        // not a valid regular expression!
        PrintOut(LOG_CRIT, "File %s line %d (drive %s): -s argument \"%s\" is INVALID extended regular expression. %s.\n",
                 configfile, lineno, name, arg, cfg.test_regex.get_errmsg());
        return -1;
      }
      // Do a bit of sanity checking and warn user if we think that
      // their regexp is "strange". User probably confused about shell
      // glob(3) syntax versus regular expression syntax regexp(7).
      // Check also for possible invalid number of digits in ':NNN[-LLL]' suffix.
      static const regular_expression syntax_check(
        "[^]$()*+./:?^[|0-9LSCOncr-]+|"
        ":[0-9]{0,2}($|[^0-9])|:[0-9]{4,}|"
        ":[0-9]{3}-(000|[0-9]{0,2}($|[^0-9])|[0-9]{4,})"
      );
      regular_expression::match_range range;
      if (syntax_check.execute(arg, 1, &range) && 0 <= range.rm_so && range.rm_so < range.rm_eo)
        PrintOut(LOG_INFO,  "File %s line %d (drive %s): warning, \"%.*s\" looks odd in "
                            "extended regular expression \"%s\"\n",
                 configfile, lineno, name, (int)(range.rm_eo - range.rm_so), arg + range.rm_so, arg);
    }
    break;
  case 'm':
    // send email to address that follows
    if (!(arg = strtok(nullptr, delim)))
      missingarg = 1;
    else {
      if (!cfg.emailaddress.empty())
        PrintOut(LOG_INFO, "File %s line %d (drive %s): ignoring previous Address Directive -m %s\n",
                 configfile, lineno, name, cfg.emailaddress.c_str());
      cfg.emailaddress = arg;
    }
    break;
  case 'M':
    // email warning options
    if (!(arg = strtok(nullptr, delim)))
      missingarg = 1;
    else if (!strcmp(arg, "once"))
      cfg.emailfreq = emailfreqs::once;
    else if (!strcmp(arg, "always"))
      cfg.emailfreq = emailfreqs::always;
    else if (!strcmp(arg, "daily"))
      cfg.emailfreq = emailfreqs::daily;
    else if (!strcmp(arg, "diminishing"))
      cfg.emailfreq = emailfreqs::diminishing;
    else if (!strcmp(arg, "test"))
      cfg.emailtest = true;
    else if (!strcmp(arg, "exec")) {
      // Get the next argument (the command line)
#ifdef _WIN32
      // Allow "/path name/with spaces/..." on Windows
      arg = strtok_dequote(delim);
      if (arg && arg[0] == '"') {
        PrintOut(LOG_CRIT, "File %s line %d (drive %s): Directive %s 'exec' argument: missing closing quote\n",
                 configfile, lineno, name, token);
        return -1;
      }
#else
      arg = strtok(nullptr, delim);
#endif
      if (!arg) {
        PrintOut(LOG_CRIT, "File %s line %d (drive %s): Directive %s 'exec' argument must be followed by executable path.\n",
                 configfile, lineno, name, token);
        return -1;
      }
      // Free the last cmd line given if any, and copy new one
      if (!cfg.emailcmdline.empty())
        PrintOut(LOG_INFO, "File %s line %d (drive %s): ignoring previous mail Directive -M exec %s\n",
                 configfile, lineno, name, cfg.emailcmdline.c_str());
      cfg.emailcmdline = arg;
    } 
    else
      badarg = 1;
    break;
  case 'i':
    // ignore failure of usage attribute
    if ((val = GetInteger((arg = strtok(nullptr, delim)), name, token, lineno, configfile, 1, 255)) < 0)
      return -1;
    cfg.monitor_attr_flags.set(val, MONITOR_IGN_FAILUSE);
    break;
  case 'I':
    // ignore attribute for tracking purposes
    if ((val = GetInteger((arg = strtok(nullptr, delim)), name, token, lineno, configfile, 1, 255)) < 0)
      return -1;
    cfg.monitor_attr_flags.set(val, MONITOR_IGNORE);
    break;
  case 'r':
    // print raw value when tracking
    if ((val = GetInteger((arg = strtok(nullptr, delim)), name, token, lineno, configfile, 1, 255, excl)) < 0)
      return -1;
    cfg.monitor_attr_flags.set(val, MONITOR_RAW_PRINT);
    if (*excl == '!') // attribute change is critical
      cfg.monitor_attr_flags.set(val, MONITOR_AS_CRIT);
    break;
  case 'R':
    // track changes in raw value (forces printing of raw value)
    if ((val = GetInteger((arg = strtok(nullptr, delim)), name, token, lineno, configfile, 1, 255, excl)) < 0)
      return -1;
    cfg.monitor_attr_flags.set(val, MONITOR_RAW_PRINT|MONITOR_RAW);
    if (*excl == '!') // raw value change is critical
      cfg.monitor_attr_flags.set(val, MONITOR_RAW_AS_CRIT);
    break;
  case 'W':
    // track Temperature
    if (Get3Integers((arg = strtok(nullptr, delim)), name, token, lineno, configfile,
                     &cfg.tempdiff, &cfg.tempinfo, &cfg.tempcrit) < 0)
      return -1;
    break;
  case 'v':
    // non-default vendor-specific attribute meaning
    if (!(arg = strtok(nullptr, delim))) {
      missingarg = 1;
    } else if (!parse_attribute_def(arg, cfg.attribute_defs, PRIOR_USER)) {
      badarg = 1;
    }
    break;
  case 'P':
    // Define use of drive-specific presets.
    if (!(arg = strtok(nullptr, delim))) {
      missingarg = 1;
    } else if (!strcmp(arg, "use")) {
      cfg.ignorepresets = false;
    } else if (!strcmp(arg, "ignore")) {
      cfg.ignorepresets = true;
    } else if (!strcmp(arg, "show")) {
      cfg.showpresets = true;
    } else if (!strcmp(arg, "showall")) {
      showallpresets();
    } else {
      badarg = 1;
    }
    break;

  case 'e':
    // Various ATA settings
    if (!(arg = strtok(nullptr, delim))) {
      missingarg = true;
    }
    else {
      char arg2[16+1]; unsigned uval;
      int n1 = -1, n2 = -1, n3 = -1, len = strlen(arg);
      if (sscanf(arg, "%16[^,=]%n%*[,=]%n%u%n", arg2, &n1, &n2, &uval, &n3) >= 1
          && (n1 == len || n2 > 0)) {
        bool on  = (n2 > 0 && !strcmp(arg+n2, "on"));
        bool off = (n2 > 0 && !strcmp(arg+n2, "off"));
        if (n3 != len)
          uval = ~0U;

        if (!strcmp(arg2, "aam")) {
          if (off)
            cfg.set_aam = -1;
          else if (uval <= 254)
            cfg.set_aam = uval + 1;
          else
            badarg = true;
        }
        else if (!strcmp(arg2, "apm")) {
          if (off)
            cfg.set_apm = -1;
          else if (1 <= uval && uval <= 254)
            cfg.set_apm = uval + 1;
          else
            badarg = true;
        }
        else if (!strcmp(arg2, "lookahead")) {
          if (off)
            cfg.set_lookahead = -1;
          else if (on)
            cfg.set_lookahead = 1;
          else
            badarg = true;
        }
        else if (!strcmp(arg, "security-freeze")) {
          cfg.set_security_freeze = true;
        }
        else if (!strcmp(arg2, "standby")) {
          if (off)
            cfg.set_standby = 0 + 1;
          else if (uval <= 255)
            cfg.set_standby = uval + 1;
          else
            badarg = true;
        }
        else if (!strcmp(arg2, "wcache")) {
          if (off)
            cfg.set_wcache = -1;
          else if (on)
            cfg.set_wcache = 1;
          else
            badarg = true;
        }
        else if (!strcmp(arg2, "dsn")) {
          if (off)
            cfg.set_dsn = -1;
          else if (on)
            cfg.set_dsn = 1;
          else
            badarg = true;
        }
        else
          badarg = true;
      }
      else
        badarg = true;
    }
    break;

  case 'c':
    // Override command line options
    {
      if (!(arg = strtok(nullptr, delim))) {
        missingarg = true;
        break;
      }
      int n = 0, nc = -1, len = strlen(arg);
      if (   (   sscanf(arg, "i=%d%n", &n, &nc) == 1
              || sscanf(arg, "interval=%d%n", &n, &nc) == 1)
          && nc == len && n >= 10)
        cfg.checktime = n;
      else
        badarg = true;
    }
    break;

  default:
    // Directive not recognized
    PrintOut(LOG_CRIT,"File %s line %d (drive %s): unknown Directive: %s\n",
             configfile, lineno, name, token);
    PrintOut(LOG_CRIT, "Run smartd -D to print a list of valid Directives.\n");
    return -1;
  }
  if (missingarg) {
    PrintOut(LOG_CRIT, "File %s line %d (drive %s): Missing argument to %s Directive\n",
             configfile, lineno, name, token);
  }
  if (badarg) {
    PrintOut(LOG_CRIT, "File %s line %d (drive %s): Invalid argument to %s Directive: %s\n",
             configfile, lineno, name, token, arg);
  }
  if (missingarg || badarg) {
    PrintOut(LOG_CRIT, "Valid arguments to %s Directive are: ", token);
    printoutvaliddirectiveargs(LOG_CRIT, sym);
    PrintOut(LOG_CRIT, "\n");
    return -1;
  }

  // Continue with no lookahead
  token = nullptr;
  return 1;
}

// Scan directive for configuration file
#define SCANDIRECTIVE "DEVICESCAN"

// This is the routine that adds things to the conf_entries list.
//
// Return values are:
//  1: parsed a normal line
//  0: found DEFAULT setting or comment or blank line
// -1: found SCANDIRECTIVE line
// -2: found an error
//
// Note: this routine modifies *line from the caller!
static int ParseConfigLine(dev_config_vector & conf_entries, dev_config & default_conf,
  smart_devtype_list & scan_types, int lineno, /*const*/ char * line)
{
  const char *delim = " \n\t";

  // get first token: device name. If a comment, skip line
  const char * name = strtok(line, delim);
  if (!name || *name == '#')
    return 0;

  // Check device name for DEFAULT or DEVICESCAN
  int retval;
  if (!strcmp("DEFAULT", name)) {
    retval = 0;
    // Restart with empty defaults
    default_conf = dev_config();
  }
  else {
    retval = (!strcmp(SCANDIRECTIVE, name) ? -1 : 1);
    // Init new entry with current defaults
    conf_entries.push_back(default_conf);
  }
  dev_config & cfg = (retval ? conf_entries.back() : default_conf);

  cfg.name = name; // Later replaced by dev->get_info().info_name
  cfg.dev_name = name; // If DEVICESCAN later replaced by get->dev_info().dev_name
  cfg.lineno = lineno;

  // parse tokens one at a time from the file.
  int rc;
  for (char * token = nullptr; (rc = ParseToken(token, cfg, scan_types)) != 0; ) {
    if (rc < 0)
      // error found on the line
      return -2;
  }

  // Check for multiple -d TYPE directives
  if (retval != -1 && scan_types.size() > 1) {
    PrintOut(LOG_CRIT, "Drive: %s, invalid multiple -d TYPE Directives on line %d of file %s\n",
             cfg.name.c_str(), cfg.lineno, configfile);
    return -2;
  }

  // Don't perform checks below for DEFAULT entries
  if (retval == 0)
    return retval;

  // If NO monitoring directives are set, then set all of them.
  if (!(   cfg.smartcheck  || cfg.selftest
        || cfg.errorlog    || cfg.xerrorlog
        || cfg.offlinests  || cfg.selfteststs
        || cfg.usagefailed || cfg.prefail  || cfg.usage
        || cfg.tempdiff    || cfg.tempinfo || cfg.tempcrit)) {
    
    PrintOut(LOG_INFO,"Drive: %s, implied '-a' Directive on line %d of file %s\n",
             cfg.name.c_str(), cfg.lineno, configfile);
    
    cfg.smartcheck = true;
    cfg.smartcheck_nvme = 0xff;
    cfg.usagefailed = true;
    cfg.prefail = true;
    cfg.usage = true;
    cfg.selftest = true;
    cfg.errorlog = true;
    cfg.selfteststs = true;
  }
  
  // additional sanity check. Has user set -M options without -m?
  if (   cfg.emailaddress.empty()
      && (!cfg.emailcmdline.empty() || cfg.emailfreq != emailfreqs::unknown || cfg.emailtest)) {
    PrintOut(LOG_CRIT,"Drive: %s, -M Directive(s) on line %d of file %s need -m ADDRESS Directive\n",
             cfg.name.c_str(), cfg.lineno, configfile);
    return -2;
  }
  
  // has the user has set <nomailer>?
  if (cfg.emailaddress == "<nomailer>") {
    // check that -M exec is also set
    if (cfg.emailcmdline.empty()){
      PrintOut(LOG_CRIT,"Drive: %s, -m <nomailer> Directive on line %d of file %s needs -M exec Directive\n",
               cfg.name.c_str(), cfg.lineno, configfile);
      return -2;
    }
    // From here on the sign of <nomailer> is cfg.emailaddress.empty() and !cfg.emailcmdline.empty()
    cfg.emailaddress.clear();
  }

  return retval;
}

// Parses a configuration file.  Return values are:
//  N=>0: found N entries
// -1:    syntax error in config file
// -2:    config file does not exist
// -3:    config file exists but cannot be read
//
// In the case where the return value is 0, there are three
// possibilities:
// Empty configuration file ==> conf_entries.empty()
// No configuration file    ==> conf_entries[0].lineno == 0
// SCANDIRECTIVE found      ==> conf_entries.back().lineno != 0 (size >= 1)
static int ParseConfigFile(dev_config_vector & conf_entries, smart_devtype_list & scan_types)
{
  // maximum line length in configuration file
  const int MAXLINELEN = 256;
  // maximum length of a continued line in configuration file
  const int MAXCONTLINE = 1023;

  stdio_file f;
  // Open config file, if it exists and is not <stdin>
  if (!(configfile == configfile_stdin)) { // pointer comparison ok here
    if (!f.open(configfile,"r") && (errno!=ENOENT || !configfile_alt.empty())) {
      // file exists but we can't read it or it should exist due to '-c' option
      int ret = (errno!=ENOENT ? -3 : -2);
      PrintOut(LOG_CRIT,"%s: Unable to open configuration file %s\n",
               strerror(errno),configfile);
      return ret;
    }
  }
  else // read from stdin ('-c -' option)
    f.open(stdin);

  // Start with empty defaults
  dev_config default_conf;

  // No configuration file found -- use fake one
  int entry = 0;
  if (!f) {
    char fakeconfig[] = SCANDIRECTIVE " -a"; // TODO: Remove this hack, build cfg_entry.

    if (ParseConfigLine(conf_entries, default_conf, scan_types, 0, fakeconfig) != -1)
      throw std::logic_error("Internal error parsing " SCANDIRECTIVE);
    return 0;
  }

#ifdef __CYGWIN__
  setmode(fileno(f), O_TEXT); // Allow files with \r\n
#endif

  // configuration file exists
  PrintOut(LOG_INFO,"Opened configuration file %s\n",configfile);

  // parse config file line by line
  int lineno = 1, cont = 0, contlineno = 0;
  char line[MAXLINELEN+2];
  char fullline[MAXCONTLINE+1];

  for (;;) {
    int len=0,scandevice;
    char *lastslash;
    char *comment;
    char *code;

    // make debugging simpler
    memset(line,0,sizeof(line));

    // get a line
    code=fgets(line, MAXLINELEN+2, f);
    
    // are we at the end of the file?
    if (!code){
      if (cont) {
        scandevice = ParseConfigLine(conf_entries, default_conf, scan_types, contlineno, fullline);
        // See if we found a SCANDIRECTIVE directive
        if (scandevice==-1)
          return 0;
        // did we find a syntax error
        if (scandevice==-2)
          return -1;
        // the final line is part of a continuation line
        entry+=scandevice;
      }
      break;
    }

    // input file line number
    contlineno++;
    
    // See if line is too long
    len=strlen(line);
    if (len>MAXLINELEN){
      const char *warn;
      if (line[len-1]=='\n')
        warn="(including newline!) ";
      else
        warn="";
      PrintOut(LOG_CRIT,"Error: line %d of file %s %sis more than MAXLINELEN=%d characters.\n",
               (int)contlineno,configfile,warn,(int)MAXLINELEN);
      return -1;
    }

    // Ignore anything after comment symbol
    if ((comment=strchr(line,'#'))){
      *comment='\0';
      len=strlen(line);
    }

    // is the total line (made of all continuation lines) too long?
    if (cont+len>MAXCONTLINE){
      PrintOut(LOG_CRIT,"Error: continued line %d (actual line %d) of file %s is more than MAXCONTLINE=%d characters.\n",
               lineno, (int)contlineno, configfile, (int)MAXCONTLINE);
      return -1;
    }
    
    // copy string so far into fullline, and increment length
    snprintf(fullline+cont, sizeof(fullline)-cont, "%s" ,line);
    cont+=len;

    // is this a continuation line.  If so, replace \ by space and look at next line
    if ( (lastslash=strrchr(line,'\\')) && !strtok(lastslash+1," \n\t")){
      *(fullline+(cont-len)+(lastslash-line))=' ';
      continue;
    }

    // Not a continuation line. Parse it
    scan_types.clear();
    scandevice = ParseConfigLine(conf_entries, default_conf, scan_types, contlineno, fullline);

    // did we find a scandevice directive?
    if (scandevice==-1)
      return 0;
    // did we find a syntax error
    if (scandevice==-2)
      return -1;

    entry+=scandevice;
    lineno++;
    cont=0;
  }

  // note -- may be zero if syntax of file OK, but no valid entries!
  return entry;
}

/* Prints the message "=======> VALID ARGUMENTS ARE: <LIST>  <=======\n", where
   <LIST> is the list of valid arguments for option opt. */
static void PrintValidArgs(char opt)
{
  const char *s;

  PrintOut(LOG_CRIT, "=======> VALID ARGUMENTS ARE: ");
  if (!(s = GetValidArgList(opt)))
    PrintOut(LOG_CRIT, "Error constructing argument list for option %c", opt);
  else
    PrintOut(LOG_CRIT, "%s", (char *)s);
  PrintOut(LOG_CRIT, " <=======\n");
}

#ifndef _WIN32
// Report error and return false if specified path is not absolute.
static bool check_abs_path(char option, const std::string & path)
{
  if (path.empty() || path[0] == '/')
    return true;

  debugmode = 1;
  PrintHead();
  PrintOut(LOG_CRIT, "=======> INVALID ARGUMENT TO -%c: %s <=======\n\n", option, path.c_str());
  PrintOut(LOG_CRIT, "Error: relative path names are not allowed\n\n");
  return false;
}
#endif // !_WIN32

// Parses input line, prints usage message and
// version/license/copyright messages
static int parse_options(int argc, char **argv)
{
  // Init default path names
#ifndef _WIN32
  configfile = SMARTMONTOOLS_SYSCONFDIR "/smartd.conf";
  warning_script = SMARTMONTOOLS_SMARTDSCRIPTDIR "/smartd_warning.sh";
#else
  std::string exedir = get_exe_dir();
  static std::string configfile_str = exedir + "/smartd.conf";
  configfile = configfile_str.c_str();
  warning_script = exedir + "/smartd_warning.cmd";
#endif

  // Please update GetValidArgList() if you edit shortopts
  static const char shortopts[] = "c:l:q:dDni:p:r:s:A:B:w:Vh?"
#if defined(HAVE_POSIX_API) || defined(_WIN32)
                                                          "u:"
#endif
#ifdef HAVE_LIBCAP_NG
                                                          "C"
#endif
                                                             ;
  // Please update GetValidArgList() if you edit longopts
  struct option longopts[] = {
    { "configfile",     required_argument, 0, 'c' },
    { "logfacility",    required_argument, 0, 'l' },
    { "quit",           required_argument, 0, 'q' },
    { "debug",          no_argument,       0, 'd' },
    { "showdirectives", no_argument,       0, 'D' },
    { "interval",       required_argument, 0, 'i' },
#ifndef _WIN32
    { "no-fork",        no_argument,       0, 'n' },
#else
    { "service",        no_argument,       0, 'n' },
#endif
    { "pidfile",        required_argument, 0, 'p' },
    { "report",         required_argument, 0, 'r' },
    { "savestates",     required_argument, 0, 's' },
    { "attributelog",   required_argument, 0, 'A' },
    { "drivedb",        required_argument, 0, 'B' },
    { "warnexec",       required_argument, 0, 'w' },
    { "version",        no_argument,       0, 'V' },
    { "license",        no_argument,       0, 'V' },
    { "copyright",      no_argument,       0, 'V' },
    { "help",           no_argument,       0, 'h' },
    { "usage",          no_argument,       0, 'h' },
#if defined(HAVE_POSIX_API) || defined(_WIN32)
    { "warn-as-user",   required_argument, 0, 'u' },
#endif
#ifdef HAVE_LIBCAP_NG
    { "capabilities",   optional_argument, 0, 'C' },
#endif
    { 0,                0,                 0, 0   }
  };

  opterr=optopt=0;
  bool badarg = false;
  const char * badarg_msg = nullptr;
  bool use_default_db = true; // set false on '-B FILE'

  // Parse input options.
  int optchar;
  while ((optchar = getopt_long(argc, argv, shortopts, longopts, nullptr)) != -1) {
    char *arg;
    char *tailptr;
    long lchecktime;

    switch(optchar) {
    case 'q':
      // when to quit
      quit_nodev0 = false;
      if (!strcmp(optarg, "nodev"))
        quit = QUIT_NODEV;
      else if (!strcmp(optarg, "nodev0")) {
        quit = QUIT_NODEV;
        quit_nodev0 = true;
      }
      else if (!strcmp(optarg, "nodevstartup"))
        quit = QUIT_NODEVSTARTUP;
      else if (!strcmp(optarg, "nodev0startup")) {
        quit = QUIT_NODEVSTARTUP;
        quit_nodev0 = true;
      }
      else if (!strcmp(optarg, "errors"))
        quit = QUIT_ERRORS;
      else if (!strcmp(optarg, "errors,nodev0")) {
        quit = QUIT_ERRORS;
        quit_nodev0 = true;
      }
      else if (!strcmp(optarg, "never"))
        quit = QUIT_NEVER;
      else if (!strcmp(optarg, "onecheck")) {
        quit = QUIT_ONECHECK;
        debugmode = 1;
      }
      else if (!strcmp(optarg, "showtests")) {
        quit = QUIT_SHOWTESTS;
        debugmode = 1;
      }
      else
        badarg = true;
      break;
    case 'l':
      // set the log facility level
      if (!strcmp(optarg, "daemon"))
        facility=LOG_DAEMON;
      else if (!strcmp(optarg, "local0"))
        facility=LOG_LOCAL0;
      else if (!strcmp(optarg, "local1"))
        facility=LOG_LOCAL1;
      else if (!strcmp(optarg, "local2"))
        facility=LOG_LOCAL2;
      else if (!strcmp(optarg, "local3"))
        facility=LOG_LOCAL3;
      else if (!strcmp(optarg, "local4"))
        facility=LOG_LOCAL4;
      else if (!strcmp(optarg, "local5"))
        facility=LOG_LOCAL5;
      else if (!strcmp(optarg, "local6"))
        facility=LOG_LOCAL6;
      else if (!strcmp(optarg, "local7"))
        facility=LOG_LOCAL7;
      else
        badarg = true;
      break;
    case 'd':
      // enable debug mode
      debugmode = 1;
      break;
    case 'n':
      // don't fork()
#ifndef _WIN32 // On Windows, --service is already handled by daemon_main()
      do_fork = false;
#endif
      break;
    case 'D':
      // print summary of all valid directives
      debugmode = 1;
      Directives();
      return 0;
    case 'i':
      // Period (time interval) for checking
      // strtol will set errno in the event of overflow, so we'll check it.
      errno = 0;
      lchecktime = strtol(optarg, &tailptr, 10);
      if (*tailptr != '\0' || lchecktime < 10 || lchecktime > INT_MAX || errno) {
        debugmode=1;
        PrintHead();
        PrintOut(LOG_CRIT, "======> INVALID INTERVAL: %s <=======\n", optarg);
        PrintOut(LOG_CRIT, "======> INTERVAL MUST BE INTEGER BETWEEN %d AND %d <=======\n", 10, INT_MAX);
        PrintOut(LOG_CRIT, "\nUse smartd -h to get a usage summary\n\n");
        return EXIT_BADCMD;
      }
      checktime = (int)lchecktime;
      break;
    case 'r':
      // report IOCTL transactions
      {
        int n1 = -1, n2 = -1, len = strlen(optarg);
        char s[9+1]; unsigned i = 1;
        sscanf(optarg, "%9[a-z]%n,%u%n", s, &n1, &i, &n2);
        if (!((n1 == len || n2 == len) && 1 <= i && i <= 4)) {
          badarg = true;
        } else if (!strcmp(s,"ioctl")) {
          ata_debugmode = scsi_debugmode = nvme_debugmode = i;
        } else if (!strcmp(s,"ataioctl")) {
          ata_debugmode = i;
        } else if (!strcmp(s,"scsiioctl")) {
          scsi_debugmode = i;
        } else if (!strcmp(s,"nvmeioctl")) {
          nvme_debugmode = i;
        } else {
          badarg = true;
        }
      }
      break;
    case 'c':
      // alternate configuration file
      if (strcmp(optarg,"-"))
        configfile = (configfile_alt = optarg).c_str();
      else // read from stdin
        configfile=configfile_stdin;
      break;
    case 'p':
      // output file with PID number
      pid_file = optarg;
      break;
    case 's':
      // path prefix of persistent state file
      state_path_prefix = (strcmp(optarg, "-") ? optarg : "");
      break;
    case 'A':
      // path prefix of attribute log file
      attrlog_path_prefix = (strcmp(optarg, "-") ? optarg : "");
      break;
    case 'B':
      {
        const char * path = optarg;
        if (*path == '+' && path[1])
          path++;
        else
          use_default_db = false;
        unsigned char savedebug = debugmode; debugmode = 1;
        if (!read_drive_database(path))
          return EXIT_BADCMD;
        debugmode = savedebug;
      }
      break;
    case 'w':
      warning_script = optarg;
      break;
#ifdef HAVE_POSIX_API
    case 'u':
      warn_as_user = false;
      if (strcmp(optarg, "-")) {
        warn_uname = warn_gname = "unknown";
        badarg_msg = parse_ugid(optarg, warn_uid, warn_gid,
                                warn_uname, warn_gname     );
        if (badarg_msg)
          break;
        warn_as_user = true;
      }
      break;
#elif defined(_WIN32)
    case 'u':
      if (!strcmp(optarg, "restricted"))
        warn_as_restr_user = true;
      else if (!strcmp(optarg, "unchanged"))
        warn_as_restr_user = false;
      else
        badarg = true;
      break;
#endif // HAVE_POSIX_API ||_WIN32
    case 'V':
      // print version and CVS info
      debugmode = 1;
      PrintOut(LOG_INFO, "%s", format_version_info("smartd", 3 /*full*/).c_str());
      return 0;
#ifdef HAVE_LIBCAP_NG
    case 'C':
      // enable capabilities
      if (!optarg)
        capabilities_mode = 1;
      else if (!strcmp(optarg, "mail"))
        capabilities_mode = 2;
      else
        badarg = true;
      break;
#endif
    case 'h':
      // help: print summary of command-line options
      debugmode=1;
      PrintHead();
      Usage();
      return 0;
    case '?':
    default:
      // unrecognized option
      debugmode=1;
      PrintHead();
      // Point arg to the argument in which this option was found.
      // Note: getopt_long() may set optind > argc (e.g. musl libc)
      arg = argv[optind <= argc ? optind - 1 : argc - 1];
      // Check whether the option is a long option that doesn't map to -h.
      if (arg[1] == '-' && optchar != 'h') {
        // Iff optopt holds a valid option then argument must be missing.
        if (optopt && strchr(shortopts, optopt)) {
          PrintOut(LOG_CRIT, "=======> ARGUMENT REQUIRED FOR OPTION: %s <=======\n",arg+2);
          PrintValidArgs(optopt);
        } else {
          PrintOut(LOG_CRIT, "=======> UNRECOGNIZED OPTION: %s <=======\n\n",arg+2);
        }
        PrintOut(LOG_CRIT, "\nUse smartd --help to get a usage summary\n\n");
        return EXIT_BADCMD;
      }
      if (optopt) {
        // Iff optopt holds a valid option then argument must be missing.
        if (strchr(shortopts, optopt)){
          PrintOut(LOG_CRIT, "=======> ARGUMENT REQUIRED FOR OPTION: %c <=======\n",optopt);
          PrintValidArgs(optopt);
        } else {
          PrintOut(LOG_CRIT, "=======> UNRECOGNIZED OPTION: %c <=======\n\n",optopt);
        }
        PrintOut(LOG_CRIT, "\nUse smartd -h to get a usage summary\n\n");
        return EXIT_BADCMD;
      }
      Usage();
      return 0;
    }

    // Check to see if option had an unrecognized or incorrect argument.
    if (badarg || badarg_msg) {
      debugmode=1;
      PrintHead();
      // It would be nice to print the actual option name given by the user
      // here, but we just print the short form.  Please fix this if you know
      // a clean way to do it.
      PrintOut(LOG_CRIT, "=======> INVALID ARGUMENT TO -%c: %s <======= \n", optchar, optarg);
      if (badarg_msg)
        PrintOut(LOG_CRIT, "%s\n", badarg_msg);
      else
        PrintValidArgs(optchar);
      PrintOut(LOG_CRIT, "\nUse smartd -h to get a usage summary\n\n");
      return EXIT_BADCMD;
    }
  }

  // non-option arguments are not allowed
  if (argc > optind) {
    debugmode=1;
    PrintHead();
    PrintOut(LOG_CRIT, "=======> UNRECOGNIZED ARGUMENT: %s <=======\n\n", argv[optind]);
    PrintOut(LOG_CRIT, "\nUse smartd -h to get a usage summary\n\n");
    return EXIT_BADCMD;
  }

  // no pidfile in debug mode
  if (debugmode && !pid_file.empty()) {
    debugmode=1;
    PrintHead();
    PrintOut(LOG_CRIT, "=======> INVALID CHOICE OF OPTIONS: -d and -p <======= \n\n");
    PrintOut(LOG_CRIT, "Error: pid file %s not written in debug (-d) mode\n\n", pid_file.c_str());
    return EXIT_BADCMD;
  }

#ifndef _WIN32
  if (!debugmode) {
    // absolute path names are required due to chdir('/') in daemon_init()
    if (!(   check_abs_path('p', pid_file)
          && check_abs_path('s', state_path_prefix)
          && check_abs_path('A', attrlog_path_prefix)))
      return EXIT_BADCMD;
  }
#endif

#ifdef _WIN32
  if (warn_as_restr_user && !popen_as_restr_check()) {
    // debugmode=1 // would suppress messages to eventlog or log file
    PrintHead();
    PrintOut(LOG_CRIT, "Option '--warn-as-user=restricted' is not effective if the current user\n");
    PrintOut(LOG_CRIT, "is the local 'SYSTEM' or 'Administrator' account\n\n");
    return EXIT_BADCMD;
  }
#endif

  // Read or init drive database
  {
    unsigned char savedebug = debugmode; debugmode = 1;
    if (!init_drive_database(use_default_db))
      return EXIT_BADCMD;
    debugmode = savedebug;
  }

  // Check option compatibility of notify support
    // cppcheck-suppress knownConditionTrueFalse
  if (!notify_post_init())
    return EXIT_BADCMD;

  // print header, don't write Copyright line to syslog
  PrintOut(LOG_INFO, "%s\n", format_version_info("smartd", (debugmode ? 2 : 1)).c_str());

  // No error, continue in main_worker()
  return -1;
}

// Function we call if no configuration file was found or if the
// SCANDIRECTIVE Directive was found.  It makes entries for device
// names returned by scan_smart_devices() in os_OSNAME.cpp
static int MakeConfigEntries(const dev_config & base_cfg,
  dev_config_vector & conf_entries, smart_device_list & scanned_devs,
  const smart_devtype_list & types)
{
  // make list of devices
  smart_device_list devlist;
  if (!smi()->scan_smart_devices(devlist, types)) {
    PrintOut(LOG_CRIT, "DEVICESCAN failed: %s\n", smi()->get_errmsg());
    return 0;
  }
  
  // if no devices, return
  if (devlist.size() == 0)
    return 0;

  // add empty device slots for existing config entries
  while (scanned_devs.size() < conf_entries.size())
    scanned_devs.push_back((smart_device *)0);

  // loop over entries to create
  for (unsigned i = 0; i < devlist.size(); i++) {
    // Move device pointer
    smart_device * dev = devlist.release(i);
    scanned_devs.push_back(dev);

    // Append configuration and update names
    conf_entries.push_back(base_cfg);
    dev_config & cfg = conf_entries.back();
    cfg.name = dev->get_info().info_name;
    cfg.dev_name = dev->get_info().dev_name;

    // Set type only if scanning is limited to specific types
    // This is later used to set SMARTD_DEVICETYPE environment variable
    if (!types.empty())
      cfg.dev_type = dev->get_info().dev_type;
    else // SMARTD_DEVICETYPE=auto
      cfg.dev_type.clear();
  }
  
  return devlist.size();
}
 
// Returns negative value (see ParseConfigFile()) if config file
// had errors, else number of entries which may be zero or positive. 
static int ReadOrMakeConfigEntries(dev_config_vector & conf_entries, smart_device_list & scanned_devs)
{
  // parse configuration file configfile (normally /etc/smartd.conf)  
  smart_devtype_list scan_types;
  int entries = ParseConfigFile(conf_entries, scan_types);

  if (entries < 0) {
    // There was an error reading the configuration file.
    conf_entries.clear();
    if (entries == -1)
      PrintOut(LOG_CRIT, "Configuration file %s has fatal syntax errors.\n", configfile);
    return entries;
  }

  // no error parsing config file.
  if (entries) {
    // we did not find a SCANDIRECTIVE and did find valid entries
    PrintOut(LOG_INFO, "Configuration file %s parsed.\n", configfile);
  }
  else if (!conf_entries.empty()) {
    // we found a SCANDIRECTIVE or there was no configuration file so
    // scan.  Configuration file's last entry contains all options
    // that were set
    dev_config first = conf_entries.back();
    conf_entries.pop_back();

    if (first.lineno)
      PrintOut(LOG_INFO,"Configuration file %s was parsed, found %s, scanning devices\n", configfile, SCANDIRECTIVE);
    else
      PrintOut(LOG_INFO,"No configuration file %s found, scanning devices\n", configfile);
    
    // make config list of devices to search for
    MakeConfigEntries(first, conf_entries, scanned_devs, scan_types);

    // warn user if scan table found no devices
    if (conf_entries.empty())
      PrintOut(LOG_CRIT,"In the system's table of devices NO devices found to scan\n");
  } 
  else
    PrintOut(LOG_CRIT, "Configuration file %s parsed but has no entries\n", configfile);
  
  return conf_entries.size();
}

// Register one device, return false on error
static bool register_device(dev_config & cfg, dev_state & state, smart_device_auto_ptr & dev,
                            const dev_config_vector * prev_cfgs)
{
  bool scanning;
  if (!dev) {
    // Get device of appropriate type
    dev = smi()->get_smart_device(cfg.name.c_str(), cfg.dev_type.c_str());
    if (!dev) {
      if (cfg.dev_type.empty())
        PrintOut(LOG_INFO, "Device: %s, unable to autodetect device type\n", cfg.name.c_str());
      else
        PrintOut(LOG_INFO, "Device: %s, unsupported device type '%s'\n", cfg.name.c_str(), cfg.dev_type.c_str());
      return false;
    }
    scanning = false;
  }
  else {
    // Use device from device scan
    scanning = true;
  }

  // Save old info
  smart_device::device_info oldinfo = dev->get_info();

  // Open with autodetect support, may return 'better' device
  dev.replace( dev->autodetect_open() );

  // Report if type has changed
  if (oldinfo.dev_type != dev->get_dev_type())
    PrintOut(LOG_INFO, "Device: %s, type changed from '%s' to '%s'\n",
      cfg.name.c_str(), oldinfo.dev_type.c_str(), dev->get_dev_type());

  // Return if autodetect_open() failed
  if (!dev->is_open()) {
    if (debugmode || !scanning)
      PrintOut(LOG_INFO, "Device: %s, open() failed: %s\n", dev->get_info_name(), dev->get_errmsg());
    return false;
  }

  // Update informal name
  cfg.name = dev->get_info().info_name;
  PrintOut(LOG_INFO, "Device: %s, opened\n", cfg.name.c_str());

  int status;
  const char * typemsg;
  // register ATA device
  if (dev->is_ata()){
    typemsg = "ATA";
    status = ATADeviceScan(cfg, state, dev->to_ata(), prev_cfgs);
  }
  // or register SCSI device
  else if (dev->is_scsi()){
    typemsg = "SCSI";
    status = SCSIDeviceScan(cfg, state, dev->to_scsi(), prev_cfgs);
  }
  // or register NVMe device
  else if (dev->is_nvme()) {
    typemsg = "NVMe";
    status = NVMeDeviceScan(cfg, state, dev->to_nvme(), prev_cfgs);
  }
  else {
    PrintOut(LOG_INFO, "Device: %s, neither ATA, SCSI nor NVMe device\n", cfg.name.c_str());
    return false;
  }

  if (status) {
    if (!scanning || debugmode) {
      if (cfg.lineno)
        PrintOut(scanning ? LOG_INFO : LOG_CRIT,
          "Unable to register %s device %s at line %d of file %s\n",
          typemsg, cfg.name.c_str(), cfg.lineno, configfile);
      else
        PrintOut(LOG_INFO, "Unable to register %s device %s\n",
          typemsg, cfg.name.c_str());
    }

    return false;
  }

  return true;
}

// This function tries devices from conf_entries.  Each one that can be
// registered is moved onto the [ata|scsi]devices lists and removed
// from the conf_entries list.
static bool register_devices(const dev_config_vector & conf_entries, smart_device_list & scanned_devs,
                             dev_config_vector & configs, dev_state_vector & states, smart_device_list & devices)
{
  // start by clearing lists/memory of ALL existing devices
  configs.clear();
  devices.clear();
  states.clear();

  // Map of already seen non-DEVICESCAN devices (unique_name -> cfg.name)
  typedef std::map<std::string, std::string> prev_unique_names_map;
  prev_unique_names_map prev_unique_names;

  // Register entries
  for (unsigned i = 0; i < conf_entries.size(); i++) {
    dev_config cfg = conf_entries[i];

    // Get unique device "name [type]" (with symlinks resolved) for duplicate detection
    std::string unique_name = smi()->get_unique_dev_name(cfg.dev_name.c_str(), cfg.dev_type.c_str());
    if (debugmode && unique_name != cfg.dev_name) {
      pout("Device: %s%s%s%s, unique name: %s\n", cfg.name.c_str(),
           (!cfg.dev_type.empty() ? " [" : ""), cfg.dev_type.c_str(),
           (!cfg.dev_type.empty() ? "]" : ""), unique_name.c_str());
    }

    if (cfg.ignore) {
      // Store for duplicate detection and ignore
      PrintOut(LOG_INFO, "Device: %s%s%s%s, ignored\n", cfg.name.c_str(),
               (!cfg.dev_type.empty() ? " [" : ""), cfg.dev_type.c_str(),
               (!cfg.dev_type.empty() ? "]" : ""));
      prev_unique_names[unique_name] = cfg.name;
      continue;
    }

    smart_device_auto_ptr dev;

    // Device may already be detected during devicescan
    bool scanning = false;
    if (i < scanned_devs.size()) {
      dev = scanned_devs.release(i);
      if (dev) {
        // Check for a preceding non-DEVICESCAN entry for the same device
        prev_unique_names_map::iterator ui = prev_unique_names.find(unique_name);
        if (ui != prev_unique_names.end()) {
          bool ne = (ui->second != cfg.name);
          PrintOut(LOG_INFO, "Device: %s, %s%s, ignored\n", dev->get_info_name(),
                   (ne ? "same as " : "duplicate"), (ne ? ui->second.c_str() : ""));
          continue;
        }
        scanning = true;
      }
    }

    // Prevent systemd unit startup timeout when registering many devices
    notify_extend_timeout();

    // Register device
    // If scanning, pass dev_idinfo of previous devices for duplicate check
    dev_state state;
    if (!register_device(cfg, state, dev, (scanning ? &configs : 0))) {
      // if device is explicitly listed and we can't register it, then
      // exit unless the user has specified that the device is removable
      if (!scanning) {
        if (!(cfg.removable || quit == QUIT_NEVER)) {
          PrintOut(LOG_CRIT, "Unable to register device %s (no Directive -d removable). Exiting.\n",
                   cfg.name.c_str());
          return false;
        }
        PrintOut(LOG_INFO, "Device: %s, not available\n", cfg.name.c_str());
        // Prevent retry of registration
        prev_unique_names[unique_name] = cfg.name;
      }
      continue;
    }

    // move onto the list of devices
    configs.push_back(cfg);
    states.push_back(state);
    devices.push_back(dev);
    if (!scanning)
      // Store for duplicate detection
      prev_unique_names[unique_name] = cfg.name;
  }

  // Set minimum check time and factors for staggered tests
  checktime_min = 0;
  unsigned factor = 0;
  for (auto & cfg : configs) {
    if (cfg.checktime && (!checktime_min || checktime_min > cfg.checktime))
      checktime_min = cfg.checktime;
    if (!cfg.test_regex.empty())
      cfg.test_offset_factor = factor++;
  }
  if (checktime_min && checktime_min > checktime)
    checktime_min = checktime;

  init_disable_standby_check(configs);
  return true;
}


// Main program without exception handling
static int main_worker(int argc, char **argv)
{
  // Initialize interface
  smart_interface::init();
  if (!smi())
    return 1;

  // Check whether systemd notify is supported and enabled
  notify_init();

  // parse input and print header and usage info if needed
  int status = parse_options(argc,argv);
  if (status >= 0)
    return status;
  
  // Configuration for each device
  dev_config_vector configs;
  // Device states
  dev_state_vector states;
  // Devices to monitor
  smart_device_list devices;

  // Drop capabilities if supported and enabled
  capabilities_drop_now();

  notify_msg("Initializing ...");

  // the main loop of the code
  bool firstpass = true, write_states_always = true;
  time_t wakeuptime = 0;
  // assert(status < 0);
  do {
    // Should we (re)read the config file?
    if (firstpass || caughtsigHUP){
      if (!firstpass) {
        // Write state files
        if (!state_path_prefix.empty())
          write_all_dev_states(configs, states);

        PrintOut(LOG_INFO,
                 caughtsigHUP==1?
                 "Signal HUP - rereading configuration file %s\n":
                 "\a\nSignal INT - rereading configuration file %s (" SIGQUIT_KEYNAME " quits)\n\n",
                 configfile);
        notify_msg("Reloading ...");
      }

      {
        dev_config_vector conf_entries; // Entries read from smartd.conf
        smart_device_list scanned_devs; // Devices found during scan
        // (re)reads config file, makes >=0 entries
        int entries = ReadOrMakeConfigEntries(conf_entries, scanned_devs);

        if (entries>=0) {
          // checks devices, then moves onto ata/scsi list or deallocates.
          if (!register_devices(conf_entries, scanned_devs, configs, states, devices)) {
            status = EXIT_BADDEV;
            break;
          }
          if (!(configs.size() == devices.size() && configs.size() == states.size()))
            throw std::logic_error("Invalid result from RegisterDevices");
        }
        else if (   quit == QUIT_NEVER
                 || ((quit == QUIT_NODEV || quit == QUIT_NODEVSTARTUP) && !firstpass)) {
          // user has asked to continue on error in configuration file
          if (!firstpass)
            PrintOut(LOG_INFO,"Reusing previous configuration\n");
        }
        else {
          // exit with configuration file error status
          status = (entries == -3 ? EXIT_READCONF : entries == -2 ? EXIT_NOCONF : EXIT_BADCONF);
          break;
        }
      }

      if (!(   devices.size() > 0 || quit == QUIT_NEVER
            || (quit == QUIT_NODEVSTARTUP && !firstpass))) {
        status = (!quit_nodev0 ? EXIT_NODEV : 0);
        PrintOut((status ? LOG_CRIT : LOG_INFO),
                 "Unable to monitor any SMART enabled devices. Exiting.\n");
        break;
      }

      // Log number of devices we are monitoring...
      int numata = 0, numscsi = 0;
      for (unsigned i = 0; i < devices.size(); i++) {
        const smart_device * dev = devices.at(i);
        if (dev->is_ata())
          numata++;
        else if (dev->is_scsi())
          numscsi++;
      }
      PrintOut(LOG_INFO, "Monitoring %d ATA/SATA, %d SCSI/SAS and %d NVMe devices\n",
               numata, numscsi, (int)devices.size() - numata - numscsi);

      if (quit == QUIT_SHOWTESTS) {
        // user has asked to print test schedule
        PrintTestSchedule(configs, states, devices);
        // assert(firstpass);
        return 0;
      }

      // reset signal
      caughtsigHUP=0;

      // Always write state files after (re)configuration
      write_states_always = true;
    }

    // check all devices once,
    // self tests are not started in first pass unless '-q onecheck' is specified
    notify_check((int)devices.size());
    CheckDevicesOnce(configs, states, devices, firstpass, (!firstpass || quit == QUIT_ONECHECK));

     // Write state files
    if (!state_path_prefix.empty())
      write_all_dev_states(configs, states, write_states_always);
    write_states_always = false;

    // Write attribute logs
    if (!attrlog_path_prefix.empty())
      write_all_dev_attrlogs(configs, states);

    // user has asked us to exit after first check
    if (quit == QUIT_ONECHECK) {
      PrintOut(LOG_INFO,"Started with '-q onecheck' option. All devices successfully checked once.\n"
               "smartd is exiting (exit status 0)\n");
      // assert(firstpass);
      return 0;
    }
    
    if (firstpass) {
      if (!debugmode) {
        // fork() into background if needed, close ALL file descriptors,
        // redirect stdin, stdout, and stderr, chdir to "/".
        status = daemon_init();
        if (status >= 0)
          return status;

        // Write PID file if configured
        if (!write_pid_file())
          return EXIT_PID;
      }

      // Set exit and signal handlers
      install_signal_handlers();

      // Initialize wakeup time to CURRENT time
      wakeuptime = time(nullptr);

      firstpass = false;
    }

    // sleep until next check time, or a signal arrives
    wakeuptime = dosleep(wakeuptime, configs, states, write_states_always);

  } while (!caughtsigEXIT);

  if (caughtsigEXIT && status < 0) {
    // Loop exited on signal
    if (caughtsigEXIT == SIGTERM || (debugmode && caughtsigEXIT == SIGQUIT)) {
      PrintOut(LOG_INFO, "smartd received signal %d: %s\n",
               caughtsigEXIT, strsignal(caughtsigEXIT));
    }
    else {
      // Unexpected SIGINT or SIGQUIT
      PrintOut(LOG_CRIT, "smartd received unexpected signal %d: %s\n",
               caughtsigEXIT, strsignal(caughtsigEXIT));
      status = EXIT_SIGNAL;
    }
  }

  // Status unset above implies success
  if (status < 0)
    status = 0;

  if (!firstpass) {
    // Loop exited after daemon_init() and write_pid_file()

    // Write state files only on normal exit
    if (!status && !state_path_prefix.empty())
      write_all_dev_states(configs, states);

    // Delete PID file, if one was created
    if (!pid_file.empty() && unlink(pid_file.c_str()))
        PrintOut(LOG_CRIT,"Can't unlink PID file %s (%s).\n",
                 pid_file.c_str(), strerror(errno));
  }

  PrintOut((status ? LOG_CRIT : LOG_INFO), "smartd is exiting (exit status %d)\n", status);
  return status;
}


#ifndef _WIN32
// Main program
int main(int argc, char **argv)
#else
// Windows: internal main function started direct or by service control manager
static int smartd_main(int argc, char **argv)
#endif
{
  int status;
  try {
    // Do the real work ...
    status = main_worker(argc, argv);
  }
  catch (const std::bad_alloc & /*ex*/) {
    // Memory allocation failed (also thrown by std::operator new)
    PrintOut(LOG_CRIT, "Smartd: Out of memory\n");
    status = EXIT_NOMEM;
  }
  catch (const std::exception & ex) {
    // Other fatal errors
    PrintOut(LOG_CRIT, "Smartd: Exception: %s\n", ex.what());
    status = EXIT_BADCODE;
  }

  // Check for remaining device objects
  if (smart_device::get_num_objects() != 0) {
    PrintOut(LOG_CRIT, "Smartd: Internal Error: %d device object(s) left at exit.\n",
             smart_device::get_num_objects());
    status = EXIT_BADCODE;
  }

  if (status == EXIT_BADCODE)
    PrintOut(LOG_CRIT, "Please inform " PACKAGE_BUGREPORT ", including output of smartd -V.\n");

  notify_exit(status);
#ifdef _WIN32
  daemon_winsvc_exitcode = status;
#endif
  return status;
}


#ifdef _WIN32
// Main function for Windows
int main(int argc, char **argv){
  // Options for smartd windows service
  static const daemon_winsvc_options svc_opts = {
    "--service", // cmd_opt
    "smartd", "SmartD Service", // servicename, displayname
    // description
    "Controls and monitors storage devices using the Self-Monitoring, "
    "Analysis and Reporting Technology System (SMART) built into "
    "ATA/SATA and SCSI/SAS hard drives and solid-state drives. "
    "www.smartmontools.org"
  };
  // daemon_main() handles daemon and service specific commands
  // and starts smartd_main() direct, from a new process,
  // or via service control manager
  return daemon_main("smartd", &svc_opts , smartd_main, argc, argv);
}
#endif
