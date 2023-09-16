See wiring instructions in code.

The only bankswitching currently supported is F4, F6, F8, FA, FE and DPC.

Once you have a cartridge inserted successfully, you can send some commands to the device.
Simply write the command to a dummy file on the device. E.g., on Windows:
    echo command:reboot > e:\dummy.txt
Or on Linux:
    echo command:reboot > /mnt/drive
    
Here are the commands currently supported:
    command:hotplug 
        This is the default mode (can be changed in code by letting hotplug = false). Cartridges are only accepted if reliably
        read (two successive reads must have the same CRC-32), and can be hotplugged. This setting will be saved to flash and will survive
        unplugging the device.
    command:nohotplug
        Cartridge must be inserted into device prior to inserting into USB port, and can't be changed. But this has hope of
        reading unreliable cartridges. This setting will be saved to flash and will survive unplugging the device.
    command:force:ext
        Force the cartridge type to ext, where ext is one of the Stella extensions: 2k, 4k, f4, f4s, f6, f6s, f8, f8s, fa, fe and dpc.
        Will reset the connection.
    command:noforce
        Go back to autodetection mode.
    command:reboot
        Reset the device.

BUILDING

I always use Roger's libmaple-based core rather than the official STM32 core, because USBComposite only works with libmaple, and libmaple is also less bloated.

Here are two different sets of build instructions (the second is mine) how to set up Arduino for this core:

 1. https://www.instructables.com/Programming-STM32-Blue-Pill-Via-USB/

 2. https://www.instructables.com/Gamecube-Controller-USB-Adapter-and-Getting-Starte/ (see steps 1 and 2)

I fear something may be out of date in these instructions, and a lot of developers have jumped ship from the libmaple core to the official one. People keep asking me to make USBComposite compatible with the official core, but I don't have the energy to figure out the details of how to do that.
