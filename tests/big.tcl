package require DiffUtil
puts "P:[info procs DiffUtil::*]"
#DiffUtil::diffFiles -b tests/bigfilex1 tests/bigfilex2

#set s1 {AjkHFAkjaaslkJADADlkhaDkhALDKJHDA}
#set s2 {AjkhfAkqqAslkJAdaDlsxaDkhAftKJHDA}

#set n 10000
#puts [time {DiffUtil::diffStrings $s1 $s2} $n]
#puts [time {DiffUtil::diffStrings2 $s1 $s2} $n]

set f1 tests/creole1.xml
set f2 tests/creole2.xml

set n 1
puts [time {DiffUtil::diffFiles -pivot 5 $f1 $f2} $n]
puts [time {DiffUtil::diffFiles -pivot 10 $f1 $f2} $n]
puts [time {DiffUtil::diffFiles -pivot 5 $f1 $f2} $n]
puts [time {DiffUtil::diffFiles -pivot 10 $f1 $f2} $n]
