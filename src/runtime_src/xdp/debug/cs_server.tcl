#
#  Use during auto-CS debug at run time- Vivado Lab sourcing procs and 
#  setup ILA and eval server
#
# TODO: Prior to release beautify this file for open source release
# TODO: Legal review of this Tcl file

proc Eval_Server {port {interp {}} {openCmd EvalOpenProc}} {
  socket -server [list EvalAccept $interp $openCmd] $port
}

proc EvalAccept { interp openCmd newsock addr port} {
  global eval
  set eval(cmdbuf,$newsock) {}
  fileevent $newsock readable [list EvalRead $newsock $interp]
  if [catch {
    interp eval $interp $openCmd $newsock $addr $port
    puts "Accept $newsock from $addr : $port"
  }] {
    close $newsock
  }
}

proc EvalOpenProc {sock addr port} {
  #authenticate here
}

proc EvalRead {sock interp} {
  global eval errorInfo errorCode
  if [eof $sock] {
    close $sock
  } else {
    gets $sock line
    append eval(cmdbuf,$sock) $line\n
    if {[string length $eval(cmdbuf,$sock)] && [info complete $eval(cmdbuf,$sock)]} {
      set code [catch {
        if {[string length $interp] == 0} {
          uplevel #0 $eval(cmdbuf,$sock)
        } else {
          interp eval $interp $eval(cmdbuf,$sock)
        }
      } result]
      set reply [list $code $result $errorInfo $errorCode]\n
      # use regsub to count newlines
      set lines [regsub -all \n $reply {} junk]
      # the reply is a line count followed by a tcl list that occupies that occupies number of lines
      puts $sock $lines
      puts -nonewline $sock $reply
      flush $sock
      set eval(cmdbuf,$sock) {}
    }
  }
}

proc Eval_Open {server port} {
  global eval
  set sock [socket $server $port]
  # save this info for later reporting
  set eval(server,$sock) $server:$port
  return $sock
}

proc Eval_Remote {sock args} {
  global eval
  #preserve the concat semantics of eval
  if {[llength $args] > 1} {
    set cmd [concat $args]
  } else {
    set cmd [lindex $args 0]
  }
  puts $sock $cmd
  flush $sock
  #read return line count and the result
  gets $sock lines
  set result {}
  while {$lines > 0} {
    gets $sock x
    append result $x\n
    incr lines -1
  }
  set code [lindex $result 0]
  set x [lindex $result 1]
  # cleanup the end of the stack
  regsub "\[^\n]+$" [lindex $result 2] \
    "*Remote Server $eval(server,$sock)*" stack
  set ec [lindex $result 3]
  return -code $code -errorinfo $stack -errorcode $ec $x
}

proc Eval_Close {sock} {
  close $sock
}

proc do_connect_target {host xvc_port hws_port} {
  connect_hw_server -url $host:$hws_port
  # suspicion of timing issue vs GUI mode
  open_hw_target -xvc_url $host:$xvc_port -quiet
  after 2000
  refresh_hw_target
}

proc do_detect_ilas {ltx_file} {
  refresh_hw_target
  set_property PROBES.FILE $ltx_file [get_hw_devices debug_bridge_0]
  set_property FULL_PROBES.FILE $ltx_file [get_hw_devices debug_bridge_0]
  refresh_hw_device [lindex [get_hw_devices debug_bridge_0] 0]
  foreach ila [get_hw_ilas -of_objects [get_hw_devices debug_bridge_0]] {
    run_hw_ila [get_hw_ilas $ila] -trigger_now
    wait_on_hw_ila [get_hw_ilas $ila]
    display_hw_ila_data [upload_hw_ila_data [get_hw_ilas $ila]]
  }
}

proc do_create_custom_ctrl_names {} {
  foreach ila [get_hw_ilas -of_objects [get_hw_devices debug_bridge_0]] {
    foreach ctrl_probe [get_hw_probes -of_objects [get_hw_ilas $ila] -filter {NAME=~"*_axi_ar_ctrl*" || NAME=~"*_axi_aw_ctrl*" || NAME=~"*_axi_r_ctrl*" || NAME=~"*_axi_w_ctrl*" || NAME=~"*_axi_b_ctrl*"}] {
      set slot_name [join [lrange [split [string tolower [get_property INTERFACE.NAME [get_hw_probes $ctrl_probe]]] "_"] 0 1] "_"]
      set interface_name [get_property INTERFACE.CONNECTED_BUS [get_hw_probes $ctrl_probe]]
      set signal_name [get_property INTERFACE.LOGICAL_PORT [get_hw_probes $ctrl_probe]]
      set_property NAME.CUSTOM "$slot_name : $interface_name : $signal_name" [get_hw_probes $ctrl_probe]
      set_property NAME.SELECT "Custom" [get_hw_probes $ctrl_probe]
      #      puts "custom name = $slot_name : $interface_name : $signal_name"
    }
  }
}

proc do_setup_ilas_any_txn_start {} {
  # TODO: Maybe allow an environment override of this filename?
  set trigger_file "cs_trigger.tcl"
  set apply_trigger_settings 1

  # Read the trigger file if it exists on disk. If there is any error
  # sourcing it, re-apply the trigger settings
  if {[file exists $trigger_file]} {
    if {! [catch {source $trigger_file} fid]} {
      set apply_trigger_settings 0
    } 
  }
  if {$apply_trigger_settings} {
    puts "Apply trigger settings for first time capture..."
    foreach ila [get_hw_ilas -of_objects [get_hw_devices debug_bridge_0]] {
      set is_system_ila 0
      foreach ar_ctrl [get_hw_probes -of_objects [get_hw_ilas $ila] -filter {NAME=~"*_axi_ar_ctrl*"}] {
        set_property TRIGGER_COMPARE_VALUE eq2'h3 [get_hw_probes $ar_ctrl]
        set is_system_ila 1
      }
      foreach aw_ctrl [get_hw_probes -of_objects [get_hw_ilas $ila] -filter {NAME=~"*_axi_aw_ctrl*"}] {
        set_property TRIGGER_COMPARE_VALUE eq2'h3 [get_hw_probes $aw_ctrl]
        set is_system_ila 1
      }
      if {$is_system_ila == 1} {
        set_property CONTROL.TRIGGER_CONDITION OR [get_hw_ilas $ila]
        set_property CONTROL.TRIGGER_POSITION 32 [get_hw_ilas $ila]
      }
    }
  }
}

proc do_open_or_create_project {project_name} {
  if {[catch {open_project $project_name} fid]} {
    create_project -force $project_name
  }
}

## MAIN ##

if {$argc != 5} {
  set errmsg "ERROR: incorrect number of command line arguments"
  puts $errmsg
  error $errmsg
}

set project_name  [file rootname [file tail [lindex $argv 0]]]
set ltx_file [lindex $argv 1]
set host     [lindex $argv 2]
set xvc_port [lindex $argv 3]
set hws_port [lindex $argv 4]

do_open_or_create_project $project_name
open_hw
do_connect_target $host $xvc_port $hws_port
do_detect_ilas $ltx_file
do_create_custom_ctrl_names
do_setup_ilas_any_txn_start

Eval_Server 2540
vwait forever
