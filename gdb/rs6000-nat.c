/* IBM RS/6000 native-dependent code for GDB, the GNU debugger.
   Copyright 1986, 1987, 1989, 1991, 1992, 1994, 1995, 1996, 1997, 1998
   Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */

#include "defs.h"
#include "inferior.h"
#include "target.h"
#include "gdbcore.h"
#include "xcoffsolib.h"
#include "symfile.h"
#include "objfiles.h"
#include "libbfd.h"		/* For bfd_cache_lookup (FIXME) */
#include "bfd.h"
#include "gdb-stabs.h"

#include <sys/ptrace.h>
#include <sys/reg.h>

#include <sys/param.h>
#include <sys/dir.h>
#include <sys/user.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include <a.out.h>
#include <sys/file.h>
#include "gdb_stat.h"
#include <sys/core.h>
#include <sys/ldr.h>

extern int errno;

extern struct vmap *map_vmap PARAMS ((bfd * bf, bfd * arch));

extern struct target_ops exec_ops;

static void
vmap_exec PARAMS ((void));

static void
vmap_ldinfo PARAMS ((struct ld_info *));

static struct vmap *
  add_vmap PARAMS ((struct ld_info *));

static int
objfile_symbol_add PARAMS ((char *));

static void
vmap_symtab PARAMS ((struct vmap *));

static void
fetch_core_registers PARAMS ((char *, unsigned int, int, CORE_ADDR));

static void
exec_one_dummy_insn PARAMS ((void));

extern void
fixup_breakpoints PARAMS ((CORE_ADDR low, CORE_ADDR high, CORE_ADDR delta));

/* Conversion from gdb-to-system special purpose register numbers.. */

static int special_regs[] =
{
  IAR,				/* PC_REGNUM    */
  MSR,				/* PS_REGNUM    */
  CR,				/* CR_REGNUM    */
  LR,				/* LR_REGNUM    */
  CTR,				/* CTR_REGNUM   */
  XER,				/* XER_REGNUM   */
  MQ				/* MQ_REGNUM    */
};

void
fetch_inferior_registers (regno)
     int regno;
{
  int ii;

  if (regno < 0)
    {				/* for all registers */

      /* read 32 general purpose registers. */

      for (ii = 0; ii < 32; ++ii)
	*(int *) &registers[REGISTER_BYTE (ii)] =
	  ptrace (PT_READ_GPR, inferior_pid, (PTRACE_ARG3_TYPE) ii, 0, 0);

      /* read general purpose floating point registers. */

      for (ii = 0; ii < 32; ++ii)
	ptrace (PT_READ_FPR, inferior_pid,
	    (PTRACE_ARG3_TYPE) & registers[REGISTER_BYTE (FP0_REGNUM + ii)],
		FPR0 + ii, 0);

      /* read special registers. */
      for (ii = 0; ii <= LAST_UISA_SP_REGNUM - FIRST_UISA_SP_REGNUM; ++ii)
	*(int *) &registers[REGISTER_BYTE (FIRST_UISA_SP_REGNUM + ii)] =
	  ptrace (PT_READ_GPR, inferior_pid, (PTRACE_ARG3_TYPE) special_regs[ii],
		  0, 0);

      registers_fetched ();
      return;
    }

  /* else an individual register is addressed. */

  else if (regno < FP0_REGNUM)
    {				/* a GPR */
      *(int *) &registers[REGISTER_BYTE (regno)] =
	ptrace (PT_READ_GPR, inferior_pid, (PTRACE_ARG3_TYPE) regno, 0, 0);
    }
  else if (regno <= FPLAST_REGNUM)
    {				/* a FPR */
      ptrace (PT_READ_FPR, inferior_pid,
	      (PTRACE_ARG3_TYPE) & registers[REGISTER_BYTE (regno)],
	      (regno - FP0_REGNUM + FPR0), 0);
    }
  else if (regno <= LAST_UISA_SP_REGNUM)
    {				/* a special register */
      *(int *) &registers[REGISTER_BYTE (regno)] =
	ptrace (PT_READ_GPR, inferior_pid,
	      (PTRACE_ARG3_TYPE) special_regs[regno - FIRST_UISA_SP_REGNUM],
		0, 0);
    }
  else
    fprintf_unfiltered (gdb_stderr,
			"gdb error: register no %d not implemented.\n",
			regno);

  register_valid[regno] = 1;
}

/* Store our register values back into the inferior.
   If REGNO is -1, do this for all registers.
   Otherwise, REGNO specifies which register (so we can save time).  */

void
store_inferior_registers (regno)
     int regno;
{

  errno = 0;

  if (regno == -1)
    {				/* for all registers..  */
      int ii;

      /* execute one dummy instruction (which is a breakpoint) in inferior
         process. So give kernel a chance to do internal house keeping.
         Otherwise the following ptrace(2) calls will mess up user stack
         since kernel will get confused about the bottom of the stack (%sp) */

      exec_one_dummy_insn ();

      /* write general purpose registers first! */
      for (ii = GPR0; ii <= GPR31; ++ii)
	{
	  ptrace (PT_WRITE_GPR, inferior_pid, (PTRACE_ARG3_TYPE) ii,
		  *(int *) &registers[REGISTER_BYTE (ii)], 0);
	  if (errno)
	    {
	      perror ("ptrace write_gpr");
	      errno = 0;
	    }
	}

      /* write floating point registers now. */
      for (ii = 0; ii < 32; ++ii)
	{
	  ptrace (PT_WRITE_FPR, inferior_pid,
	    (PTRACE_ARG3_TYPE) & registers[REGISTER_BYTE (FP0_REGNUM + ii)],
		  FPR0 + ii, 0);
	  if (errno)
	    {
	      perror ("ptrace write_fpr");
	      errno = 0;
	    }
	}

      /* write special registers. */
      for (ii = 0; ii <= LAST_UISA_SP_REGNUM - FIRST_UISA_SP_REGNUM; ++ii)
	{
	  ptrace (PT_WRITE_GPR, inferior_pid,
		  (PTRACE_ARG3_TYPE) special_regs[ii],
	     *(int *) &registers[REGISTER_BYTE (FIRST_UISA_SP_REGNUM + ii)],
		  0);
	  if (errno)
	    {
	      perror ("ptrace write_gpr");
	      errno = 0;
	    }
	}
    }

  /* else, a specific register number is given... */

  else if (regno < FP0_REGNUM)	/* a GPR */
    {
      if (regno == SP_REGNUM)
	exec_one_dummy_insn ();
      ptrace (PT_WRITE_GPR, inferior_pid, (PTRACE_ARG3_TYPE) regno,
	      *(int *) &registers[REGISTER_BYTE (regno)], 0);
    }

  else if (regno <= FPLAST_REGNUM)	/* a FPR */
    {
      ptrace (PT_WRITE_FPR, inferior_pid,
	      (PTRACE_ARG3_TYPE) & registers[REGISTER_BYTE (regno)],
	      regno - FP0_REGNUM + FPR0, 0);
    }

  else if (regno <= LAST_UISA_SP_REGNUM)	/* a special register */
    {
      ptrace (PT_WRITE_GPR, inferior_pid,
	      (PTRACE_ARG3_TYPE) special_regs[regno - FIRST_UISA_SP_REGNUM],
	      *(int *) &registers[REGISTER_BYTE (regno)], 0);
    }

  else if (regno < NUM_REGS)
    {
      /* Ignore it.  */
    }

  else
    fprintf_unfiltered (gdb_stderr,
			"Gdb error: register no %d not implemented.\n",
			regno);

  if (errno)
    {
      perror ("ptrace write");
      errno = 0;
    }
}

/* Execute one dummy breakpoint instruction.  This way we give the kernel
   a chance to do some housekeeping and update inferior's internal data,
   including u_area. */

static void
exec_one_dummy_insn ()
{
#define	DUMMY_INSN_ADDR	(TEXT_SEGMENT_BASE)+0x200

  char shadow_contents[BREAKPOINT_MAX];		/* Stash old bkpt addr contents */
  int status, pid;
  CORE_ADDR prev_pc;

  /* We plant one dummy breakpoint into DUMMY_INSN_ADDR address. We
     assume that this address will never be executed again by the real
     code. */

  target_insert_breakpoint (DUMMY_INSN_ADDR, shadow_contents);

  errno = 0;

  /* You might think this could be done with a single ptrace call, and
     you'd be correct for just about every platform I've ever worked
     on.  However, rs6000-ibm-aix4.1.3 seems to have screwed this up --
     the inferior never hits the breakpoint (it's also worth noting
     powerpc-ibm-aix4.1.3 works correctly).  */
  prev_pc = read_pc ();
  write_pc (DUMMY_INSN_ADDR);
  ptrace (PT_CONTINUE, inferior_pid, (PTRACE_ARG3_TYPE) 1, 0, 0);

  if (errno)
    perror ("pt_continue");

  do
    {
      pid = wait (&status);
    }
  while (pid != inferior_pid);

  write_pc (prev_pc);
  target_remove_breakpoint (DUMMY_INSN_ADDR, shadow_contents);
}

static void
fetch_core_registers (core_reg_sect, core_reg_size, which, reg_addr)
     char *core_reg_sect;
     unsigned core_reg_size;
     int which;
     CORE_ADDR reg_addr;	/* Unused in this version */
{
  /* fetch GPRs and special registers from the first register section
     in core bfd. */
  if (which == 0)
    {
      /* copy GPRs first. */
      memcpy (registers, core_reg_sect, 32 * 4);

      /* gdb's internal register template and bfd's register section layout
         should share a common include file. FIXMEmgo */
      /* then comes special registes. They are supposed to be in the same
         order in gdb template and bfd `.reg' section. */
      core_reg_sect += (32 * 4);
      memcpy (&registers[REGISTER_BYTE (FIRST_UISA_SP_REGNUM)],
	      core_reg_sect,
	      (LAST_UISA_SP_REGNUM - FIRST_UISA_SP_REGNUM + 1) * 4);
    }

  /* fetch floating point registers from register section 2 in core bfd. */
  else if (which == 2)
    memcpy (&registers[REGISTER_BYTE (FP0_REGNUM)], core_reg_sect, 32 * 8);

  else
    fprintf_unfiltered
      (gdb_stderr,
       "Gdb error: unknown parameter to fetch_core_registers().\n");
}

/* handle symbol translation on vmapping */

static void
vmap_symtab (vp)
     register struct vmap *vp;
{
  register struct objfile *objfile;
  struct section_offsets *new_offsets;
  int i;

  objfile = vp->objfile;
  if (objfile == NULL)
    {
      /* OK, it's not an objfile we opened ourselves.
         Currently, that can only happen with the exec file, so
         relocate the symbols for the symfile.  */
      if (symfile_objfile == NULL)
	return;
      objfile = symfile_objfile;
    }

  new_offsets = (struct section_offsets *) alloca (SIZEOF_SECTION_OFFSETS);

  for (i = 0; i < objfile->num_sections; ++i)
    ANOFFSET (new_offsets, i) = ANOFFSET (objfile->section_offsets, i);

  /* The symbols in the object file are linked to the VMA of the section,
     relocate them VMA relative.  */
  ANOFFSET (new_offsets, SECT_OFF_TEXT) = vp->tstart - vp->tvma;
  ANOFFSET (new_offsets, SECT_OFF_DATA) = vp->dstart - vp->dvma;
  ANOFFSET (new_offsets, SECT_OFF_BSS) = vp->dstart - vp->dvma;

  objfile_relocate (objfile, new_offsets);
}

/* Add symbols for an objfile.  */

static int
objfile_symbol_add (arg)
     char *arg;
{
  struct objfile *obj = (struct objfile *) arg;

  syms_from_objfile (obj, NULL, 0, 0);
  new_symfile_objfile (obj, 0, 0);
  return 1;
}

/* Add a new vmap entry based on ldinfo() information.

   If ldi->ldinfo_fd is not valid (e.g. this struct ld_info is from a
   core file), the caller should set it to -1, and we will open the file.

   Return the vmap new entry.  */

static struct vmap *
add_vmap (ldi)
     register struct ld_info *ldi;
{
  bfd *abfd, *last;
  register char *mem, *objname;
  struct objfile *obj;
  struct vmap *vp;

  /* This ldi structure was allocated using alloca() in 
     xcoff_relocate_symtab(). Now we need to have persistent object 
     and member names, so we should save them. */

  mem = ldi->ldinfo_filename + strlen (ldi->ldinfo_filename) + 1;
  mem = savestring (mem, strlen (mem));
  objname = savestring (ldi->ldinfo_filename, strlen (ldi->ldinfo_filename));

  if (ldi->ldinfo_fd < 0)
    /* Note that this opens it once for every member; a possible
       enhancement would be to only open it once for every object.  */
    abfd = bfd_openr (objname, gnutarget);
  else
    abfd = bfd_fdopenr (objname, gnutarget, ldi->ldinfo_fd);
  if (!abfd)
    error ("Could not open `%s' as an executable file: %s",
	   objname, bfd_errmsg (bfd_get_error ()));

  /* make sure we have an object file */

  if (bfd_check_format (abfd, bfd_object))
    vp = map_vmap (abfd, 0);

  else if (bfd_check_format (abfd, bfd_archive))
    {
      last = 0;
      /* FIXME??? am I tossing BFDs?  bfd? */
      while ((last = bfd_openr_next_archived_file (abfd, last)))
	if (STREQ (mem, last->filename))
	  break;

      if (!last)
	{
	  bfd_close (abfd);
	  /* FIXME -- should be error */
	  warning ("\"%s\": member \"%s\" missing.", abfd->filename, mem);
	  return 0;
	}

      if (!bfd_check_format (last, bfd_object))
	{
	  bfd_close (last);	/* XXX???       */
	  goto obj_err;
	}

      vp = map_vmap (last, abfd);
    }
  else
    {
    obj_err:
      bfd_close (abfd);
      error ("\"%s\": not in executable format: %s.",
	     objname, bfd_errmsg (bfd_get_error ()));
      /*NOTREACHED */
    }
  obj = allocate_objfile (vp->bfd, 0);
  vp->objfile = obj;

#ifndef SOLIB_SYMBOLS_MANUAL
  if (catch_errors (objfile_symbol_add, (char *) obj,
		    "Error while reading shared library symbols:\n",
		    RETURN_MASK_ALL))
    {
      /* Note this is only done if symbol reading was successful.  */
      vmap_symtab (vp);
      vp->loaded = 1;
    }
#endif
  return vp;
}

/* update VMAP info with ldinfo() information
   Input is ptr to ldinfo() results.  */

static void
vmap_ldinfo (ldi)
     register struct ld_info *ldi;
{
  struct stat ii, vi;
  register struct vmap *vp;
  int got_one, retried;
  int got_exec_file = 0;

  /* For each *ldi, see if we have a corresponding *vp.
     If so, update the mapping, and symbol table.
     If not, add an entry and symbol table.  */

  do
    {
      char *name = ldi->ldinfo_filename;
      char *memb = name + strlen (name) + 1;

      retried = 0;

      if (fstat (ldi->ldinfo_fd, &ii) < 0)
	{
	  /* The kernel sets ld_info to -1, if the process is still using the
	     object, and the object is removed. Keep the symbol info for the
	     removed object and issue a warning.  */
	  warning ("%s (fd=%d) has disappeared, keeping its symbols",
		   name, ldi->ldinfo_fd);
	  continue;
	}
    retry:
      for (got_one = 0, vp = vmap; vp; vp = vp->nxt)
	{
	  struct objfile *objfile;

	  /* First try to find a `vp', which is the same as in ldinfo.
	     If not the same, just continue and grep the next `vp'. If same,
	     relocate its tstart, tend, dstart, dend values. If no such `vp'
	     found, get out of this for loop, add this ldi entry as a new vmap
	     (add_vmap) and come back, find its `vp' and so on... */

	  /* The filenames are not always sufficient to match on. */

	  if ((name[0] == '/' && !STREQ (name, vp->name))
	      || (memb[0] && !STREQ (memb, vp->member)))
	    continue;

	  /* See if we are referring to the same file.
	     We have to check objfile->obfd, symfile.c:reread_symbols might
	     have updated the obfd after a change.  */
	  objfile = vp->objfile == NULL ? symfile_objfile : vp->objfile;
	  if (objfile == NULL
	      || objfile->obfd == NULL
	      || bfd_stat (objfile->obfd, &vi) < 0)
	    {
	      warning ("Unable to stat %s, keeping its symbols", name);
	      continue;
	    }

	  if (ii.st_dev != vi.st_dev || ii.st_ino != vi.st_ino)
	    continue;

	  if (!retried)
	    close (ldi->ldinfo_fd);

	  ++got_one;

	  /* Found a corresponding VMAP.  Remap!  */

	  /* We can assume pointer == CORE_ADDR, this code is native only.  */
	  vp->tstart = (CORE_ADDR) ldi->ldinfo_textorg;
	  vp->tend = vp->tstart + ldi->ldinfo_textsize;
	  vp->dstart = (CORE_ADDR) ldi->ldinfo_dataorg;
	  vp->dend = vp->dstart + ldi->ldinfo_datasize;

	  /* The run time loader maps the file header in addition to the text
	     section and returns a pointer to the header in ldinfo_textorg.
	     Adjust the text start address to point to the real start address
	     of the text section.  */
	  vp->tstart += vp->toffs;

	  /* The objfile is only NULL for the exec file.  */
	  if (vp->objfile == NULL)
	    got_exec_file = 1;

	  /* relocate symbol table(s). */
	  vmap_symtab (vp);

	  /* There may be more, so we don't break out of the loop.  */
	}

      /* if there was no matching *vp, we must perforce create the sucker(s) */
      if (!got_one && !retried)
	{
	  add_vmap (ldi);
	  ++retried;
	  goto retry;
	}
    }
  while (ldi->ldinfo_next
	 && (ldi = (void *) (ldi->ldinfo_next + (char *) ldi)));

  /* If we don't find the symfile_objfile anywhere in the ldinfo, it
     is unlikely that the symbol file is relocated to the proper
     address.  And we might have attached to a process which is
     running a different copy of the same executable.  */
  if (symfile_objfile != NULL && !got_exec_file)
    {
      warning_begin ();
      fputs_unfiltered ("Symbol file ", gdb_stderr);
      fputs_unfiltered (symfile_objfile->name, gdb_stderr);
      fputs_unfiltered ("\nis not mapped; discarding it.\n\
If in fact that file has symbols which the mapped files listed by\n\
\"info files\" lack, you can load symbols with the \"symbol-file\" or\n\
\"add-symbol-file\" commands (note that you must take care of relocating\n\
symbols to the proper address).\n", gdb_stderr);
      free_objfile (symfile_objfile);
      symfile_objfile = NULL;
    }
  breakpoint_re_set ();
}

/* As well as symbol tables, exec_sections need relocation. After
   the inferior process' termination, there will be a relocated symbol
   table exist with no corresponding inferior process. At that time, we
   need to use `exec' bfd, rather than the inferior process's memory space
   to look up symbols.

   `exec_sections' need to be relocated only once, as long as the exec
   file remains unchanged.
 */

static void
vmap_exec ()
{
  static bfd *execbfd;
  int i;

  if (execbfd == exec_bfd)
    return;

  execbfd = exec_bfd;

  if (!vmap || !exec_ops.to_sections)
    error ("vmap_exec: vmap or exec_ops.to_sections == 0\n");

  for (i = 0; &exec_ops.to_sections[i] < exec_ops.to_sections_end; i++)
    {
      if (STREQ (".text", exec_ops.to_sections[i].the_bfd_section->name))
	{
	  exec_ops.to_sections[i].addr += vmap->tstart - vmap->tvma;
	  exec_ops.to_sections[i].endaddr += vmap->tstart - vmap->tvma;
	}
      else if (STREQ (".data", exec_ops.to_sections[i].the_bfd_section->name))
	{
	  exec_ops.to_sections[i].addr += vmap->dstart - vmap->dvma;
	  exec_ops.to_sections[i].endaddr += vmap->dstart - vmap->dvma;
	}
      else if (STREQ (".bss", exec_ops.to_sections[i].the_bfd_section->name))
	{
	  exec_ops.to_sections[i].addr += vmap->dstart - vmap->dvma;
	  exec_ops.to_sections[i].endaddr += vmap->dstart - vmap->dvma;
	}
    }
}

/* xcoff_relocate_symtab -      hook for symbol table relocation.
   also reads shared libraries.. */

void
xcoff_relocate_symtab (pid)
     unsigned int pid;
{
  int load_segs = 64; /* number of load segments */

  do
    {
  struct ld_info *ldi;
      int rc;

      ldi = (void *) alloca (load_segs * sizeof (*ldi));
      if (ldi == 0)
	perror_with_name ("xcoff_relocate_symtab");

  /* According to my humble theory, AIX has some timing problems and
     when the user stack grows, kernel doesn't update stack info in time
     and ptrace calls step on user stack. That is why we sleep here a little,
     and give kernel to update its internals. */

  usleep (36000);

  errno = 0;
      rc = ptrace (PT_LDINFO, pid, (PTRACE_ARG3_TYPE) ldi,
	      load_segs * sizeof (*ldi), (int *) ldi);
      if (rc == -1)
        {
	if (errno == ENOMEM)
	  load_segs *= 2;
	else
    perror_with_name ("ptrace ldinfo");
        }
      else
	{
  vmap_ldinfo (ldi);
	  vmap_exec (); /* relocate the exec and core sections as well. */
	}
    } while (rc == -1);
}

/* Core file stuff.  */

/* Relocate symtabs and read in shared library info, based on symbols
   from the core file.  */

void
xcoff_relocate_core (target)
     struct target_ops *target;
{
/* Offset of member MEMBER in a struct of type TYPE.  */
#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((int) &((TYPE *)0)->MEMBER)
#endif

/* Size of a struct ld_info except for the variable-length filename.  */
#define LDINFO_SIZE (offsetof (struct ld_info, ldinfo_filename))

  sec_ptr ldinfo_sec;
  int offset = 0;
  struct ld_info *ldip;
  struct vmap *vp;

  /* Allocated size of buffer.  */
  int buffer_size = LDINFO_SIZE;
  char *buffer = xmalloc (buffer_size);
  struct cleanup *old = make_cleanup (free_current_contents, &buffer);

  /* FIXME, this restriction should not exist.  For now, though I'll
     avoid coredumps with error() pending a real fix.  */
  if (vmap == NULL)
    error
      ("Can't debug a core file without an executable file (on the RS/6000)");

  ldinfo_sec = bfd_get_section_by_name (core_bfd, ".ldinfo");
  if (ldinfo_sec == NULL)
    {
    bfd_err:
      fprintf_filtered (gdb_stderr, "Couldn't get ldinfo from core file: %s\n",
			bfd_errmsg (bfd_get_error ()));
      do_cleanups (old);
      return;
    }
  do
    {
      int i;
      int names_found = 0;

      /* Read in everything but the name.  */
      if (bfd_get_section_contents (core_bfd, ldinfo_sec, buffer,
				    offset, LDINFO_SIZE) == 0)
	goto bfd_err;

      /* Now the name.  */
      i = LDINFO_SIZE;
      do
	{
	  if (i == buffer_size)
	    {
	      buffer_size *= 2;
	      buffer = xrealloc (buffer, buffer_size);
	    }
	  if (bfd_get_section_contents (core_bfd, ldinfo_sec, &buffer[i],
					offset + i, 1) == 0)
	    goto bfd_err;
	  if (buffer[i++] == '\0')
	    ++names_found;
	}
      while (names_found < 2);

      ldip = (struct ld_info *) buffer;

      /* Can't use a file descriptor from the core file; need to open it.  */
      ldip->ldinfo_fd = -1;

      /* The first ldinfo is for the exec file, allocated elsewhere.  */
      if (offset == 0)
	vp = vmap;
      else
	vp = add_vmap (ldip);

      offset += ldip->ldinfo_next;

      /* We can assume pointer == CORE_ADDR, this code is native only.  */
      vp->tstart = (CORE_ADDR) ldip->ldinfo_textorg;
      vp->tend = vp->tstart + ldip->ldinfo_textsize;
      vp->dstart = (CORE_ADDR) ldip->ldinfo_dataorg;
      vp->dend = vp->dstart + ldip->ldinfo_datasize;

      /* The run time loader maps the file header in addition to the text
         section and returns a pointer to the header in ldinfo_textorg.
         Adjust the text start address to point to the real start address
         of the text section.  */
      vp->tstart += vp->toffs;

      /* Unless this is the exec file,
         add our sections to the section table for the core target.  */
      if (vp != vmap)
	{
	  struct section_table *stp;

	  target_resize_to_sections (target, 2);
	  stp = target->to_sections_end - 2;

	  stp->bfd = vp->bfd;
	  stp->the_bfd_section = bfd_get_section_by_name (stp->bfd, ".text");
	  stp->addr = vp->tstart;
	  stp->endaddr = vp->tend;
	  stp++;

	  stp->bfd = vp->bfd;
	  stp->the_bfd_section = bfd_get_section_by_name (stp->bfd, ".data");
	  stp->addr = vp->dstart;
	  stp->endaddr = vp->dend;
	}

      vmap_symtab (vp);
    }
  while (ldip->ldinfo_next != 0);
  vmap_exec ();
  breakpoint_re_set ();
  do_cleanups (old);
}

int
kernel_u_size ()
{
  return (sizeof (struct user));
}

/* Under AIX, we have to pass the correct TOC pointer to a function
   when calling functions in the inferior.
   We try to find the relative toc offset of the objfile containing PC
   and add the current load address of the data segment from the vmap.  */

static CORE_ADDR
find_toc_address (pc)
     CORE_ADDR pc;
{
  struct vmap *vp;

  for (vp = vmap; vp; vp = vp->nxt)
    {
      if (pc >= vp->tstart && pc < vp->tend)
	{
	  /* vp->objfile is only NULL for the exec file.  */
	  return vp->dstart + get_toc_offset (vp->objfile == NULL
					      ? symfile_objfile
					      : vp->objfile);
	}
    }
  error ("Unable to find TOC entry for pc 0x%x\n", pc);
}

/* Register that we are able to handle rs6000 core file formats. */

static struct core_fns rs6000_core_fns =
{
  bfd_target_coff_flavour,		/* core_flavour */
  default_check_format,			/* check_format */
  default_core_sniffer,			/* core_sniffer */
  fetch_core_registers,			/* core_read_registers */
  NULL					/* next */
};

void
_initialize_core_rs6000 ()
{
  /* Initialize hook in rs6000-tdep.c for determining the TOC address when
     calling functions in the inferior.  */
  find_toc_address_hook = &find_toc_address;

  /* For native configurations, where this module is included, inform
     the xcoffsolib module where it can find the function for symbol table
     relocation at runtime. */
  xcoff_relocate_symtab_hook = &xcoff_relocate_symtab;
  add_core_fns (&rs6000_core_fns);
}
