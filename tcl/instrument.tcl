#!/bin/sh
#----------------------------------------------------------------------
# A simple instrumenter for code coverage
#----------------------------------------------------------------------
# $Revision: 1.1 $
#----------------------------------------------------------------------
# the next line restarts using tclsh \
exec tclsh "$0" "$@"

set ch [open diffutil.tcl r]
set cho [open diffutili.tcl w]
if {$argc == 0} {
    set instrument 1
    set endRE ""
} else {
    set instrument 0
    set startRE "proc (?:DiffUtil::)?(?:[join $argv |])"
    set endRE "^\}\\s*$"
}
set commRE "^\\s*\#"
# Missing: switch branches
set iRE {(?:if|elseif|else|while|for|foreach) .*\{\s*$}
set lineNo 0
set prev ""
while {[gets $ch line] >= 0} {
    incr lineNo
    if {$prev ne ""} {
        set line $prev\n$line
        set prev ""
    }
    if {[string index $line end] eq "\\"} {
        set prev $line
        continue
    }
    if {$instrument} {
        if {$endRE ne "" && [regexp $endRE $line]} {
            set instrument 0
            puts $cho $line
            continue
        }
        if {![regexp $commRE $line]&& [regexp $iRE $line]} {
            append line " [list set ::_instrument($lineNo) 1]"
            set ::_instrument($lineNo) 1
        }
    } else {
        if {[regexp $startRE $line]} {
            set instrument 1
        }
    }
    puts $cho $line
}
close $ch

# Enter a cleanup function that will disply the results when the
# tests ends.
set body {
    set old [pwd]
    cd tcl

    puts "Checking instrumenting result"
    set ch [open _instrument_result w]
    puts $ch [join [lsort -integer [array names ::_instrument]] \n]
    close $ch
    set ch [open {|diff _instrument_lines _instrument_result}]
    while {[gets $ch line] >= 0} {
        if {[regexp {<|>} $line]} {
            puts $line
        }
    }
    catch {close $ch}
    file delete -force _instrument_result
    file delete -force diffutili.tcl
    file delete -force _instrument_lines
    file delete -force pkgIndex.tcl
    cd $old
}

puts $cho [list proc tcltest::cleanupTestsHook {} $body]
close $cho

# Dump the instrumented lines to a file.
set ch [open _instrument_lines w]
puts $ch [join [lsort -integer [array names ::_instrument]] \n]
close $ch

# Change pkgIndex.tcl to point at the instrumented file.
set ch [open pkgIndex.tcl r]
set data [read $ch]
close $ch
regsub {diffutil\.tcl} $data {diffutili.tcl} data
set ch [open pkgIndex.tcl w]
puts -nonewline $ch $data
close $ch
