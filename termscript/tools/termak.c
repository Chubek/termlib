/* Launch the trusted Ruby build DSL. argv is passed unchanged, never via a
 * shell. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef TERMAK_BUILD_ENTRY
#define TERMAK_BUILD_ENTRY "termscript/termak/Termakfile.rb"
#endif
#ifndef TERMAK_INSTALL_ENTRY
#define TERMAK_INSTALL_ENTRY "/usr/local/share/termscript/termak/Termakfile.rb"
#endif
int
main (int argc, char **argv)
{
  const char *entry = access (TERMAK_BUILD_ENTRY, R_OK) == 0
                          ? TERMAK_BUILD_ENTRY
                          : TERMAK_INSTALL_ENTRY;
  char **args = calloc ((size_t)argc + 2, sizeof *args);
  if (!args)
    {
      fputs ("termak: out of memory\n", stderr);
      return 1;
    }
  args[0] = "ruby";
  args[1] = (char *)entry;
  for (int i = 1; i < argc; i++)
    args[i + 1] = argv[i];
  execvp (args[0], args);
  fprintf (stderr, "termak: cannot launch Ruby: %s\n", strerror (errno));
  free (args);
  return 1;
}
