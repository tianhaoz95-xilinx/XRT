# View waveform.ila in hardware manager
#

if {$argc != 1} {
  set errmsg "ERROR: Usage: vivado_lab -tclargs <wave_file>"
  puts $errmsg
  error $errmsg
}

set wave_file  [lindex $argv 0]
open_hw
display_hw_ila_data [read_hw_ila_data $wave_file]

