
/******************************************************************************
 *
 *  This file is part of canu, a software program that assembles whole-genome
 *  sequencing reads into contigs.
 *
 *  This software is based on:
 *    'Celera Assembler' (http://wgs-assembler.sourceforge.net)
 *    the 'kmer package' (http://kmer.sourceforge.net)
 *  both originally distributed by Applera Corporation under the GNU General
 *  Public License, version 2.
 *
 *  Canu branched from Celera Assembler at its revision 4587.
 *  Canu branched from the kmer project at its revision 1994.
 *
 *  Modifications by:
 *
 *    Brian P. Walenz from 2007-AUG-03 to 2013-AUG-01
 *      are Copyright 2007-2009,2011-2013 J. Craig Venter Institute, and
 *      are subject to the GNU General Public License version 2
 *
 *    Sergey Koren on 2009-MAR-06
 *      are Copyright 2009 J. Craig Venter Institute, and
 *      are subject to the GNU General Public License version 2
 *
 *    Brian P. Walenz on 2015-MAR-03
 *      are Copyright 2015 Battelle National Biodefense Institute, and
 *      are subject to the BSD 3-Clause License
 *
 *    Brian P. Walenz beginning on 2015-NOV-08
 *      are a 'United States Government Work', and
 *      are released in the public domain
 *
 *  File 'README.licenses' in the root directory of this distribution contains
 *  full conditions and disclaimers for each license.
 */

#include "AS_global.H"
#include "canu_version.H"

#include "files.H"
#include "system.H"

#ifdef X86_GCC_LINUX
#include <fpu_control.h>
#endif

#ifdef _GLIBCXX_PARALLEL
#include <parallel/algorithm>
#include <parallel/settings.h>
#endif


//  We take argc and argv, so, maybe, eventually, we'll want to parse
//  something out of there.  We return argc in case what we parse we
//  want to remove.
//
int
AS_configure(int argc, char **argv) {


#ifdef X86_GCC_LINUX
  //  Set the x86 FPU control word to force double precision rounding
  //  rather than `extended' precision rounding. This causes base
  //  calls and quality values on x86 GCC-Linux (tested on RedHat
  //  Linux) machines to be identical to those on IEEE conforming UNIX
  //  machines.
  //
  fpu_control_t fpu_cw = ( _FPU_DEFAULT & ~_FPU_EXTENDED ) | _FPU_DOUBLE;

  _FPU_SETCW( fpu_cw );
#endif


#ifdef _GLIBCXX_PARALLEL_SETTINGS_H
  __gnu_parallel::_Settings s = __gnu_parallel::_Settings::get();

  //  The sizes that GNU decides to enable parallelization for are
  //  ludicrously small.  A benchmark (Dec 2019, gcc6) showed that a
  //  sort-heavy code was a whopping 10% faster (wall clock 61 vs 69 seconds)
  //  but used 10x the CPU time (707 vs 69 seconds).
  //
  //  Further, the sort is no longer in-place.  This matters, significantly,
  //  for our overlap sorting, since we fill memory with as many overlaps as
  //  possible, and if we don't sort in-place, we run out of memory.
  //
  //  So, if the silly user has enabled parallelization, turn it off for
  //  sorting by setting the minimal size to something large.
  //
  s.sort_minimal_n = UINT64_MAX;

  __gnu_parallel::_Settings::set(s);
#endif


  //  Default to one thread.  This is mostly to disable the parallel sort,
  //  which seems to have a few bugs left in it.  e.g., a crash when using 48
  //  threads, but not when using 47, 49 or 64 threads.

  omp_set_num_threads(1);


  //  Install a signal handler to catch seg faults and errors.

  AS_UTL_installCrashCatcher(argv[0]);


  //  Set the start time.

  getProcessTime();


  //
  //  Et cetera.
  //

  for (int32 i=0; i<argc; i++) {
    if (strcmp(argv[i], "--version") == 0) {
      fputs(CANU_VERSION, stderr);
      exit(0);
    }
  }


  //
  //  Logging.
  //

  char *p = getenv("CANU_DIRECTORY");
  if (p == NULL)
    return(argc);

  char  D[FILENAME_MAX] = {0};
  char  N[FILENAME_MAX] = {0};
  char  H[1024]         = {0};  //  HOST_NAME_MAX?  Undefined.

  //  Make a directory for logs.  If an error, just return now, there's nothing we can log.

  snprintf(D, FILENAME_MAX, "%s/canu-logs", p);

  errno = 0;
  mkdir(D, S_IRWXU | S_IRWXG | S_IRWXO);
  if ((errno != 0) && (errno != EEXIST))
    return(argc);

  //  Our hostname is part of our unique filename.

  gethostname(H, 1024);

  //  Our executable name is part of our unique filename too.

  char *E = argv[0] + strlen(argv[0]) - 1;
  while ((E != argv[0]) && (*E != '/'))
    E--;
  if (*E == '/')
    E++;

  //  Construct a name for this log, and open it.  If we can't open it, just skip the log.

  snprintf(N, FILENAME_MAX, "%s/" F_U64 "_%s_" F_U64 "_%s",
           D,
           (uint64)time(NULL),
           H,
           (uint64)getpid(),
           E);

  errno = 0;
  FILE *F = fopen(N, "w");
  if ((errno != 0) || (F == NULL))
    return(argc);

  fprintf(F, "Canu v%s.%s (+%s commits) r%s %s.\n",
          CANU_VERSION_MAJOR,
          CANU_VERSION_MINOR,
          CANU_VERSION_COMMITS,
          CANU_VERSION_REVISION,
          CANU_VERSION_HASH);
  fprintf(F, "\n");
  fprintf(F, "Current Working Directory:\n");
  fprintf(F, "%s\n", getcwd(N, FILENAME_MAX));
  fprintf(F, "\n");
  fprintf(F, "Command:\n");
  fprintf(F, "%s", argv[0]);

  for (int32 i=1; i<argc; i++)
    if (argv[i][0] == '-')
      fprintf(F, " \\\n  %s", argv[i]);
    else
      fprintf(F, " %s", argv[i]);

  fprintf(F, "\n");

  AS_UTL_closeFile(F, N, true);

  return(argc);
}
