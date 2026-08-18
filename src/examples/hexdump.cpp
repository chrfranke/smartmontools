/*
 * hexdump.cpp - hex dump utility (libsmartmon example program)
 *
 * Home page of code is: https://www.smartmontools.org
 *
 * Copyright (C) 2026 Christian Franke
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <smartmon/hexdump.h>
#include <smartmon/version.h>

#include <cstdio>
#include <cstring>

static int usage(const char * prog, int status)
{
  std::printf(
    "Hex Dump Utility (smartmontools " SMARTMONTOOLS_GIT_VER_DESC ")\n\n"
    "Usage: %s [-CRh] [FILE]\n\n"
    "    -C         Print 'hexdump --canonical' compatible format (see hexdump(1))\n"
    "    -R         Print 'xxd -r' compatible format\n"
    "    -h         Print this help\n",
    prog
  );
  return status;
}

int main(int argc, char **argv)
{
  smartmon::hexdump_options options{};
  int ai;
  for (ai = 1; ai < argc && argv[ai][0] == '-'; ai++) {
    if (!std::strcmp(argv[ai], "-C"))
      options = smartmon::hexdump_options_canonical;
    else if (!std::strcmp(argv[ai], "-R"))
      options = smartmon::hexdump_options_xxd_r;
    else if (!std::strcmp(argv[ai], "-h"))
      return usage(argv[0], 0);
    else
      return usage(argv[0], 1);
  }

  FILE * f = stdin;
  if (ai + 1 == argc) {
    f = std::fopen(argv[ai], "rb");
    if (!f) {
      std::perror(argv[ai]);
      return 1;
    }
  }
  else if (ai != argc)
    return usage(argv[0], 1);

  auto out = [](const char * str) { std::fputs(str, stdout); };
  smartmon::hexdumper<decltype(out)> hexdump(out, options);

  uint8_t buf[16];
  while (size_t n = std::fread(buf, 1, sizeof(buf), f))
    hexdump(buf, n);
  hexdump();

  int status = 0;
  if (std::ferror(f)) {
    std::fputs("read error\n", stderr);
    status = 1;
  }
  std::fclose(f);
  return status;
}
