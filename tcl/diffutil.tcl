#----------------------------------------------------------------------
#
#  This is a Tcl implementation of the DiffUtil package
#
#  Copyright (c) 2004, Peter Spjuth
#
#  This package is starting out as a refactoring of diff code from
#  Eskil, and will be released as a separate package when mature.
#
#----------------------------------------------------------------------
# $Revision: 1.11 $
#----------------------------------------------------------------------

package provide DiffUtil 0.2

namespace eval DiffUtil {
    namespace export diffFiles diffStrings
}

# Figure out a place to store temporary files.
proc DiffUtil::LocateTmp {} {
    variable tmpdir
    if {[info exists tmpdir] && $tmpdir != ""} return

    set candidates {}
    if {[info exists ::env(TEMP)]} {
        lappend candidates $::env(TEMP)
    }
    if {[info exists ::env(TMP)]} {
        lappend candidates $::env(TMP)
    }
    lappend candidates /tmp . ~
    foreach cand $candidates {
        set cand [file normalize $cand]
        if {[file isdirectory $cand] && [file writable $cand]} {
            set tmpdir $cand
            return
        }
    }
    # Panic?
    set tmpdir .
}

# Generate a temporary file name
proc DiffUtil::TmpFile {} {
    variable tmpdir
    variable tmpcnt
    LocateTmp
    if {[info exists tmpcnt]} {
        incr tmpcnt
    } else {
        set tmpcnt 0
    }
    set name [file join $tmpdir "diffutil[pid]a$tmpcnt"]
    return $name
}

# Locate a diff executable
proc DiffUtil::LocateDiffExe {{appFile {}}} {
    variable diffexe
    if {[info exists diffexe]} return

    if {![string equal [auto_execok diff] ""]} {
        set diffexe diff
        return
    }

    # Build a list of possible directories.
    set dirs {}
    if {$appFile ne ""} {
        set thisDir [file normalize [file dirname $appFile]]
        lappend dirs $thisDir

        # Are we in a starkit?
        if {[string match "*/lib/app-*" $thisDir]} {
            lappend dirs [file dirname [file dirname [file dirname $thisDir]]]
            # And for a starpack
            lappend dirs [file dirname [info nameofexecutable]]
        }
    }
    lappend dirs c:/bin

    foreach dir $dirs {
        set try [file join $dir diff.exe]
        if {[file exists $try]} {
            set diffexe $try
            return
        }
    }

    return -code error "Could not locate any external diff executable."
}

# Execute the external diff and parse result
# start1/2 is the line number of the first line in each file
proc DiffUtil::ExecDiffFiles {diffopts file1 file2 {start1 1} {start2 1}} {
    variable diffexe

    LocateDiffExe

    # A special case to diff files in a virtual file system
    if {[lindex [file system $file1] 0] ne "native"} {
        set tmpfile1 [TmpFile]
        file copy -force -- $file1 $tmpfile1
        set file1 $tmpfile1
    }
    if {[lindex [file system $file2] 0] ne "native"} {
        set tmpfile2 [TmpFile]
        file copy -force -- $file2 $tmpfile2
        set file2 $tmpfile2
    }

    # Execute diff
    set differr [catch {eval exec \$diffexe $diffopts \
            \$file1 \$file2} diffres]

    # Clean up any temporary files
    if {[info exists tmpfile1]} {
        file delete -force $tmpfile1
    }
    if {[info exists tmpfile2]} {
        file delete -force $tmpfile2
    }

    # Parse the result
    set result {}
    foreach line [split $diffres "\n"] {
        if {[string match {[0-9]*} $line]} {
            lappend result $line
        }
    }

    if {[llength $result] == 0} {
        if {$differr == 1} {
            return -code error $diffres
        } else {
            return {}
        }
    }

    incr start1 -1
    incr start2 -1
    set diffs {}
    foreach i $result {
        if {![regexp {(.*)([acd])(.*)} $i -> l c r]} {
            return -code error "No regexp match for $i"
        } else {
            if {[regexp {([0-9]+),([0-9]+)} $l apa start stop]} {
                set n1 [expr {$stop - $start + 1}]
                set line1 $start
            } else {
                set n1 1
                set line1 $l
            }
            if {[regexp {([0-9]+),([0-9]+)} $r apa start stop]} {
                set n2 [expr {$stop - $start + 1}]
                set line2 $start
            } else {
                set n2 1
                set line2 $r
            }
            switch $c {
                a {
                    # Gap in left, new in right
                    lappend diffs [list [expr {$line1 + 1 + $start1}] 0 \
                            [expr {$line2 + $start2}] $n2]
                }
                c {
                    lappend diffs [list [expr {$line1 + $start1}] $n1 \
                            [expr {$line2 + $start2}] $n2]
                }
                d {
                    # Gap in right, new in left
                    lappend diffs [list [expr {$line1 + $start1}] $n1 \
                            [expr {$line2 + 1 + $start2}] 0]
                }
            }
        }
    }
    return $diffs
}

# Compare two files
# Usage: diffFiles ?options? file1 file2
# -nocase -i  : Ignore case
# -b          : Ignore space changes
# -w          : Ignore all spaces
# -align list : Align lines
# -range list : Diff only a range of the files.
#               The list is {first1 last1 first2 last2}
#
# Returns a list of differences, each in a four element list.
# {LineNumber1 NumberOfLines1 LineNumber2 NumberOfLines2}
proc DiffUtil::diffFiles {args} {
    variable diffexe

    if {[llength $args] < 2} {
        return -code error "wrong # args"
    }
    set file1 [lindex $args end-1]
    set file2 [lindex $args end]
    set args [lrange $args 0 end-2]

    set diffopts {}
    set opts(-align) {}
    set opts(-range) {}
    set opts(-noempty)  0  ;# Allowed but ignored
    set opts(-regsubRE) {}
    set opts(-regsubSub) {}

    set value ""
    foreach arg $args {
        if {$value != ""} {
            if {$value eq "-regsub"} {
                foreach {RE Sub} $arg {
                    if {[catch {regsub -all $RE "a string" $Sub Dummy}]} {
                        return -code error "Bad regexp: '$RE'"
                    } else {
                        lappend opts(-regsubRE) $RE
                        lappend opts(-regsubSub) $Sub
                    }
                }
            } else {
                set opts($value) $arg
                set value ""
            }
            continue
        }
        switch -- $arg {
            -i - -b - -w { lappend diffopts $arg }
            -nocase      { lappend diffopts -i }
            -align -
            -regsub -
            -range { set value $arg }
            -noempty { set opts($arg) 1 }
            -nodigit {
                lappend opts(-regsubRE) {\d+}
                lappend opts(-regsubSub) "0"
            }
            default {
                return -code error "bad option \"$arg\""
            }
        }
    }

    # The simple case
    if {[llength $opts(-align)] == 0     && \
            [llength $opts(-range)] == 0 && \
                [llength $opts(-regsubRE)] == 0} {
        return [ExecDiffFiles $diffopts $file1 $file2]
    }
    if {[llength $opts(-range)] != 0 && [llength $opts(-range)] != 4} {
        return -code error "bad range \"$opts(-range)\""
    }
    if {([llength $opts(-align)] % 2) != 0} {
        return -code error "bad align \"$opts(-align)\""
    }

    if {[llength $opts(-range)] != 0} {
        foreach {from1 to1 from2 to2} $opts(-range) break
        if {$from1 < 1 || $from1 > $to1 || $from2 < 0 || $from2 > $to2} {
            return -code error "bad range \"$opts(-range)\""
        }
    } else {
        set from1 1
        set from2 1
        set to1 2000000000
        set to2 2000000000
    }
    
    # Handle alignment by copying the object block by block to temporary
    # files and diff each block separately.

    set ch1 [open $file1 "r"]
    set ch2 [open $file2 "r"]
    set n1 1
    set n2 1
    
    # Skip lines from files to get the range
    while {$n1 < $from1 && [gets $ch1 line] >= 0} {
        incr n1
    }
    while {$n2 < $from2 && [gets $ch2 line] >= 0} {
        incr n2
    }

    set diffs {}
    set tmp1 [TmpFile]
    set tmp2 [TmpFile]
    set endlines {}
    set last1 $from1
    set last2 $from2

    if {[llength $opts(-align)] > 0} {
        set apairs {}
        foreach {align1 align2} $opts(-align) {
            lappend apairs [list $align1 $align2]
        }
        set apairs [lsort -index 0 -integer $apairs]
        foreach apair $apairs {
            foreach {align1 align2} $apair break
            # Skip aligns that are out of range
            if {$align1 < $last1 || $align1 > $to1} continue
            if {$align2 < $last2 || $align2 > $to2} continue
            # Add the align to the list of boundaries
            if {$align1 != $last1 || $align2 != $last2} {
                lappend endlines [expr {$align1 - 1}] [expr {$align2 - 1}]
            }
            lappend endlines $align1 $align2
            set last1 [expr {$align1 + 1}]
            set last2 [expr {$align2 + 1}]
        }
    }
    # End of range. If no align was used, this is the only chunk.
    lappend endlines $to1 $to2

    foreach {align1 align2} $endlines {
        # Copy up to the align line to temporary files
        set cho1 [open $tmp1 "w"]
        set start1 $n1
        while {$n1 <= $align1 && [gets $ch1 line] >= 0} {
            foreach RE $opts(-regsubRE) Sub $opts(-regsubSub) {
                regsub -all $RE $line $Sub line
            }
            puts $cho1 $line
            incr n1
        }
        close $cho1
        set cho2 [open $tmp2 "w"]
        set start2 $n2
        while {$n2 <= $align2 && [gets $ch2 line] >= 0} {
            foreach RE $opts(-regsubRE) Sub $opts(-regsubSub) {
                regsub -all $RE $line $Sub line
            }
            puts $cho2 $line
            incr n2
        }
        close $cho2

        #puts "A: $align1 $align2 $start1 $start2"
        set differr [catch {ExecDiffFiles $diffopts $tmp1 $tmp2 \
                $start1 $start2} diffres]
        file delete -force $tmp1 $tmp2
        if {$differr} { ###nocoverage
            return -code error $diffres
        }
        # Add diffres's elements to diffs
        ##nagelfar ignore Found constant \"diffs\"
        eval [linsert $diffres 0 lappend diffs]
        #puts "D: $diffs"
    }

    return $diffs
}

# Recursively look for common substrings in strings s1 and s2
# The strings are known to not have anything in common at start or end.
# The return value is like for diffString, except the first pair is not
# equal. An uneven number of pairs is always returned.
# This is sort of a Longest Common Subsequence algorithm but with
# a preference for long consecutive substrings, and it does not look
# for really small substrings.
proc DiffUtil::CompareMidString {s1 s2 words nocase} {
    set len1 [string length $s1]
    set len2 [string length $s2]
    if {$nocase} {
        set cs1 [string tolower $s1]
        set cs2 [string tolower $s2]
    } else {
        set cs1 $s1
        set cs2 $s2
    }
    # Is s1 a substring of s2 ?
    if {$len1 < $len2} {
        set t [string first $cs1 $cs2]
        if {$t != -1} {
            set left2 [string range $s2 0 [expr {$t - 1}]]
            set mid2 [string range $s2 $t [expr {$t + $len1 - 1}]]
            set right2 [string range $s2 [expr {$t + $len1}] end]
            return [list "" $left2 $s1 $mid2 "" $right2]
        }
    }

    # Is s2 a substring of s1 ?
    if {$len2 < $len1} {
        set t [string first $cs2 $cs1]
        if {$t != -1} {
            set left1 [string range $s1 0 [expr {$t - 1}]]
            set mid1 [string range $s1 $t [expr {$t + $len2 - 1}]]
            set right1 [string range $s1 [expr {$t + $len2}] end]
            return [list $left1 "" $mid1 $s2 $right1 ""]
        }
    }

    # Are they too short to be considered ?
    if {$len1 < 4 || $len2 < 4} {
        return [list $s1 $s2]
    }

    set foundlen -1
    set minlen 2 ;# The shortest common substring we detect is 3 chars

    # Find the longest string common to both strings
    for {set t 0 ; set u $minlen} {$u < $len1} {incr t ; incr u} {
        set i [string first [string range $cs1 $t $u] $cs2]
        if {$i >= 0} {
            for {set p1 [expr {$u + 1}]; set p2 [expr {$i + $minlen + 1}]} \
                    {$p1 < $len1 && $p2 < $len2} {incr p1 ; incr p2} {
                if {[string index $cs1 $p1] ne [string index $cs2 $p2]} {
                    break
                }
            }
            if {$words} {
                set newt $t
                if {($t > 0 && [string index $cs1 [expr {$t - 1}]] ne " ") || \
                    ($i > 0 && [string index $cs2 [expr {$i - 1}]] ne " ")} {
                    for {} {$newt < $p1} {incr newt} {
                        if {[string index $cs1 $newt] eq " "} break
                    }
                }

                set newp1 [expr {$p1 - 1}]
                if {($p1 < $len1 && [string index $cs1 $p1] ne " ") || \
                    ($p2 < $len2 && [string index $cs2 $p2] ne " ")} {
                    for {} {$newp1 > $newt} {incr newp1 -1} {
                        if {[string index $cs1 $newp1] eq " "} break
                    }
                }
                incr newp1

                if {$newp1 - $newt > $minlen} {
                    set foundlen [expr {$newp1 - $newt}]
                    set found1 $newt
                    set found2 [expr {$i + $newt - $t}]
                    set minlen $foundlen
                    set u [expr {$t + $minlen}]
                }
            } else {
                set foundlen [expr {$p1 - $t}]
                set found1 $t
                set found2 $i
                set minlen $foundlen
                set u [expr {$t + $minlen}]
            }
        }
    }

    if {$foundlen == -1} {
        return [list $s1 $s2]
    } else {
        set left1 [string range $s1 0 [expr {$found1 - 1}]]
        set mid1 [string range $s1 $found1 [expr {$found1 + $foundlen - 1}]]
        set right1 [string range $s1 [expr {$found1 + $foundlen}] end]

        set left2 [string range $s2 0 [expr {$found2 - 1}]]
        set mid2 [string range $s2 $found2 [expr {$found2 + $foundlen - 1}]]
        set right2 [string range $s2 [expr {$found2 + $foundlen}] end]

        set leftl  [CompareMidString $left1  $left2  $words $nocase]
        set rightl [CompareMidString $right1 $right2 $words $nocase]

        return [concat $leftl [list $mid1 $mid2] $rightl]
    }
}

# Compare two strings
# Usage: diffStrings ?options? string1 string2
# -nocase -i  : Ignore case
# -b          : Ignore space changes
# -w          : Ignore all spaces
# -words      : Align changes to words
#
# The return value is a list where the first, third etc. element comes
# from the first string and if concatenated will rebuild the first string.
# The second, fourth etc. element, comes from the second string.
# The first pair will be equal (and may be empty strings), the second pair
# differs, and so on.
# {eq1 eq2 d1 d2 eq1 eq2 d1 d2 eq1 eq2}
#
# returns a list of substrings from alternating strings
# str1sub1 str2sub1 str1sub2 str2sub2...
# str1sub* concatenated gives string1
# str2sub* concatenated gives string2
# str1subN and str2subN are equal when N is odd, not equal when N is even
proc DiffUtil::diffStrings {args} {
    if {[llength $args] < 2} {
        return -code error "wrong # args"
    }
    set string1 [lindex $args end-1]
    set string2 [lindex $args end]
    set args [lrange $args 0 end-2]
    
    set diffopts {}
    set opts(-nocase) 0
    set opts(-space) 0
    set opts(-words) 0
    set value ""
    foreach arg $args {
        switch -- $arg {
            -b           {set opts(-space)  1}
            -w           {set opts(-space)  2}
            -words       {set opts(-words)  1}
            -nocase - -i {set opts(-nocase) 1}
            default {
                return -code error "bad option \"$arg\""
            }
        }
    }
    
    # This processes the lines from both ends first.
    # A typical line has few changes thus this gets rid of most
    # equalities. The middle part is then optionally parsed further.

    if {$opts(-space) != 0} {
        # Skip white space in both ends

        set apa1 [string trimleft $string1]
        set leftp1 [expr {[string length $string1] - [string length $apa1]}]
        set mid1 [string trimright $string1]

        set apa2 [string trimleft $string2]
        set leftp2 [expr {[string length $string2] - [string length $apa2]}]
        set mid2 [string trimright $string2]
    } else {
        # If option "ignore nothing" is selected
        set apa1 $string1
        set leftp1 0
        set mid1 $string1
        set apa2 $string2
        set leftp2 0
        set mid2 $string2
    }

    # Check for matching left chars/words.
    # leftp1 and leftp2 will be the indicies of the first difference

    set len1 [string length $apa1]
    set len2 [string length $apa2]
    set len [expr {$len1 < $len2 ? $len1 : $len2}]
    # Do -nocase in its own loop to not slow down the other
    if {$opts(-nocase)} {
        for {set t 0; set s 0; set flag 0} {$t < $len} {incr t} {
            set c [string index $apa1 $t]
            if {![string equal -nocase $c [string index $apa2 $t]]} {
                incr flag 2
                break
            }
            if {$c eq " "} {
                set s $t
                set flag 1
            }
        }
    } else {
        for {set t 0; set s 0; set flag 0} {$t < $len} {incr t} {
            if {[set c [string index $apa1 $t]] ne [string index $apa2 $t]} {
                incr flag 2
                break
            }
            if {$c eq " "} {
                set s $t
                set flag 1
            }
        }
    }

    if {$opts(-words) == 0} {
        incr leftp1 $t
        incr leftp2 $t
    } else {
        if {$flag < 2} {
            # If no difference was found during the parse, consider all equal
            set s $len
        } elseif {$flag == 3} {
            # s points to the last seen space, consider the char after that
            # as the first difference
            incr s
        } ;# The else case is when no space was seen, consider all different
        incr leftp1 $s
        incr leftp2 $s
    }

    # Check for matching right chars/words.
    # t1 and t2 will be the indicies of the last difference

    set len1 [string length $mid1]
    set len2 [string length $mid2]

    set t1 [expr {$len1 - 1}]
    set t2 [expr {$len2 - 1}]
    set s1 $t1
    set s2 $t2
    set flag 0
    # Do -nocase in its own loop to not slow down the other
    if {$opts(-nocase)} {
        for {} {$t1 >= $leftp1 && $t2 >= $leftp2} {incr t1 -1; incr t2 -1} {
            set c [string index $mid1 $t1]
            if {![string equal -nocase $c [string index $mid2 $t2]]} {
                incr flag 2
                break
            }
            if {$c eq " "} {
                set s1 $t1
                set s2 $t2
                set flag 1
            }
        }
    } else {
        for {} {$t1 >= $leftp1 && $t2 >= $leftp2} {incr t1 -1; incr t2 -1} {
            if {[set c [string index $mid1 $t1]] ne [string index $mid2 $t2]} {
                incr flag 2
                break
            }
            if {$c eq " "} {
                set s1 $t1
                set s2 $t2
                set flag 1
            }
        }
    }
    if {$opts(-words)} {
        if {$flag >= 2} {
            if {$flag == 3} {
                incr s1 -1
                incr s2 -1
            }
            set t1 $s1
            set t2 $s2
        }
    }

    # Make the result
    if {$leftp1 > $t1 && $leftp2 > $t2} {
        set res [list $string1 $string2]
    } else {
        set right1 [string range $string1 [expr {$t1 + 1}] end]
        set mid1 [string range $string1 $leftp1 $t1]
        set left1 [string range $string1 0 [expr {$leftp1 - 1}]]

        set right2 [string range $string2 [expr {$t2 + 1}] end]
        set mid2 [string range $string2 $leftp2 $t2]
        set left2 [string range $string2 0 [expr {$leftp2 - 1}]]

        if {$mid1 ne "" && $mid2 ne ""} {
            set midl [CompareMidString $mid1 $mid2 $opts(-words) $opts(-nocase)]
            set res [concat [list $left1 $left2] $midl [list $right1 $right2]]
        } else {
            set res [list $left1 $left2 $mid1 $mid2 $right1 $right2]
        }
    }
    return $res
}
