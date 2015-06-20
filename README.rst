=========
pipepulse
=========

---------------------------------------------------
write stats to a file as bytes flow through a pipe.
---------------------------------------------------

:Author: michiel@michielbuddingh.net
:Date: 2015-06-20
:Version: 0.1
:Manual section: 1
:Manual section: utilities

SYNOPSIS
--------

::
   
    ... | pipepulse --out=bytes.piped [--every 25s] [--per 1M] | ...

		 
DESCRIPTION
-----------

``pipepulse`` periodically writes how many bytes have been transferred
through the pipe to a file, or to ``STDERR``.  If the minimum rate is
not met, the file is not updated.

OPTIONS
-------

-o\, --out=<bytes.piped>
   Write the number of bytes transferred to the file ``bytes.piped`` The
   file will contain a single line, stating the number of bytes
   transferred in the most recent period, and the number of bytes
   transferred in total, separated by a single tab character.

-E\, --stderr
   Write to ``STDERR``.  Continuously writes the number of bytes piped
   through to the standard error output.  Like **--out**, both the
   bytes written in the current period, and the total number of bytes are
   written.

-p\, --per=<threshold>
   Only write stats if the number of bytes written within a period
   exceeds this threshold.  The number must be written as bytes (*-b*),
   kilobytes (*-k*), Megabytes (*-M*) or Gigabytes (*-G*).  It may also be
   0b; in that case the **--out** file is updated periodically as long
   as the process is running.

-e\, --every=<period>
   Write the stats every *period*, unless the threshold specified by
   **--per** is not met.  The period must be specified in either
   seconds (*-s*), minutes (*-m*), hours (*-h*) or days (*-d*).

EXAMPLES
--------

``... | pipepulse --out=heartbeat --every=60s --per=0b | ...``
   update the file heartbeat every 60s, until the pipe is closed by the
   writer or receiver process.  This can be used for monitoring.

``... | pipepulse -E -p 0 | ...``
   output the number of bytes sent through the pipe to standard error
   every ten seconds.

CORNER CASES
------------

If the pipe is closed before the period is over, the file is updated
if the threshold has been reached.

BUGS
----
``pipepulse`` uses Linux-specific interfaces.
