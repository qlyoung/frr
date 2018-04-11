.. _vtysh:

*****
VTYSH
*****

.. program:: vtysh 

FRR is a collection of programs that each handle different functions of the
full routing protocol suite. Each of these programs has their own configuration
and each is driven from its own command line interface, which they optionally
expose via telnet (see documentation on each daemon for the address and port
they bind to). However, it is inconvenient to open multiple telnet sessions to
make changes to different programs in the suite. To solve this problem, FRR
provides *vtysh*, which acts as a unified shell for all programs in the suite.

It is enabled by default at build time, but can be disabled through the
``--disable-vtysh`` option to the configure script.

.. seealso:: command-line-interface

Design
======
*vtysh* is complicated and performs many tasks, some of which may be
surprising. We will focus on its core functionality first.

Daemon connections
------------------
Every daemon opens a socket specifically for VTYSH to connect to. These are
placed in |INSTALL_PREFIX_STATE| and are named like this:

.. code-block:: shell

   $ ls -ls /var/run/frr
   total 52
   4 -rw-r--r-- 1 frr  frr    6  babeld.pid
   0 srwxrwx--- 1 frr  frrvty 0  babeld.vty=
   4 -rw-r--r-- 1 frr  frr    6  bgpd.pid
   0 srwxrwx--- 1 frr  frrvty 0  bgpd.vty=
   4 -rw-r--r-- 1 frr  frr    6  eigrpd.pid
   0 srwxrwx--- 1 frr  frrvty 0  eigrpd.vty=
   4 -rw-r--r-- 1 frr  frr    6  isisd.pid
   0 srwxrwx--- 1 frr  frrvty 0  isisd.vty=
   4 -rw-r--r-- 1 frr  frr    6  ospf6d.pid
   0 srwxrwx--- 1 frr  frrvty 0  ospf6d.vty=
   4 -rw-r--r-- 1 frr  frr    6  ospfd.pid
   0 srwxrwx--- 1 frr  frrvty 0  ospfd.vty=
   4 -rw-r--r-- 1 frr  frr    6  pbrd.pid
   0 srwxrwx--- 1 frr  frrvty 0  pbrd.vty=
   4 -rw-r--r-- 1 frr  frr    6  pimd.pid
   ...

In :file:`vtysh/vtysh.c` there is a hardcoded list of daemons. When adding a
new daemon, you must update this list or *vtysh* will not be able to talk to
the daemon.

On startup *vtysh* will attempt to connect to all of the daemons in this list.
Provided you have initialized the VTY and CLI subsystems properly in your
daemon, this should work automatically. Once connected *vtysh* will issue the
``enable`` command to put itself in privileged mode, then yield to the user.
From this point the user is presented with a similar prompt as they would be
when connecting to a daemon over telnet. All the usual line-editing keyboard
shortcuts (which use GNU Readline under the hood), autocompletion, tab
completion, ``?``-completion etc. will work. Reason tells us that one of two
things is going on for this to work. Either *vtysh* asks the daemons for the
CLI information and structure at runtime, or it already has it baked in at
compile time. As of this writing, the situation is the latter. This is
accomplished by scraping C source files, which brings us to the next section.

extract.pl
----------
*vtysh* is similar to daemons in that it links ``libfrr`` and has its own CLI
graph constructed from DEFUNs of various types, placed in its source files and
defined in the usual way. However, the same CLI graph also contains all the
commands defined for each daemon. This is accomplished by running a Perl script
over source files that contain CLI, copying the DEFUNs, running some
replacements on them and placing them into a generated source file named
:file:`vtysh/vtysh_cmd.c`. The Perl script is located at
:file:`vtysh/extract.pl.in` and is invoked by Automake at compile time on files
named in :file:`vtysh/Makefile.am`. The script remembers the directory it got
each command definition from and sets an attribute in the generated definition
that designates the daemon from which it was extracted. This becomes relevant
later.

A generated command looks like this:

.. code-block:: c

   DEFSH (VTYSH_BGPD, bgp_default_ipv4_unicast_cmd_vtysh,
          "bgp default ipv4-unicast",
          "BGP specific commands\n"
          "Configure BGP defaults\n"
          "Activate ipv4-unicast for a peer by default\n")

Notice that the first argument is a flag indicating which daemon this command
was extracted from. The second argument is the name of the command structure
plus ``_vtysh``. All other arguments are identical. The definition macro itself
is `DEFSH`, which does not expect a body after the definition. The body is not
included for extracted commands because VTYSH does not actually execute the
command, but instead merely passes the command along to whichever daemon it
came from.

Of course there are many exceptions to this due to the nature of this code
generation. The immediate one is ``DEFUN_NOSH``, which daemons can use to
prevent ``extract.pl`` from copying that particular command. This one tends to
be used in two places. The most obvious use is in :file:`lib/`, because *vtysh*
itself links :file:`lib/` and so using a regular DEFUN here results in the
command being defined twice (although there are exceptions to this as well).
The second use is an artifact of the complete lack of state synchronization
between daemons and *vtysh*, is complicated and subtle, and deserves its own
section...

Mode Synchronization
--------------------


Configuration Dumping
---------------------


Known Issues
------------
