package require DiffUtil
puts "P:[info procs DiffUtil::*]"
#DiffUtil::diffFiles -b tests/bigfilex1 tests/bigfilex2

set s1 {AjkHFAkjaaslkJADADlkhaDkhALDKJHDA}
set s2 {AjkhfAkqqAslkJAdaDlsxaDkhAftKJHDA}

set n 10000
puts [time {DiffUtil::diffStrings $s1 $s2} $n]
#puts [time {DiffUtil::diffStrings2 $s1 $s2} $n]
