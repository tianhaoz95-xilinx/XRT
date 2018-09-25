#  Use during auto-CS debug at run time- Load "eval client" Tcl procs and open client-side socket connection
#
# TODO: Should we use this file or C++ify the client side before release? 
#
variable sock

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
  #puts "Command Result: $x"
  return -code $code -errorinfo $stack -errorcode $ec $x
}

proc Eval_Close {sock} {
  close $sock
}


# Report detailed MIG info and status
proc report_migs {host} {
    variable sock
    set sock [Eval_Open $host 2540]

    set result [Eval_Remote $sock {report_hw_mig [get_hw_migs] -return_string}]
    puts $result
}


# Find number of MIG cores
proc find_migs {host} {
    variable sock
    set sock [Eval_Open $host 2540]
    
    set migList {}
    set migList [Eval_Remote $sock {get_hw_migs}]
    set migCount [llength $migList]
    puts "Number of MIG found on device: $migCount"
}


# Test if socket connection is ready
proc ready {host} {
    variable sock
    set sock [Eval_Open $host 2540]
    
    puts "\nConnection is ready."
}


# Run ILA, wait for trigger and upload, write out ILA data
proc run_ila {host {outPath "capture"}} {
    variable sock
    set sock [Eval_Open $host 2540]
    
    Eval_Remote $sock {run_hw_ila [get_hw_ilas -of_objects [get_hw_devices debug_bridge_0]]}
    
    puts "System ILA has being armed."
}


# Capture ILA data and write out ILA data
proc capture_ila {host {outPath "capture"}} {
    variable sock
    set sock [Eval_Open $host 2540]
    
    puts "Hardware transactions are currently being processed..."
    
    Eval_Remote $sock {wait_on_hw_ila [get_hw_ilas -of_objects [get_hw_devices debug_bridge_0]]}
    Eval_Remote $sock {display_hw_ila_data [upload_hw_ila_data [get_hw_ilas -of_objects [get_hw_devices debug_bridge_0]]]}
    Eval_Remote $sock {write_hw_ila_data -force "waveform"}

    puts "ILA capture is written out as: waveform.ila"
}


# Find the target host, connect to it and refresh device
proc open {host} {
    variable sock
    set sock [Eval_Open $host 2540]

    #
    # Send commands to remote system to open the hardware manager, connect 
    # to the XVC, refresh device to detect debug cores on the debug bridge
    #
    Eval_Remote $sock {open_hw}
    Eval_Remote $sock {connect_hw_server}
    Eval_Remote $sock {open_hw_target -quiet -xvc_url localhost:10200}
    Eval_Remote $sock {refresh_hw_device [lindex [get_hw_devices debug_bridge_0] 0]}
}


# Send "exit" command remote to Vivado instance, then close 
# socket connection to remote server
#
# (NOTE: You will likely see a "bad completion code" error 
# on the last command since exiting Vivado will also close 
# the socket connection on the remote "server". You can 
# safely ignore this error and it is okay when things are in silent mode.)
#
proc close {host} {
    variable sock
    
    Eval_Remote $sock {exit}
    Eval_Close $sock
}
