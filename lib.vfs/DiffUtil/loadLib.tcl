# Provide a load function for a Starkit version of the package.

namespace eval ::DiffUtil {
    proc _load {dir libfile version} {
        if {$::tcl_platform(platform) == "windows"} {
            set root [file rootname $libfile]
            set root [string map {. {}} $root]
            set libfile $root[info sharedlibext]
        }
        if {[catch {uplevel \#0 [list load [file join $dir $libfile]]}]} {
            uplevel \#0 [list source [file join $dir tcl diffutil.tcl]]
        }
    }
}
