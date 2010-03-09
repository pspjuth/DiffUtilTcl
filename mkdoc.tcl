#!/bin/sh
# Inspired by sak.tcl from tcllib distribution
# -*- tcl -*- \
exec tclsh "$0" ${1+"$@"}

proc get_input {f} {return [read [set if [open $f r]]][close $if]}
proc write_out {f text} {
    catch {file delete -force $f}
    puts -nonewline [set of [open $f w]] $text
    close $of
}

package require doctools
foreach {format ext} {nroff n html html} {
    ::doctools::new dt -format $format -module diffutil -file doc/diffutil.man
    write_out doc/diffutil.$ext [dt format [get_input doc/diffutil.man]]
    dt destroy
}
