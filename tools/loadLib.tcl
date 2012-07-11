# Provide a load function for a Starkit version of the package.

namespace eval ::DiffUtil {
    # return a platform designator, including both OS and machine
    proc _platform {} {
        global tcl_platform
        set plat [lindex $tcl_platform(os) 0]
        set mach $tcl_platform(machine)
        switch -glob -- $mach {
            sun4* { set mach sparc }
            intel -
            i*86* { set mach x86 }
            "Power Macintosh" { set mach ppc }
        }
        return "$plat-$mach"
    }
    proc _load {dir libfile version} {
        set root [file rootname $libfile]
        if {$::tcl_platform(platform) == "windows"} {
            set root [string map {. {}} $root]
        }
        set libfile $root[info sharedlibextension]
        set fulllibfile [file join $dir [_platform] $libfile]
        if {[catch {uplevel \#0 [list load $fulllibfile]}]} {
            set ::DiffUtil::DebugLibFile $fulllibfile
            uplevel \#0 [list source [file join $dir tcl diffutil.tcl]]
        }
    }
}
