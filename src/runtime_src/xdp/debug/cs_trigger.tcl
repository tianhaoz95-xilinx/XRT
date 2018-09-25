##############################################################################
#                    CHIPSCOPE USER TRIGGER SETTINGS FILE
##############################################################################
#
# If this file exists, trigger settings will be applied each iteration of
# the host. 
#
# The default trigger condition captures any AXI traffic
#
#
# TODO: Can we simplify this file or add some examples so it is easy
#       for a new user to modify the settings for various AXI trigger conditions?

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
