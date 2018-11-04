# Device Management API

## Runtime Version

```python
xrt.core.hal.version()
```

This API will give the version number of the runtime. So far, it can only be either 1 or 2 because there are only two versions.

###### Arguments

* `[none]`

###### Return

* `version_number [int]`: the version number of the runtime, so far either 1 or 2

###### Example

```python
from xrt.core import hal

hal.version()
# this will give the version number of the runtime
```

## Probe Devices

```python
xrt.core.hal.probe()
```
This API is used to scan all the available devices on the system.

Note: devices without proper DSA are not counted as available devices.

###### Arguments

* `[none]`

###### Return

* `device_cnt [int]`: the number of devices connected to the system that is ready to be used

###### Example

```python
from xrt.core import hal

device_cnt = hal.probe()
# device_cnt will be the number of devices connected
```

## Open Device

```python
xrt.core.hal.open(device_index, device_name, verbosity_level)
```

This API will open the device to be used.

Note: this API will notify the driver of this process.

###### Arguments

* `device_index [int]`: the slot index of the target device

* `device_name [string]`: user defined unique ID for the target device which will be used to identify this device in other API, and for log filename which will be `[device_name].log`

* `verbosity_level [string]`: the level of information that will be put into log file which can be one of:

  * `"info"`
  * `"warning"`
  * `"error"`
  * `"quiet"`

###### Return

* `[none]`

###### Example

```python
from xrt.core import hal

device_cnt = hal.probe()
if device_cnt > 0:
 hal.open(0, "my_device", "info")
# from now on you can use my_device to manipulate the device at slot 0
```

## Close Device

```python
xrt.core.hal.close(device_name)
```

This API will close the device. As a good practice, this API should always get called whenever the program is done with the device.

###### Arguments

* `device_name [string]`: the unique ID of the device to close

###### Return

* `[none]`

###### Example

```python
from xrt.core import hal

device_cnt = hal.probe()
if device_cnt > 0:
 hal.open(0, "my_device", "info")
 hal.close("my_device")
 # now the device has been closed and not usable
```

## Reset Device

```python
xrt.core.hal.reset(device_name, reset_type)
```

This API is used to reset the device to initial state. Commonly used to reset firewalls.

###### Arguments

* `device_name [string]`: the unique device ID given when open the device

* `reset_type [string]`: the type of reset which can be one of:
  * `"kernel"`: only reset the kernel
  * `"full"`: reset the entire board

###### Return

* `[none]`

###### Example

```python
from xrt.core import hal

device_cnt = hal.probe()
if device_cnt > 0:
 hal.open(0, "my_device", "info")
 hal.reset("my_device", "full")
 # now the device status has been reset
```

## Lock Device

```python
xrt.core.hal.lock(deivce_name)
```

This API is used to lock the device to perform critical operations like downloading the bitstream.

###### Arguments

* `device_name [string]`: the unique ID of the device given in open

###### Return

* `[none]`

###### Example

```python
from xrt.core import hal

device_cnt = hal.probe()
if device_cnt > 0:
 hal.open(0, "my_device", "info")
 hal.lock("my_device")
 # now the device has been locked and
 # no one else can access this device
```

## Unlock Device

```python
xrt.core.hal.unlock(device_name)
```

This API is used to unlock the device after finishing critical operations so that other people can access the device.

###### Arguments

* `device_name [string]`: the unique ID of the device given in `open`

###### Return

* `[none]`

###### Example

```python
from xrt.core import hal

device_cnt = hal.probe()
if device_cnt > 0:
 hal.open(0, "my_device", "info")
 hal.lock("my_device")
 # now the device has been locked and
 # no one else can access this device
 hal.unlock("my_device")
 # now the device has been unlocked and
 # other can access this device again
```

## Reclock Device [Not Tested!]

```python
xrt.core.hal.reclock(device_name, target_region, target_freq)
```

This API is used to force the device to reclock. I personally have never tried it, but you are more than welcome to be the first one to give it a go lol.

###### Arguments

* `device_name [string]`: the unique ID of the device given in `open`

* `target_reigon [int]`: the programmable region to reclock which should always be 0 for now

* `target_freq [int]`: the target frequency in MHz

###### Return

* `[none]`

###### Example

```python
from xrt.core import hal

device_cnt = hal.probe()
if device_cnt > 0:
 hal.open(0, "my_device", "info")
 hal.reclock("my_device", 0, 120)
 # now the device has been reclocked to 120 MHz
```

## Get Device Information

```python
xrt.core.hal.info(device_name)
```

This API is used to get device information including DSA name, device version, vendor name, power consumption, and memory configuration, etc.

###### Arguments

* `device_name [string]`: the device name used to open the device

###### Return

* `device_info [dict]`: the dict that contains all the device information

###### Example

```python
from xrt.core import hal

device_cnt = hal.probe()
if device_cnt > 0:
  hal.open(0, "my_device", "info")
  hal.info("my_device")
  # you should see all the device information here
```

## Get Device Usage

```python
xrt.core.hal.usage(device_name)
```
This API is used to query the device usage information including memory usage, and bandwidth usage.

###### Arguments

* `device_name [string]`: the device name used to open the device

###### Return

* `device_usage [dict]`: the dict that contains all the device usage information

###### Example

```python
from xrt.core import hal

device_cnt = hal.probe()
if device_cnt > 0:
  hal.open(0, "my_device", "info")
  hal.usage("my_device")
  # you should see all the device usage here
```

## Get Device Error

```python
xrt.core.hal.error(device_name);
```

This API is used to query hardware error from on-device registers.

Note: this API will still give valuable information even the other host code freezes.

###### Arguments

* `device_name [string]`: the device name used to open the device

###### Return

* `device_error [dict]`: the dict that contains all the device error information

###### Example

```python
from xrt.core import hal

device_cnt = hal.probe()
if device_cnt > 0:
  hal.open(0, "my_device", "info")
  hal.error("my_device")
  # you should see all the device error here
```
