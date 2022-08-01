# TM1637-linux
communicate to a tm1637 display using numlock and caplock LED indicators directly through ```serio``` and   ```libps2```.
only try this if you dont mind not using your keyboard ;) 
## Execution
Unload the current module that drives the numlock and caplocks indicators
This can be found by listing the sysfs led driver:
```
$ ls /sys/class/leds/
input3::capslock  input3::numlock  input3::scrolllock  phy0-led
```
and then list the respective module by running the ```get_led_device_info.sh``` script.
get_led_device_info.sh
```
$ ./get_led_device_info.sh  /sys/class/leds/input3\:\:capslock
 
#####################################
# LED class device hardware details #
#####################################

bus:			input
product:		AT Translated Set 2 keyboard
driver:			AT and PS/2 keyboard driver
associated input node:	input3

####################################
# LED class device name validation #
####################################

devicename :	input3               [ OK ]     
function   :	capslock             [ OK ]     Matching definition: LED_FUNCTION_CAPSLOCK

```
Only continue if the underylying driver is ``` AT and PS/2 keyboard driver```.
Finally, unload the ```atkbd``` kernel module by running:
NOTE: This will render your keybord to not take in any inputs and its suggested to switch over to either SSH or connect an external USB keyboard. 
```
sudo rmmod -f atkbd
````
Next, clone the repo and compile the module.
```
git clone https://github.com/vimpop/tm1637-linux
make 
```
and then proceed to inject the module:
```
sudo insmod tm1637.ko
```
Now both capslock and numlock lights will be flickering which is an indication that the data is being transferred. 
