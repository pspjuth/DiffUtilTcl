# Provide a load function for a Starkit version of the package.

namespace eval ::DiffUtil {
    proc _load {dir libfile version} {
        if {$::tcl_platform(platform) == "windows"} {
            set root [file rootname $libfile]
            set root [string map {. {}} $root]
            set libfile $root[info sharedlibext]
        }
        if {[catch {uplevel \#0 load [file join $dir $libfile]}]} {
            source [file join $dir diffutil.tcl]
        }
    }
}
